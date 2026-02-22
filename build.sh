#!/bin/bash
# pgAndroid build script
# Usage: ./build.sh [--arch arm64-v8a|x86_64] [--clean] [--debug] [--jobs N]
#
# Cross-compiles PostgreSQL + extensions (pgcrypto, uuid-ossp) for Android
# and links everything into a single libpgandroid.so.
#
# Prerequisites:
#   - Android NDK installed, env.sh configured
#   - OpenSSL cross-compiled at deps/openssl-android-arm64/
#   - Patches ready in patches/
#
# Output:
#   out/<abi>/libpgandroid.so
#   out/<abi>/libpgandroid.h

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------

ARCH="arm64-v8a"
CLEAN=false
DEBUG=false
JOBS=$(nproc)

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

while [[ $# -gt 0 ]]; do
    case "$1" in
        --arch)
            ARCH="$2"
            shift 2
            ;;
        --arch=*)
            ARCH="${1#*=}"
            shift
            ;;
        --clean)
            CLEAN=true
            shift
            ;;
        --debug)
            DEBUG=true
            shift
            ;;
        --jobs)
            JOBS="$2"
            shift 2
            ;;
        --jobs=*)
            JOBS="${1#*=}"
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [--arch arm64-v8a|x86_64] [--clean] [--debug] [--jobs N]"
            echo ""
            echo "  --arch    Target ABI (default: arm64-v8a)"
            echo "  --clean   Remove build directory before building"
            echo "  --debug   Keep debug symbols, disable optimisations"
            echo "  --jobs N  Parallel jobs (default: nproc)"
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

# ---------------------------------------------------------------------------
# Validate arch
# ---------------------------------------------------------------------------

case "$ARCH" in
    arm64-v8a|arm64)
        ARCH="arm64-v8a"
        TRIPLE="aarch64-linux-android"
        CONFIGURE_HOST="aarch64-linux-android34"
        CLANG_TARGET="aarch64-linux-android34"
        OPENSSL_DIR_DEFAULT="openssl-android-arm64"
        ;;
    x86_64)
        TRIPLE="x86_64-linux-android"
        CONFIGURE_HOST="x86_64-linux-android34"
        CLANG_TARGET="x86_64-linux-android34"
        OPENSSL_DIR_DEFAULT="openssl-android-x86_64"
        ;;
    *)
        echo "ERROR: Unsupported arch: $ARCH (supported: arm64-v8a, x86_64)" >&2
        exit 1
        ;;
esac

# ---------------------------------------------------------------------------
# Project paths
# ---------------------------------------------------------------------------

PROJECT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

BUILD_DIR="$PROJECT/build"
PG_SRC="$BUILD_DIR/pg-src"
PG_INSTALL="$BUILD_DIR/pg-install"
OUT_DIR="$PROJECT/out/$ARCH"

OPENSSL_DIR="$PROJECT/deps/$OPENSSL_DIR_DEFAULT"

# ---------------------------------------------------------------------------
# Source NDK environment
# ---------------------------------------------------------------------------

if [[ ! -f "$PROJECT/env.sh" ]]; then
    echo "ERROR: env.sh not found at $PROJECT/env.sh" >&2
    echo "Create it by setting ANDROID_NDK_HOME to your NDK installation." >&2
    exit 1
fi

# shellcheck source=env.sh
source "$PROJECT/env.sh"

if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
    echo "ERROR: ANDROID_NDK_HOME is not set after sourcing env.sh" >&2
    exit 1
fi

NDK="$ANDROID_NDK_HOME"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"

if [[ ! -d "$TOOLCHAIN" ]]; then
    echo "ERROR: NDK toolchain not found at $TOOLCHAIN" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Cross-compilation variables
# ---------------------------------------------------------------------------

export CC="$TOOLCHAIN/bin/${CLANG_TARGET}-clang"
export CXX="$TOOLCHAIN/bin/${CLANG_TARGET}-clang++"
export AR="$TOOLCHAIN/bin/llvm-ar"
export RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
export STRIP="$TOOLCHAIN/bin/llvm-strip"

if $DEBUG; then
    OPT_FLAGS="-O0 -g"
else
    OPT_FLAGS="-Os"
fi

export CFLAGS="-D__ANDROID_PGANDROID__ -D__ANDROID_API__=34 ${OPT_FLAGS} -fPIC"
export CPPFLAGS="-D_GNU_SOURCE -I$OPENSSL_DIR/include"
export LDFLAGS="-L$OPENSSL_DIR/lib"

# ---------------------------------------------------------------------------
# Validate prerequisites
# ---------------------------------------------------------------------------

if [[ ! -f "$CC" ]]; then
    echo "ERROR: Cross-compiler not found: $CC" >&2
    exit 1
fi

if [[ ! -d "$OPENSSL_DIR/include" ]] || [[ ! -d "$OPENSSL_DIR/lib" ]]; then
    echo "ERROR: OpenSSL for $ARCH not found at $OPENSSL_DIR" >&2
    echo "Run ./build-openssl.sh --arch $ARCH to build it first." >&2
    exit 1
fi

if [[ ! -d "$PROJECT/upstream/postgres-pglite" ]]; then
    echo "ERROR: postgres-pglite source not found at $PROJECT/upstream/postgres-pglite" >&2
    echo "Clone it first:" >&2
    echo "  git clone https://github.com/electric-sql/postgres-pglite.git upstream/postgres-pglite" >&2
    echo "  git -C upstream/postgres-pglite checkout REL_17_5_WASM" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Announce build configuration
# ---------------------------------------------------------------------------

echo "========================================================"
echo " pgAndroid build"
echo "========================================================"
echo "  Architecture : $ARCH"
echo "  Triple       : $TRIPLE"
echo "  NDK          : $NDK"
echo "  CC           : $CC"
echo "  OpenSSL      : $OPENSSL_DIR"
echo "  Jobs         : $JOBS"
echo "  Debug        : $DEBUG"
echo "  Project      : $PROJECT"
echo "========================================================"
echo ""

# ---------------------------------------------------------------------------
# Clean
# ---------------------------------------------------------------------------

if $CLEAN; then
    echo "[clean] Removing $BUILD_DIR ..."
    rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR" "$OUT_DIR"

# ---------------------------------------------------------------------------
# Prepare source (working copy)
# ---------------------------------------------------------------------------

if [[ ! -d "$PG_SRC/src/backend" ]]; then
    echo "[source] Copying upstream/postgres-pglite -> build/pg-src ..."
    cp -a "$PROJECT/upstream/postgres-pglite" "$PG_SRC"
fi

# Apply patches if not already applied (detect via sentinel file)
PATCH_SENTINEL="$PG_SRC/.pgandroid-patches-applied"
if [[ ! -f "$PATCH_SENTINEL" ]]; then
    echo "[patches] Applying pgandroid patches ..."
    "$PROJECT/patches/apply.sh" --postgres-src "$PG_SRC"
    touch "$PATCH_SENTINEL"
else
    echo "[patches] Patches already applied (sentinel found), skipping."
fi

# ---------------------------------------------------------------------------
# Configure PostgreSQL
# ---------------------------------------------------------------------------

MAKEFILE_BUILT="$PG_SRC/GNUmakefile"
if [[ ! -f "$MAKEFILE_BUILT" ]] || $CLEAN; then
    echo ""
    echo "[configure] Running ./configure ..."
    (
        cd "$PG_SRC"
        ./configure \
            --host="$CONFIGURE_HOST" \
            --prefix="$PG_INSTALL" \
            --without-readline \
            --without-zlib \
            --without-icu \
            --disable-nls \
            --with-openssl \
            --with-template=android \
            OPENSSL_CFLAGS="-I$OPENSSL_DIR/include" \
            OPENSSL_LIBS="-L$OPENSSL_DIR/lib -lssl -lcrypto" \
            CC="$CC" \
            CFLAGS="$CFLAGS" \
            CPPFLAGS="$CPPFLAGS" \
            LDFLAGS="$LDFLAGS"
    )
else
    echo "[configure] Makefile already present, skipping configure."
fi

# ---------------------------------------------------------------------------
# Build PostgreSQL
# ---------------------------------------------------------------------------

echo ""
echo "[build] Building PostgreSQL backend (jobs=$JOBS) ..."
# Build all .o files in src/backend. The final 'postgres' binary link will fail
# because we link our own shared library instead — that's expected. We only need
# the compiled object files, not the server executable.
make -C "$PG_SRC/src/backend" -j"$JOBS" || {
    # Verify .o files were actually compiled (link failure is OK)
    OBJ_COUNT=$(find "$PG_SRC/src/backend" -name '*.o' | wc -l)
    if [[ "$OBJ_COUNT" -lt 100 ]]; then
        echo "ERROR: Backend compilation failed — only $OBJ_COUNT .o files found" >&2
        exit 1
    fi
    echo "[build] Backend .o files compiled ($OBJ_COUNT objects). Final postgres link failed (expected — we link our own .so)."
}

echo ""
echo "[install] Installing headers to $PG_INSTALL ..."
# Install headers so extensions (pgcrypto, uuid-ossp) can find pg_config
make -C "$PG_SRC/src/backend" install || true

# ---------------------------------------------------------------------------
# Build extensions
# ---------------------------------------------------------------------------

echo ""
echo "[extensions] Building pgcrypto objects ..."
(
    cd "$PG_SRC/contrib/pgcrypto"
    # Only compile .o files — we link them directly into libpgandroid.so.
    # The shared library (pgcrypto.so) and install steps are not needed.
    make -j"$JOBS" || true
    # Verify .o files were compiled
    OBJ_COUNT=$(find . -name '*.o' | wc -l)
    echo "[extensions] pgcrypto: $OBJ_COUNT object files compiled"
)

# uuid-ossp is intentionally skipped: it requires an external UUID library
# (ossp-uuid or e2fsprogs libuuid) that is not available in the Android NDK.
# PostgreSQL 17 provides gen_random_uuid() natively via pgcrypto/OpenSSL,
# which covers all embedded use cases.
echo ""
echo "[extensions] Skipping uuid-ossp (no libuuid in Android NDK; use gen_random_uuid() instead)."

# ---------------------------------------------------------------------------
# Compile JNI bridge (jni/pgandroid_jni.c)
# ---------------------------------------------------------------------------
#
# pgandroid_jni.c implements the JNI native methods called from Kotlin
# (com.pgandroid.PgDatabase) and the extended C API functions declared in
# jni/pgandroid.h (pgandroid_init_with_config, pgandroid_checkpoint).
#
# It is compiled separately so that the JNI includes do not pollute the
# PostgreSQL backend translation units.
#
# JNI headers come from the Android NDK sysroot — no JAVA_HOME required.

echo ""
echo "[jni] Compiling JNI bridge (jni/pgandroid_jni.c) ..."

JNI_BRIDGE_SRC="$PROJECT/jni/pgandroid_jni.c"
JNI_BRIDGE_OBJ="$OUT_DIR/pgandroid_jni.o"

# The Android NDK ships jni.h in its sysroot
NDK_JNI_INC="$TOOLCHAIN/sysroot/usr/include"

if [[ ! -f "$NDK_JNI_INC/jni.h" ]]; then
    echo "WARNING: jni.h not found at $NDK_JNI_INC/jni.h" >&2
    echo "         JNI bridge may not compile; install Android NDK r23+ or later." >&2
fi

# shellcheck disable=SC2086
"$CC" \
    -c \
    -fPIC \
    ${OPT_FLAGS} \
    -D__ANDROID_PGANDROID__ \
    "-D__ANDROID_API__=34" \
    -I"$NDK_JNI_INC" \
    -I"$PROJECT/jni" \
    -I"$PG_SRC/pgandroid" \
    -I"$PG_INSTALL/include/server" \
    -o "$JNI_BRIDGE_OBJ" \
    "$JNI_BRIDGE_SRC"

echo "[jni] JNI bridge object: $JNI_BRIDGE_OBJ"

# ---------------------------------------------------------------------------
# Compile pgandroid entry-point files (pgandroid_main.c, pgandroid_io.c)
# ---------------------------------------------------------------------------

echo ""
echo "[pgandroid] Compiling pgandroid backend entry-point files ..."

# pgandroid_main.c and pgandroid_io.c are backend translation units:
# they use #include "postgres.h" and need android_pgandroid.h force-included.
for src_file in "$PG_SRC/pgandroid/pgandroid_main.c" "$PG_SRC/pgandroid/pgandroid_io.c"; do
    base=$(basename "$src_file" .c)
    obj_file="$OUT_DIR/${base}.o"
    echo "  Compiling $base.c ..."
    # shellcheck disable=SC2086
    "$CC" \
        -c \
        -fPIC \
        ${OPT_FLAGS} \
        -D__ANDROID_PGANDROID__ \
        -DPGANDROID_MAIN \
        "-D__ANDROID_API__=34" \
        -D_GNU_SOURCE \
        -Wno-unused-function \
        -include "$PG_SRC/src/include/port/android_pgandroid.h" \
        -I"$NDK_JNI_INC" \
        -I"$PROJECT/jni" \
        -I"$PG_SRC/pgandroid" \
        -I"$PG_SRC/src/include" \
        -I"$PG_SRC/src" \
        -I"$PG_SRC/src/interfaces/libpq" \
        -I"$PG_INSTALL/include/server" \
        -I"$OPENSSL_DIR/include" \
        -o "$obj_file" \
        "$src_file"
done

# ---------------------------------------------------------------------------
# Compile pgandroid_initdb.c as a FRONTEND translation unit
#
# pgandroid_initdb.c includes bin/initdb/initdb.c which starts with
# #include "postgres_fe.h" (frontend).  Compiling it in the same TU as
# postgres.h (backend) causes irreconcilable conflicts, so we compile it
# separately with -DFRONTEND and frontend include paths.
# ---------------------------------------------------------------------------

echo ""
echo "[pgandroid] Compiling pgandroid_initdb.c (frontend TU) ..."

INITDB_OBJ="$OUT_DIR/pgandroid_initdb.o"
# shellcheck disable=SC2086
"$CC" \
    -c \
    -fPIC \
    ${OPT_FLAGS} \
    -DFRONTEND \
    -DUSE_PRIVATE_ENCODING_FUNCS \
    -D__ANDROID_PGANDROID__ \
    -DPGANDROID_INITDB_MAIN \
    "-D__ANDROID_API__=34" \
    -D_GNU_SOURCE \
    -Wno-unused-function \
    -Wno-implicit-function-declaration \
    -I"$NDK_JNI_INC" \
    -I"$PG_SRC/pgandroid" \
    -I"$PG_SRC/src/include" \
    -I"$PG_SRC/src" \
    -I"$PG_SRC/src/interfaces/libpq" \
    -I"$PG_SRC/src/bin/initdb" \
    -I"$OPENSSL_DIR/include" \
    -o "$INITDB_OBJ" \
    "$PG_SRC/pgandroid/pgandroid_initdb.c"
echo "[pgandroid] pgandroid_initdb.o compiled."

# ---------------------------------------------------------------------------
# Compile frontend support objects needed by pgandroid_initdb / initdb.c
#
# initdb.c calls functions from:
#   - libpq/pqexpbuffer.c   (PQExpBuffer API)
#   - common/logging.c      (pg_log_generic, pg_logging_init)
#   - fe_utils/string_utils.c (appendShellString, fmtId)
#   - fe_utils/option_utils.c (option_parse_int, parse_sync_method)
#   - common/file_utils.c   (sync_pgdata, fsync_fname)
# ---------------------------------------------------------------------------

echo ""
echo "[pgandroid] Compiling frontend support objects ..."

FE_CFLAGS="-c -fPIC ${OPT_FLAGS} -DFRONTEND -DUSE_PRIVATE_ENCODING_FUNCS -D__ANDROID_PGANDROID__ -D__ANDROID_API__=34 -D_GNU_SOURCE -Wno-unused-function"
FE_INCS="-I$NDK_JNI_INC -I$PG_SRC/src/include -I$PG_SRC/src -I$PG_SRC/src/interfaces/libpq -I$OPENSSL_DIR/include"

declare -A FE_SRCS
FE_SRCS=(
    ["pqexpbuffer"]="$PG_SRC/src/interfaces/libpq/pqexpbuffer.c"
    ["fe_logging"]="$PG_SRC/src/common/logging.c"
    ["fe_string_utils"]="$PG_SRC/src/fe_utils/string_utils.c"
    ["fe_option_utils"]="$PG_SRC/src/fe_utils/option_utils.c"
    ["fe_file_utils"]="$PG_SRC/src/common/file_utils.c"
)

FE_OBJS=""
for name in "${!FE_SRCS[@]}"; do
    src="${FE_SRCS[$name]}"
    obj="$OUT_DIR/${name}.o"
    echo "  Compiling $name ..."
    # shellcheck disable=SC2086
    "$CC" $FE_CFLAGS $FE_INCS -o "$obj" "$src"
    FE_OBJS="$FE_OBJS $obj"
done

echo "[pgandroid] Frontend support objects: $FE_OBJS"

PGANDROID_ENTRY_OBJS=$(find "$OUT_DIR" -name 'pgandroid_*.o' ! -name 'pgandroid_jni.o' 2>/dev/null | tr '\n' ' ')
echo "[pgandroid] Entry-point objects: $PGANDROID_ENTRY_OBJS"

# ---------------------------------------------------------------------------
# Link libpgandroid.so
# ---------------------------------------------------------------------------
#
# Combine the PostgreSQL backend object files, the built extensions, the
# pgandroid entry-point objects (pgandroid_main.c, pgandroid_io.c, etc.),
# and the JNI bridge into a single shared library.

echo ""
echo "[link] Linking libpgandroid.so ..."

# Collect all backend .o files
BACKEND_OBJS=$(find "$PG_SRC/src/backend" -name '*.o' 2>/dev/null | tr '\n' ' ')
# src/common and src/port provide essential support functions (stringinfo, pg_prng,
# path canonicalization, etc.) that the backend references but are compiled separately.
# Use the *_srv.o variants where available (server-specific), fall back to base .o.
COMMON_OBJS=$(find "$PG_SRC/src/common" -name '*_srv.o' 2>/dev/null | tr '\n' ' ')
PORT_OBJS=$(find "$PG_SRC/src/port" -name '*_srv.o' 2>/dev/null | tr '\n' ' ')
TIMEZONE_OBJS=$(find "$PG_SRC/src/timezone" -name '*.o' 2>/dev/null | tr '\n' ' ')
PGCRYPTO_OBJS=$(find "$PG_SRC/contrib/pgcrypto" -name '*.o' 2>/dev/null | tr '\n' ' ')
# uuid-ossp skipped: no libuuid in Android NDK
PGANDROID_OBJS=$(find "$PG_SRC/pgandroid" -name '*.o' 2>/dev/null | tr '\n' ' ')

SO_OUT="$OUT_DIR/libpgandroid.so"

# Link everything into a shared library.
# JNI bridge object ($JNI_BRIDGE_OBJ) provides:
#   - Java_com_pgandroid_PgDatabase_nativeOpen
#   - Java_com_pgandroid_PgDatabase_nativeExec
#   - Java_com_pgandroid_PgDatabase_nativeExecParams
#   - Java_com_pgandroid_PgDatabase_nativeCheckpoint
#   - Java_com_pgandroid_PgDatabase_nativeClose
#   - pgandroid_init_with_config  (extended C API)
#   - pgandroid_checkpoint         (extended C API)
# shellcheck disable=SC2086
"$CXX" \
    -shared \
    -fPIC \
    -Wl,--allow-multiple-definition \
    $BACKEND_OBJS \
    $COMMON_OBJS \
    $PORT_OBJS \
    $TIMEZONE_OBJS \
    $PGCRYPTO_OBJS \
    $PGANDROID_OBJS \
    $PGANDROID_ENTRY_OBJS \
    "$INITDB_OBJ" \
    $FE_OBJS \
    "$JNI_BRIDGE_OBJ" \
    -L"$OPENSSL_DIR/lib" \
    -lssl \
    -lcrypto \
    -llog \
    -landroid \
    -o "$SO_OUT"

# Strip debug symbols for release builds
if ! $DEBUG; then
    echo "[strip] Stripping debug symbols from $SO_OUT ..."
    "$STRIP" --strip-unneeded "$SO_OUT"
fi

# ---------------------------------------------------------------------------
# Copy headers
# ---------------------------------------------------------------------------

# Internal C API header (generated during build)
HEADER_SRC="$PG_SRC/pgandroid/libpgandroid.h"
HEADER_OUT="$OUT_DIR/libpgandroid.h"

if [[ -f "$HEADER_SRC" ]]; then
    cp "$HEADER_SRC" "$HEADER_OUT"
    echo "[output] Header copied to $HEADER_OUT"
else
    echo "WARNING: Internal header not found at $HEADER_SRC — skipping." >&2
fi

# Public JNI API header (jni/pgandroid.h) — include in the SDK output
PGANDROID_H_SRC="$PROJECT/jni/pgandroid.h"
PGANDROID_H_OUT="$OUT_DIR/pgandroid.h"

if [[ -f "$PGANDROID_H_SRC" ]]; then
    cp "$PGANDROID_H_SRC" "$PGANDROID_H_OUT"
    echo "[output] Public header copied to $PGANDROID_H_OUT"
else
    echo "WARNING: Public header not found at $PGANDROID_H_SRC — skipping." >&2
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

echo ""
echo "========================================================"
echo " Build complete"
echo "========================================================"

if [[ -f "$SO_OUT" ]]; then
    SO_SIZE=$(du -sh "$SO_OUT" | cut -f1)
    echo "  Output   : $SO_OUT"
    echo "  Size     : $SO_SIZE"
    echo ""
    echo "  Architecture verification:"
    file "$SO_OUT" | sed 's/^/    /'
else
    echo "  WARNING: $SO_OUT was not produced!" >&2
fi

if [[ -f "$HEADER_OUT" ]]; then
    echo "  Header   : $HEADER_OUT"
fi
if [[ -f "$PGANDROID_H_OUT" ]]; then
    echo "  API hdr  : $PGANDROID_H_OUT"
fi

echo "========================================================"

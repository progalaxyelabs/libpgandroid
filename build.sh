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
export CPPFLAGS="-I$OPENSSL_DIR/include"
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
            --disable-nls \
            --with-openssl \
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
echo "[build] Building PostgreSQL (jobs=$JOBS) ..."
make -C "$PG_SRC" -j"$JOBS"

echo ""
echo "[install] Installing to $PG_INSTALL ..."
make -C "$PG_SRC" install

# ---------------------------------------------------------------------------
# Build extensions
# ---------------------------------------------------------------------------

echo ""
echo "[extensions] Building pgcrypto ..."
(
    cd "$PG_SRC/contrib/pgcrypto"
    make -j"$JOBS"
    make install
)

echo ""
echo "[extensions] Building uuid-ossp ..."
(
    cd "$PG_SRC/contrib/uuid-ossp"
    make -j"$JOBS"
    make install
)

# ---------------------------------------------------------------------------
# Link libpgandroid.so
# ---------------------------------------------------------------------------
#
# Combine the PostgreSQL backend object files, the built extensions, and the
# JNI bridge (pgandroid entry points) into a single shared library.
# The JNI bridge sources are expected at $PG_SRC/pgandroid/ (copied there by
# patches/apply.sh).

echo ""
echo "[link] Linking libpgandroid.so ..."

# Collect all backend .o files
BACKEND_OBJS=$(find "$PG_SRC/src/backend" -name '*.o' 2>/dev/null | tr '\n' ' ')
PGCRYPTO_OBJS=$(find "$PG_SRC/contrib/pgcrypto" -name '*.o' 2>/dev/null | tr '\n' ' ')
UUIDOSSP_OBJS=$(find "$PG_SRC/contrib/uuid-ossp" -name '*.o' 2>/dev/null | tr '\n' ' ')
JNI_OBJS=$(find "$PG_SRC/pgandroid" -name '*.o' 2>/dev/null | tr '\n' ' ')

SO_OUT="$OUT_DIR/libpgandroid.so"

# Link everything into a shared library
# shellcheck disable=SC2086
"$CXX" \
    -shared \
    -fPIC \
    -Wl,--allow-multiple-definition \
    $BACKEND_OBJS \
    $PGCRYPTO_OBJS \
    $UUIDOSSP_OBJS \
    $JNI_OBJS \
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
# Copy header
# ---------------------------------------------------------------------------

HEADER_SRC="$PG_SRC/pgandroid/libpgandroid.h"
HEADER_OUT="$OUT_DIR/libpgandroid.h"

if [[ -f "$HEADER_SRC" ]]; then
    cp "$HEADER_SRC" "$HEADER_OUT"
    echo "[output] Header copied to $HEADER_OUT"
else
    echo "WARNING: JNI header not found at $HEADER_SRC — skipping header copy." >&2
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

echo "========================================================"

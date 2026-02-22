#!/usr/bin/env bash
# patches/apply.sh
#
# Apply all pgandroid patches to a clean checkout of postgres-pglite.
#
# Usage:
#   ./patches/apply.sh [--postgres-src /path/to/postgres-pglite]
#
# Prerequisites:
#   - git (for applying patches)
#   - postgres-pglite cloned and on REL_17_5_WASM branch
#
# The script applies patches in numbered order (001, 002, 003, 004).
# Each patch is applied using 'git am' (if the checkout is a git repo)
# or 'patch -p1' (fallback).
#
# After applying:
#   cd /path/to/postgres-pglite
#   ./configure --host=aarch64-linux-android [...]
#   make -f src/makefiles/Makefile.android all

set -euo pipefail

# -----------------------------------------------------------------------
# Configuration
# -----------------------------------------------------------------------

# Directory containing this script
PATCHES_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Default postgres-pglite source directory (upstream/postgres-pglite relative
# to the pgandroid project root, one level above patches/)
PGANDROID_ROOT="$(dirname "$PATCHES_DIR")"
POSTGRES_SRC="${PGANDROID_ROOT}/upstream/postgres-pglite"

# Parse --postgres-src argument
while [[ $# -gt 0 ]]; do
    case "$1" in
        --postgres-src)
            POSTGRES_SRC="$2"
            shift 2
            ;;
        --postgres-src=*)
            POSTGRES_SRC="${1#*=}"
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [--postgres-src /path/to/postgres-pglite]"
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

# -----------------------------------------------------------------------
# Validation
# -----------------------------------------------------------------------

if [[ ! -d "$POSTGRES_SRC" ]]; then
    echo "ERROR: postgres-pglite source not found at: $POSTGRES_SRC" >&2
    echo "Clone it first:" >&2
    echo "  git clone https://github.com/electric-sql/postgres-pglite.git $POSTGRES_SRC" >&2
    echo "  git -C $POSTGRES_SRC checkout REL_17_5_WASM" >&2
    exit 1
fi

if [[ ! -f "$POSTGRES_SRC/src/backend/Makefile" ]]; then
    echo "ERROR: $POSTGRES_SRC does not look like a PostgreSQL source tree" >&2
    exit 1
fi

# Check that we are on REL_17_5_WASM (the expected base branch)
if git -C "$POSTGRES_SRC" rev-parse --git-dir >/dev/null 2>&1; then
    CURRENT_BRANCH=$(git -C "$POSTGRES_SRC" symbolic-ref --short HEAD 2>/dev/null || echo "DETACHED")
    if [[ "$CURRENT_BRANCH" != "REL_17_5_WASM" ]]; then
        echo "WARNING: Expected branch REL_17_5_WASM, but found: $CURRENT_BRANCH" >&2
        echo "Patches may not apply cleanly on a different branch." >&2
        echo "Continue anyway? [y/N]" >&2
        read -r reply
        if [[ "$reply" != [Yy] ]]; then
            exit 1
        fi
    fi
fi

# -----------------------------------------------------------------------
# Patch list (in application order)
# -----------------------------------------------------------------------

PATCHES=(
    "001-android-platform.patch"
    "002-android-io.patch"
    "003-android-filesystem.patch"
    "004-android-build.patch"
)

# -----------------------------------------------------------------------
# Apply patches
# -----------------------------------------------------------------------

IS_GIT_REPO=false
if git -C "$POSTGRES_SRC" rev-parse --git-dir >/dev/null 2>&1; then
    IS_GIT_REPO=true
fi

echo "Applying pgandroid patches to: $POSTGRES_SRC"
echo ""

applied=0
failed=0

for patch_name in "${PATCHES[@]}"; do
    patch_file="$PATCHES_DIR/$patch_name"

    if [[ ! -f "$patch_file" ]]; then
        echo "  SKIP  $patch_name (file not found)"
        continue
    fi

    echo -n "  APPLY $patch_name ... "

    if $IS_GIT_REPO; then
        # Try git am first (preserves commit metadata)
        if git -C "$POSTGRES_SRC" am \
               --whitespace=fix \
               --reject \
               "$patch_file" 2>/dev/null; then
            echo "OK (git am)"
            ((applied++))
        else
            # git am failed — abort and try patch -p1
            git -C "$POSTGRES_SRC" am --abort 2>/dev/null || true
            if patch -d "$POSTGRES_SRC" -p1 --forward < "$patch_file"; then
                echo "OK (patch -p1)"
                ((applied++))
            else
                echo "FAILED"
                ((failed++))
            fi
        fi
    else
        # Non-git directory: use patch -p1
        if patch -d "$POSTGRES_SRC" -p1 --forward < "$patch_file"; then
            echo "OK (patch -p1)"
            ((applied++))
        else
            echo "FAILED"
            ((failed++))
        fi
    fi
done

echo ""
echo "Summary: $applied applied, $failed failed"

if [[ $failed -gt 0 ]]; then
    echo ""
    echo "Some patches failed. Check .rej files in $POSTGRES_SRC for rejected hunks."
    echo "You may need to apply them manually."
    exit 1
fi

# -----------------------------------------------------------------------
# Post-apply: copy pgandroid entry-point files to the source tree
# -----------------------------------------------------------------------

# The pgandroid/ directory in the pgandroid repo contains the JNI entry points
# (pgandroid_main.c, pgandroid_io.c, etc.) which are NOT part of the
# postgres-pglite source tree (they are pgandroid-specific additions, not patches).
# They are referenced by the patches but do not exist in postgres-pglite.
#
# We copy them to a pgandroid/ subdirectory in the postgres-pglite source tree
# so that the build system can find them.

PGANDROID_ENTRY_DIR="$PGANDROID_ROOT/pgandroid"

if [[ -d "$PGANDROID_ENTRY_DIR" ]]; then
    echo ""
    echo "Copying pgandroid entry-point files..."
    mkdir -p "$POSTGRES_SRC/pgandroid"
    cp -r "$PGANDROID_ENTRY_DIR/"* "$POSTGRES_SRC/pgandroid/"
    echo "  Copied to: $POSTGRES_SRC/pgandroid/"
fi

# -----------------------------------------------------------------------
# Next steps
# -----------------------------------------------------------------------

echo ""
echo "========================================================"
echo " pgandroid patches applied successfully!"
echo "========================================================"
echo ""
echo "Next steps:"
echo ""
echo "Option A — CMake (recommended for production builds):"
echo "  cmake -DCMAKE_TOOLCHAIN_FILE=\$NDK/build/cmake/android.toolchain.cmake \\"
echo "        -DANDROID_ABI=arm64-v8a \\"
echo "        -DANDROID_PLATFORM=android-35 \\"
echo "        -DPGANDROID_POSTGRES_SRC=$POSTGRES_SRC \\"
echo "        -B build-android-arm64 \\"
echo "        $PGANDROID_ROOT"
echo "  cmake --build build-android-arm64"
echo ""
echo "Option B — configure + make (for development/testing):"
echo "  cd $POSTGRES_SRC"
echo "  ./configure \\"
echo "      --host=aarch64-linux-android \\"
echo "      --without-readline \\"
echo "      --without-zlib \\"
echo "      CC=\$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android35-clang \\"
echo "      CFLAGS='-D__ANDROID_PGANDROID__'"
echo "  make -f src/makefiles/Makefile.android \\"
echo "      NDK=\$NDK ANDROID_API=35 ABI=arm64-v8a \\"
echo "      PGANDROID_DIR=$PGANDROID_ROOT/pgandroid \\"
echo "      all"
echo ""
echo "See $PGANDROID_ROOT/HLD.md for architecture documentation."

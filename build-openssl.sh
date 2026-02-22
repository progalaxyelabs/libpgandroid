#!/bin/bash
# build-openssl.sh — Cross-compile OpenSSL for Android (standalone script)
#
# Builds static OpenSSL libraries for one or more Android ABIs and installs
# them under deps/ so that build.sh can link PostgreSQL with SSL support.
#
# Usage:
#   ./build-openssl.sh [--arch arm64-v8a|x86_64] [--version 3.4.1] [--clean]
#
# Output:
#   deps/openssl-android-arm64/   (arm64-v8a)
#   deps/openssl-android-x86_64/  (x86_64)
#
# Prerequisites:
#   - Android NDK installed, env.sh configured
#   - perl (for OpenSSL's Configure script)
#   - curl or wget (to download source if not already present)

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------

ARCH="arm64-v8a"
OPENSSL_VERSION="3.4.1"
CLEAN=false
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
        --version)
            OPENSSL_VERSION="$2"
            shift 2
            ;;
        --version=*)
            OPENSSL_VERSION="${1#*=}"
            shift
            ;;
        --clean)
            CLEAN=true
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
            echo "Usage: $0 [--arch arm64-v8a|x86_64] [--version 3.4.1] [--clean] [--jobs N]"
            echo ""
            echo "  --arch       Target ABI: arm64-v8a or x86_64 (default: arm64-v8a)"
            echo "  --version    OpenSSL version to build (default: 3.4.1)"
            echo "  --clean      Remove existing build and install dirs before building"
            echo "  --jobs N     Parallel jobs (default: nproc)"
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

# ---------------------------------------------------------------------------
# Arch mapping
# ---------------------------------------------------------------------------

case "$ARCH" in
    arm64-v8a|arm64)
        ARCH="arm64-v8a"
        OPENSSL_TARGET="android-arm64"
        OUTPUT_DIR_NAME="openssl-android-arm64"
        ;;
    x86_64)
        OPENSSL_TARGET="android-x86_64"
        OUTPUT_DIR_NAME="openssl-android-x86_64"
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
DEPS_DIR="$PROJECT/deps"
TARBALL="$DEPS_DIR/openssl-${OPENSSL_VERSION}.tar.gz"
SRC_DIR="$DEPS_DIR/openssl-${OPENSSL_VERSION}"
INSTALL_DIR="$DEPS_DIR/$OUTPUT_DIR_NAME"

# ---------------------------------------------------------------------------
# Source NDK environment
# ---------------------------------------------------------------------------

if [[ ! -f "$PROJECT/env.sh" ]]; then
    echo "ERROR: env.sh not found at $PROJECT/env.sh" >&2
    exit 1
fi

# shellcheck source=env.sh
source "$PROJECT/env.sh"

if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
    echo "ERROR: ANDROID_NDK_HOME is not set after sourcing env.sh" >&2
    exit 1
fi

NDK="$ANDROID_NDK_HOME"

# OpenSSL's Configure script requires ANDROID_NDK_ROOT
export ANDROID_NDK_ROOT="$NDK"

# Ensure the NDK toolchain bin is on PATH (already done by env.sh, but be explicit)
TOOLCHAIN_BIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
export PATH="$TOOLCHAIN_BIN:$PATH"

# ---------------------------------------------------------------------------
# Announce
# ---------------------------------------------------------------------------

echo "========================================================"
echo " Building OpenSSL $OPENSSL_VERSION for Android ($ARCH)"
echo "========================================================"
echo "  NDK         : $NDK"
echo "  Target      : $OPENSSL_TARGET"
echo "  Install dir : $INSTALL_DIR"
echo "  Jobs        : $JOBS"
echo "========================================================"
echo ""

# ---------------------------------------------------------------------------
# Clean
# ---------------------------------------------------------------------------

if $CLEAN; then
    echo "[clean] Removing $SRC_DIR ..."
    rm -rf "$SRC_DIR"
    echo "[clean] Removing $INSTALL_DIR ..."
    rm -rf "$INSTALL_DIR"
fi

# ---------------------------------------------------------------------------
# Download source
# ---------------------------------------------------------------------------

mkdir -p "$DEPS_DIR"

if [[ ! -f "$TARBALL" ]]; then
    echo "[download] Fetching openssl-${OPENSSL_VERSION}.tar.gz ..."
    OPENSSL_URL="https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz"
    if command -v curl &>/dev/null; then
        curl -L --fail --progress-bar -o "$TARBALL" "$OPENSSL_URL"
    elif command -v wget &>/dev/null; then
        wget -O "$TARBALL" "$OPENSSL_URL"
    else
        echo "ERROR: Neither curl nor wget found. Install one and retry." >&2
        exit 1
    fi
else
    echo "[download] Tarball already present at $TARBALL — skipping download."
fi

# ---------------------------------------------------------------------------
# Extract
# ---------------------------------------------------------------------------

if [[ ! -d "$SRC_DIR" ]]; then
    echo "[extract] Extracting openssl-${OPENSSL_VERSION}.tar.gz ..."
    tar -xzf "$TARBALL" -C "$DEPS_DIR"
else
    echo "[extract] Source already extracted at $SRC_DIR — skipping."
fi

# ---------------------------------------------------------------------------
# Configure
# ---------------------------------------------------------------------------

CONFIGURED_SENTINEL="$SRC_DIR/.pgandroid-configured-${ARCH}"

if [[ ! -f "$CONFIGURED_SENTINEL" ]]; then
    echo ""
    echo "[configure] Configuring OpenSSL for $OPENSSL_TARGET ..."
    (
        cd "$SRC_DIR"
        perl Configure "$OPENSSL_TARGET" \
            "-D__ANDROID_API__=34" \
            "--prefix=$INSTALL_DIR" \
            no-shared \
            no-tests \
            no-ui-console \
            no-docs
    )
    touch "$CONFIGURED_SENTINEL"
else
    echo "[configure] Already configured for $ARCH — skipping."
fi

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

echo ""
echo "[build] Compiling OpenSSL (jobs=$JOBS) ..."
make -C "$SRC_DIR" -j"$JOBS"

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------

echo ""
echo "[install] Installing libraries to $INSTALL_DIR ..."
make -C "$SRC_DIR" install_sw   # libraries + headers, no docs

# ---------------------------------------------------------------------------
# Verify
# ---------------------------------------------------------------------------

echo ""
echo "[verify] Checking output binaries ..."

LIBCRYPTO="$INSTALL_DIR/lib/libcrypto.a"
LIBSSL="$INSTALL_DIR/lib/libssl.a"

for lib in "$LIBCRYPTO" "$LIBSSL"; do
    if [[ -f "$lib" ]]; then
        echo "  OK  $lib"
        file "$lib" | sed 's/^/      /'
    else
        echo "  MISSING  $lib" >&2
        exit 1
    fi
done

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

echo ""
echo "========================================================"
echo " OpenSSL $OPENSSL_VERSION built successfully for $ARCH"
echo "========================================================"
echo "  libcrypto : $LIBCRYPTO"
echo "  libssl    : $LIBSSL"
echo "  headers   : $INSTALL_DIR/include/openssl/"
echo ""
echo "You can now run ./build.sh --arch $ARCH"
echo "========================================================"

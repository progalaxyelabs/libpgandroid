# Set ANDROID_NDK_HOME before sourcing, or override the default below
: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME to your Android NDK path (e.g. \$ANDROID_SDK/ndk/27.2.12479018)}"
export ANDROID_NDK_HOME
export PATH=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH

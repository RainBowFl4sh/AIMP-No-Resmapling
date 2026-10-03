#!/bin/sh
# AIMP Prevent Resampling - build on Linux
#
#   ./build.sh            Linux plugin (.so, 64 bit) + test
#   ./build.sh all        additionally the Windows DLLs (32/64 bit) via MinGW-w64
#   ./build.sh install    install the Linux plugin to ~/.local/share/AIMP/Plugins (override with AIMP_PLUGINS)
#
# Requires: cmake, g++, pkg-config, libcairo2-dev (headers only), git
#           for "all" also: mingw-w64
# Result:   dist/PreventResampling/{PreventResampling.dll, x64/PreventResampling.dll, x64/PreventResampling.so}
set -e
cd "$(dirname "$0")"
GEN=""
command -v ninja >/dev/null 2>&1 && GEN="-G Ninja"

cmake -S . -B build/linux $GEN -DAR_BUILD_TESTS=ON
cmake --build build/linux
ctest --test-dir build/linux --output-on-failure
cmake --install build/linux --prefix dist

if [ "$1" = "all" ]; then
    for arch in x86_64 i686; do
        cmake -S . -B build/win-$arch $GEN -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-$arch.cmake
        cmake --build build/win-$arch
        cmake --install build/win-$arch --prefix dist
    done
fi

if [ "$1" = "install" ]; then
    TARGET="${AIMP_PLUGINS:-$HOME/.local/share/AIMP/Plugins}"
    mkdir -p "$TARGET"
    cp -r dist/PreventResampling "$TARGET/"
    echo "Installed to $TARGET/PreventResampling"
fi

echo "Done:"
find dist -type f

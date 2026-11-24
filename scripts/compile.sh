#!/usr/bin/env bash
# Compile build/omp-crowd.dll (Win32, x86, static: it needs only Windows' own kernel32, msvcrt
# and ws2_32, so Wine and Proton run it as they are). Wherever i686-w64-mingw32-g++ is:
# build.sh's container, or CI. No timestamp in the PE header: the same source and toolchain
# give the same bytes, so anyone can check a release.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
i686-w64-mingw32-g++ -std=c++17 -O2 -shared -static -static-libgcc -static-libstdc++ \
	-Wl,--enable-stdcall-fixup -Wl,--no-insert-timestamp -o build/omp-crowd.dll dllmain.cpp -lws2_32
i686-w64-mingw32-strip build/omp-crowd.dll

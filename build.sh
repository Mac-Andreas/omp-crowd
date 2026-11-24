#!/usr/bin/env bash
# Build omp-crowd.dll (Win32, x86) with MinGW in a Debian container: static, no runtime DLLs.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
docker run --rm -v "$PWD:/src" -w /src debian:bookworm bash -euo pipefail -c '
	apt-get update -qq && apt-get install -y -qq --no-install-recommends g++-mingw-w64-i686 >/dev/null
	i686-w64-mingw32-g++ -std=c++17 -O2 -shared -static -static-libgcc -static-libstdc++ \
		-Wl,--enable-stdcall-fixup -o build/omp-crowd.dll dllmain.cpp -lws2_32
	i686-w64-mingw32-strip build/omp-crowd.dll
'
file build/omp-crowd.dll

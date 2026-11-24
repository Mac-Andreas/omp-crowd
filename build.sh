#!/usr/bin/env bash
# Build omp-crowd.dll (Win32, x86) with MinGW in a Debian container: static, no runtime DLLs.
# CI (.github/workflows/build.yml) runs the same scripts/compile.sh in the same image.
# ./build.sh 0.6.0 also packages it (scripts/package.sh) as a release would.
set -euo pipefail
cd "$(dirname "$0")"
docker run --rm -v "$PWD:/src" -w /src debian:bookworm bash -euo pipefail -c '
	apt-get update -qq && apt-get install -y -qq --no-install-recommends g++-mingw-w64-i686 zip >/dev/null
	bash scripts/compile.sh
	if [ -n "${1:-}" ]; then bash scripts/package.sh "$1"; fi
' build "${1:-}"
file build/omp-crowd.dll

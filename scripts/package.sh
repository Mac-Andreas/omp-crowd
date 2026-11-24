#!/usr/bin/env bash
# Package build/omp-crowd.dll for a release: omp-crowd-<version>.zip (the DLL, a sample
# omp-crowd.ini, README.txt, LICENSE), the DLL and the ini on their own, and SHA256SUMS.txt.
set -euo pipefail
cd "$(dirname "$0")/.."
v="${1:?usage: scripts/package.sh <version>}"
test -f build/omp-crowd.dll
d="build/omp-crowd-$v"
rm -rf "$d" "build/omp-crowd-$v.zip"
mkdir -p "$d"
cp build/omp-crowd.dll LICENSE "$d/"
sed "s/@VERSION@/$v/g" dist/omp-crowd.ini > "$d/omp-crowd.ini"
sed "s/@VERSION@/$v/g" dist/README.txt > "$d/README.txt"
(cd build && zip -qX -r "omp-crowd-$v.zip" "omp-crowd-$v")
cp "$d/omp-crowd.ini" build/omp-crowd.ini
if command -v sha256sum >/dev/null; then sum="sha256sum"; else sum="shasum -a 256"; fi
(cd build && $sum omp-crowd.dll omp-crowd.ini "omp-crowd-$v.zip" > SHA256SUMS.txt && cat SHA256SUMS.txt)

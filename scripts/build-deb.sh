#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
mkdir -p build
cd build
package_version="${PACKAGE_VERSION:-0.1.0}"
cmake -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release -DCPACK_PACKAGE_VERSION="$package_version" ..
cmake --build . --parallel "$(nproc)"
cpack -G DEB -C Release

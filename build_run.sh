#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_ROOT="${BUILD_ROOT:-${SCRIPT_DIR}/build/macos}"
RUN_ROOT="${RUN_ROOT:-${SCRIPT_DIR}/build/run}"
CONFIGURATION="${CONFIGURATION:-Release}"

require_tool() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "error: required tool '$1' was not found on PATH" >&2
        exit 1
    fi
}

require_tool /usr/bin/xcodebuild
require_tool cc
require_tool aa
require_tool lipo

rm -rf "${RUN_ROOT}"
mkdir -p "${BUILD_ROOT}" "${RUN_ROOT}/bundles"

echo "==> Building Files.bundle"
/usr/bin/xcodebuild \
    -project "${SCRIPT_DIR}/Files.xcodeproj" \
    -scheme Files \
    -configuration "${CONFIGURATION}" \
    SYMROOT="${BUILD_ROOT}" \
    ARCHS="arm64 x86_64" \
    ONLY_ACTIVE_ARCH=NO \
    CODE_SIGNING_ALLOWED=NO \
    CODE_SIGNING_REQUIRED=NO \
    build

echo "==> Archiving FilesContent bundles"
"${SCRIPT_DIR}/Scripts/archive_files_bundle.sh" \
    "${BUILD_ROOT}/${CONFIGURATION}/Files.bundle" \
    "${RUN_ROOT}/bundles" \
    FilesContent.bundle

echo "==> Building FilesBackend"
cc -std=gnu17 -Wall -Wextra -O2 \
    -o "${BUILD_ROOT}/${CONFIGURATION}/FilesBackend" \
    "${SCRIPT_DIR}/Backend/main.c"

echo "Built:"
echo "  ${BUILD_ROOT}/${CONFIGURATION}/FilesBackend"
echo "  ${RUN_ROOT}/bundles"
echo
echo "Run:"
echo "  \"${BUILD_ROOT}/${CONFIGURATION}/FilesBackend\" --port 7354 --bundles-dir \"${RUN_ROOT}/bundles\""

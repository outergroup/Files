#!/bin/bash

set -euo pipefail
export COPYFILE_DISABLE=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_ROOT="${BUILD_ROOT:-${REPO_ROOT}/build/frontend}"
PACKAGE_ROOT="${PACKAGE_ROOT:-${REPO_ROOT}/build/linux-package}"
OUTPUT_ROOT="${OUTPUT_ROOT:-${REPO_ROOT}/build/release}"
CONFIGURATION="${CONFIGURATION:-Release}"

require_file() {
    if [[ ! -f "$1" ]]; then
        echo "error: missing $1" >&2
        exit 1
    fi
}

require_file "${PACKAGE_ROOT}/RemoteLinuxBinaries/aarch64/FilesBackend"
require_file "${PACKAGE_ROOT}/RemoteLinuxBinaries/x86_64/FilesBackend"
require_file "${REPO_ROOT}/app-icon.png"

mkdir -p "${OUTPUT_ROOT}" "${PACKAGE_ROOT}/bundles"

echo "==> Building Files frontend"
/usr/bin/xcodebuild \
    -project "${REPO_ROOT}/Files.xcodeproj" \
    -scheme Files \
    -configuration "${CONFIGURATION}" \
    SYMROOT="${BUILD_ROOT}" \
    ARCHS="arm64 x86_64" \
    ONLY_ACTIVE_ARCH=NO \
    CODE_SIGNING_ALLOWED=NO \
    CODE_SIGNING_REQUIRED=NO \
    build

echo "==> Archiving FilesContent bundles"
"${REPO_ROOT}/Scripts/archive_files_bundle.sh" \
    "${BUILD_ROOT}/${CONFIGURATION}/Files.bundle" \
    "${PACKAGE_ROOT}/bundles" \
    FilesContent.bundle

STAGING_ROOT="$(mktemp -d)"
trap 'rm -rf "${STAGING_ROOT}"' EXIT

OUTPUT_APP_ROOT="${OUTPUT_ROOT}/Files"
rm -rf "${OUTPUT_APP_ROOT}"
mkdir -p "${OUTPUT_APP_ROOT}"

package_linux_variant() {
    local arch="$1"
    local output_name="$2"
    local app_root="${STAGING_ROOT}/Files"
    rm -rf "${app_root}"
    mkdir -p \
        "${app_root}/RemoteLinuxBinaries/${arch}" \
        "${app_root}/bundles"
    install -m 0755 "${PACKAGE_ROOT}/RemoteLinuxBinaries/${arch}/FilesBackend" "${app_root}/RemoteLinuxBinaries/${arch}/FilesBackend"
    install -m 0644 "${PACKAGE_ROOT}/bundles/FilesContent.bundle.macos-arm.aar" "${app_root}/bundles/FilesContent.bundle.macos-arm.aar"
    install -m 0644 "${PACKAGE_ROOT}/bundles/FilesContent.bundle.macos-x86.aar" "${app_root}/bundles/FilesContent.bundle.macos-x86.aar"
    install -m 0644 "${REPO_ROOT}/app-icon.png" "${app_root}/app-icon.png"
    tar --format ustar --no-xattrs -C "${STAGING_ROOT}" -czf "${OUTPUT_APP_ROOT}/${output_name}.tar.gz" Files
    echo "Packaged ${OUTPUT_APP_ROOT}/${output_name}.tar.gz"
}

package_linux_variant aarch64 linux-aarch64
package_linux_variant x86_64 linux-x86_64

#!/bin/bash

set -euo pipefail
export COPYFILE_DISABLE=1

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_ROOT="${BUILD_ROOT:-${REPO_ROOT}/build/frontend}"
MACOS_BUILD_ROOT="${MACOS_BUILD_ROOT:-${REPO_ROOT}/build/macos}"
PACKAGE_ROOT="${PACKAGE_ROOT:-${REPO_ROOT}/build/linux-package}"
OUTPUT_ROOT="${OUTPUT_ROOT:-${REPO_ROOT}/build/release}"
CONFIGURATION="${CONFIGURATION:-Release}"
APP_CODESIGN_IDENTITY="${APP_CODESIGN_IDENTITY:-${OUTER_SHELL_CODESIGN_IDENTITY:-${CODE_SIGN_IDENTITY:--}}}"

require_file() {
    if [[ ! -f "$1" ]]; then
        echo "error: missing $1" >&2
        exit 1
    fi
}

require_file "${PACKAGE_ROOT}/RemoteLinuxBinaries/aarch64/FilesBackend"
require_file "${PACKAGE_ROOT}/RemoteLinuxBinaries/x86_64/FilesBackend"
require_file "${PACKAGE_ROOT}/RemoteLinuxBinariesMusl/aarch64/FilesBackend"
require_file "${PACKAGE_ROOT}/RemoteLinuxBinariesMusl/x86_64/FilesBackend"
require_file "${REPO_ROOT}/app-icon.png"

mkdir -p "${OUTPUT_ROOT}" "${PACKAGE_ROOT}/bundles" "${MACOS_BUILD_ROOT}/${CONFIGURATION}"

macos_codesign_args() {
    if [[ "${APP_CODESIGN_IDENTITY}" == "-" ]]; then
        printf '%s\n' --force --sign - --timestamp=none
    else
        printf '%s\n' --force --options runtime --timestamp --sign "${APP_CODESIGN_IDENTITY}"
    fi
}

sign_macos_app() {
    local path="$1"
    if ! command -v /usr/bin/codesign >/dev/null 2>&1; then
        return 0
    fi
    local args=()
    while IFS= read -r arg; do
        args+=("$arg")
    done < <(macos_codesign_args)
    /usr/bin/codesign "${args[@]}" "$path" >/dev/null
}

echo "==> Building FilesBackend for macOS"
cc -std=gnu17 -Wall -Wextra -O2 \
    -arch arm64 -arch x86_64 \
    -mmacosx-version-min=14.6 \
    -o "${MACOS_BUILD_ROOT}/${CONFIGURATION}/FilesBackend" \
    "${REPO_ROOT}/Backend/main.c"

require_file "${MACOS_BUILD_ROOT}/${CONFIGURATION}/FilesBackend"

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

install_shared_resources() {
    local app_root="$1"
    mkdir -p "${app_root}/bundles"
    install -m 0644 "${PACKAGE_ROOT}/bundles/FilesContent.bundle.macos-arm.aar" "${app_root}/bundles/FilesContent.bundle.macos-arm.aar"
    install -m 0644 "${PACKAGE_ROOT}/bundles/FilesContent.bundle.macos-x86.aar" "${app_root}/bundles/FilesContent.bundle.macos-x86.aar"
    install -m 0644 "${REPO_ROOT}/app-icon.png" "${app_root}/app-icon.png"
}

write_info_plist() {
    local app_bundle="$1"
    cat > "${app_bundle}/Contents/Info.plist" <<'__FILES_INFO_PLIST__'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>FilesBackend</string>
    <key>CFBundleIdentifier</key>
    <string>org.outershell.Files</string>
    <key>CFBundleName</key>
    <string>Files</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>LSBackgroundOnly</key>
    <true/>
</dict>
</plist>
__FILES_INFO_PLIST__
}

package_linux_variant() {
    local arch="$1"
    local libc="$2"
    local output_name="$3"
    local binary_directory=RemoteLinuxBinaries
    [[ "${libc}" == musl ]] && binary_directory=RemoteLinuxBinariesMusl
    local app_root="${STAGING_ROOT}/Files"
    rm -rf "${app_root}"
    mkdir -p "${app_root}/${binary_directory}/${arch}"
    install_shared_resources "${app_root}"
    install -m 0755 "${PACKAGE_ROOT}/${binary_directory}/${arch}/FilesBackend" "${app_root}/${binary_directory}/${arch}/FilesBackend"
    tar --format ustar --no-xattrs -C "${STAGING_ROOT}" -czf "${OUTPUT_APP_ROOT}/${output_name}.tar.gz" Files
    echo "Packaged ${OUTPUT_APP_ROOT}/${output_name}.tar.gz"
}

package_macos_variant() {
    local arch="$1"
    local output_name="$2"
    local app_root="${STAGING_ROOT}/Files"
    local macos_app_root="${app_root}/Files.app"
    rm -rf "${app_root}"
    mkdir -p \
        "${macos_app_root}/Contents/MacOS" \
        "${macos_app_root}/Contents/Resources/bundles"
    install_shared_resources "${app_root}"
    /usr/bin/lipo "${MACOS_BUILD_ROOT}/${CONFIGURATION}/FilesBackend" -thin "${arch}" -output "${macos_app_root}/Contents/MacOS/FilesBackend"
    chmod 0755 "${macos_app_root}/Contents/MacOS/FilesBackend"
    install -m 0644 "${PACKAGE_ROOT}/bundles/FilesContent.bundle.macos-arm.aar" "${macos_app_root}/Contents/Resources/bundles/FilesContent.bundle.macos-arm.aar"
    install -m 0644 "${PACKAGE_ROOT}/bundles/FilesContent.bundle.macos-x86.aar" "${macos_app_root}/Contents/Resources/bundles/FilesContent.bundle.macos-x86.aar"
    install -m 0644 "${REPO_ROOT}/app-icon.png" "${macos_app_root}/Contents/Resources/app-icon.png"
    write_info_plist "${macos_app_root}"
    sign_macos_app "${macos_app_root}"
    tar --format ustar --no-xattrs -C "${STAGING_ROOT}" -czf "${OUTPUT_APP_ROOT}/${output_name}.tar.gz" Files
    echo "Packaged ${OUTPUT_APP_ROOT}/${output_name}.tar.gz"
}

package_linux_variant aarch64 glibc linux-aarch64
package_linux_variant x86_64 glibc linux-x86_64
package_linux_variant aarch64 musl linux-aarch64-musl
package_linux_variant x86_64 musl linux-x86_64-musl
package_macos_variant arm64 macos-arm64
package_macos_variant x86_64 macos-x86_64

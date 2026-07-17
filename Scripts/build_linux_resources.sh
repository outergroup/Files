#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
# shellcheck source=/dev/null
source "${REPO_ROOT}/app.env"

case "$(uname -m)" in
    aarch64|arm64) ARCH="aarch64" ;;
    x86_64|amd64) ARCH="x86_64" ;;
    *) echo "error: unsupported Linux architecture: $(uname -m)" >&2; exit 1 ;;
esac

LIBC="${OUTER_APP_LINUX_LIBC:-}"
if [[ -z "${LIBC}" ]]; then
    if [[ -e "/lib/ld-musl-$(uname -m).so.1" ]] ||
       (command -v ldd >/dev/null 2>&1 && ldd --version 2>&1 | grep -qi musl); then
        LIBC=musl
    else
        LIBC=glibc
    fi
fi
case "${LIBC}" in
    glibc) BINARY_DIRECTORY=RemoteLinuxBinaries ;;
    musl) BINARY_DIRECTORY=RemoteLinuxBinariesMusl ;;
    *) echo "error: OUTER_APP_LINUX_LIBC must be glibc or musl" >&2; exit 1 ;;
esac

OUTPUT_DIR="${REPO_ROOT}/build/linux-package/${BINARY_DIRECTORY}/${ARCH}"
mkdir -p "${OUTPUT_DIR}"

cc ${BACKEND_CFLAGS} \
    -o "${OUTPUT_DIR}/${BACKEND_EXECUTABLE_NAME}" \
    "${REPO_ROOT}/${BACKEND_SOURCE}" \
    ${BACKEND_LDFLAGS}

if command -v strip >/dev/null 2>&1; then
    strip --strip-unneeded "${OUTPUT_DIR}/${BACKEND_EXECUTABLE_NAME}" || true
fi

echo "Built ${BACKEND_EXECUTABLE_NAME} Linux/${ARCH}/${LIBC} resource"

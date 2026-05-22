#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

case "$(uname -m)" in
    aarch64|arm64)
        ARCH="aarch64"
        ;;
    x86_64|amd64)
        ARCH="x86_64"
        ;;
    *)
        echo "error: unsupported Linux architecture: $(uname -m)" >&2
        exit 1
        ;;
esac

OUTPUT_DIR="${REPO_ROOT}/build/linux-package/RemoteLinuxBinaries/${ARCH}"
mkdir -p "${OUTPUT_DIR}"

cc -std=gnu17 -Os -ffunction-sections -fdata-sections -flto \
    -o "${OUTPUT_DIR}/FilesBackend" \
    "${REPO_ROOT}/Backend/main.c" \
    -ldl -lpthread -lm

if command -v strip >/dev/null 2>&1; then
    strip --strip-unneeded "${OUTPUT_DIR}/FilesBackend" || true
fi

echo "Built FilesBackend Linux resource for ${ARCH}"

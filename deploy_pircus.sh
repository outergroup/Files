#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOST="${HOST:-Pircus}"
REMOTE_DIR="${REMOTE_DIR:-outerloop-files}"
PORT="${PORT:-7354}"

"${SCRIPT_DIR}/build_run.sh"

ssh "${HOST}" "mkdir -p '${REMOTE_DIR}/bundles'"
scp "${SCRIPT_DIR}/Backend/main.c" "${HOST}:${REMOTE_DIR}/FilesBackend.c"
scp \
    "${SCRIPT_DIR}/build/run/bundles/FilesContent.bundle.macos-arm.aar" \
    "${SCRIPT_DIR}/build/run/bundles/FilesContent.bundle.macos-x86.aar" \
    "${HOST}:${REMOTE_DIR}/bundles/"

ssh "${HOST}" "cd '${REMOTE_DIR}' && cc -std=gnu17 -Wall -Wextra -Werror -O2 -o FilesBackend FilesBackend.c"
ssh "${HOST}" "cd '${REMOTE_DIR}' && ( \
    if [ -f files-backend.pid ]; then \
        old_pid=\$(cat files-backend.pid); \
        if kill -0 \"\$old_pid\" 2>/dev/null; then \
            kill \"\$old_pid\"; \
            sleep 1; \
        fi; \
    fi; \
    pkill -x FilesBackend 2>/dev/null || true; \
    sleep 1; \
    pkill -9 -x FilesBackend 2>/dev/null || true; \
    nohup ./FilesBackend --port '${PORT}' --bundles-dir ./bundles > files-backend.log 2>&1 & \
    echo \$! > files-backend.pid \
)"

echo "FilesBackend is running on ${HOST}:127.0.0.1:${PORT}"
echo "Remote log: ssh ${HOST} 'tail -f ${REMOTE_DIR}/files-backend.log'"

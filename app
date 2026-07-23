#!/bin/bash
# Development task runner for an official Outer Shell bundled app.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${ROOT}"
source "${ROOT}/app.env"

BUILD_DIR="${ROOT}/build/app-deploy"
STAGE_PARENT="${BUILD_DIR}/stage"
STAGE_ROOT="${STAGE_PARENT}/${APP_STAGE_NAME}"
FRONTEND_BUILD_DIR="${BUILD_DIR}/frontend"
MATRIX_ROOT="${ROOT}/build/linux-package"
TARGET_KIND=ssh
SSH_BASE=()
TARGET_OS=""
TARGET_ARCH=""
TARGET_LIBC=""
TARGET_UID=""
TARGET_HOME=""

require_tool() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "error: required tool '$1' was not found" >&2
        exit 1
    }
}

BUILD_CONTAINER_RUNTIME=""

apple_container_is_ready() {
    command -v container >/dev/null 2>&1 &&
        container system status >/dev/null 2>&1
}

docker_is_ready() {
    command -v docker >/dev/null 2>&1 &&
        docker info >/dev/null 2>&1
}

select_build_container_runtime() {
    case "${OUTER_BUILD_CONTAINER_RUNTIME:-auto}" in
        auto)
            if [[ "$(uname -s)" == Darwin ]] && apple_container_is_ready; then
                BUILD_CONTAINER_RUNTIME=container
            elif docker_is_ready; then
                BUILD_CONTAINER_RUNTIME=docker
            elif apple_container_is_ready; then
                BUILD_CONTAINER_RUNTIME=container
            else
                return 1
            fi
            ;;
        container)
            apple_container_is_ready || return 1
            BUILD_CONTAINER_RUNTIME=container
            ;;
        docker)
            docker_is_ready || return 1
            BUILD_CONTAINER_RUNTIME=docker
            ;;
        *)
            echo "error: OUTER_BUILD_CONTAINER_RUNTIME must be auto, container, or docker" >&2
            return 2
            ;;
    esac
}

build_container_runtime_is_ready() {
    select_build_container_runtime && return 0
    local status=$?
    [[ "${status}" -ne 2 ]] || exit 1
    return "${status}"
}

require_build_container_runtime() {
    select_build_container_runtime && return 0
    local status=$?
    [[ "${status}" -ne 2 ]] || exit 1
    echo "error: no supported build container runtime is ready" >&2
    echo "       start Apple container or Docker, or set OUTER_BUILD_CONTAINER_RUNTIME" >&2
    exit 1
}

ensure_internal_container_network() {
    local network="$1"
    if ! container network list --quiet | grep -Fxq "${network}"; then
        container network create --internal "${network}" >/dev/null
    fi
}

shell_quote() {
    local value="$1"
    printf "'%s'" "${value//\'/\'\\\'\'}"
}

load_target() {
    if [[ ! -f "${ROOT}/target.env" ]]; then
        echo "error: no deploy target configured" >&2
        echo "  ./app target \"ssh -p 22 you@server\"" >&2
        exit 1
    fi
    # shellcheck source=/dev/null
    source "${ROOT}/target.env"
    TARGET_KIND="${OUTER_TARGET_KIND:-ssh}"
    if [[ "${TARGET_KIND}" != ssh ]]; then
        echo "error: official bundled-app deployment currently requires an SSH target" >&2
        exit 1
    fi
    if [[ "$(declare -p OUTER_TARGET_SSH 2>/dev/null)" != declare\ -a* ]] ||
       [[ "${#OUTER_TARGET_SSH[@]}" -eq 0 ]]; then
        echo "error: target.env must define OUTER_TARGET_SSH as a non-empty array" >&2
        exit 1
    fi
    if [[ "$(basename "${OUTER_TARGET_SSH[0]}")" == ssh ]]; then
        mkdir -p "${HOME}/.ssh"
        SSH_BASE=(
            "${OUTER_TARGET_SSH[0]}"
            -o ControlMaster=auto
            -o "ControlPath=${HOME}/.ssh/app-%C"
            -o ControlPersist=2m
            "${OUTER_TARGET_SSH[@]:1}"
        )
    else
        SSH_BASE=("${OUTER_TARGET_SSH[@]}")
    fi
}

run_ssh() {
    "${SSH_BASE[@]}" "$@"
}

probe_target() {
    load_target
    TARGET_OS="$(run_ssh uname -s </dev/null)"
    TARGET_ARCH="$(run_ssh uname -m </dev/null)"
    TARGET_UID="$(run_ssh id -u </dev/null)"
    TARGET_HOME="$(run_ssh 'printf %s "$HOME"' </dev/null)"
    case "${TARGET_ARCH}" in
        aarch64|arm64) TARGET_ARCH=aarch64 ;;
        x86_64|amd64) TARGET_ARCH=x86_64 ;;
        *) echo "error: unsupported target architecture '${TARGET_ARCH}'" >&2; exit 1 ;;
    esac
    if [[ "${TARGET_OS}" != Linux ]]; then
        echo "error: ${APP_NAME} development deploys currently support Linux SSH targets" >&2
        exit 1
    fi
    if run_ssh 'test -e "/lib/ld-musl-$(uname -m).so.1" || (command -v ldd >/dev/null 2>&1 && ldd --version 2>&1 | grep -qi musl)' </dev/null; then
        TARGET_LIBC=musl
    else
        TARGET_LIBC=glibc
    fi
}

target_scope() {
    local scope="${OUTER_TARGET_SCOPE:-${DEFAULT_DEPLOY_SCOPE:-user}}"
    if [[ "${TARGET_UID}" == 0 ]]; then scope=system; fi
    if [[ "${scope}" != user && "${scope}" != system ]]; then
        echo "error: OUTER_TARGET_SCOPE must be user or system" >&2
        exit 1
    fi
    if [[ "${ROOT_ONLY:-false}" == true && "${scope}" != system ]]; then
        echo "error: ${APP_NAME} is root-only; set OUTER_TARGET_SCOPE=system" >&2
        exit 1
    fi
    printf '%s\n' "${scope}"
}

cmd_build_frontend() {
    [[ "$(uname -s)" == Darwin ]] || {
        echo "error: frontend bundles must be built on macOS" >&2
        exit 1
    }
    require_tool /usr/bin/xcodebuild
    require_tool aa
    require_tool lipo

    rm -rf "${FRONTEND_BUILD_DIR}" "${STAGE_ROOT}/bundles" "${STAGE_ROOT}/web"
    mkdir -p "${FRONTEND_BUILD_DIR}" "${STAGE_ROOT}/bundles" "${STAGE_ROOT}/web"
    echo "==> Building ${APP_NAME} frontend"
    /usr/bin/xcodebuild \
        -project "${ROOT}/${XCODE_PROJECT}" \
        -scheme "${XCODE_SCHEME}" \
        -configuration Release \
        SYMROOT="${FRONTEND_BUILD_DIR}" \
        ARCHS="arm64 x86_64" \
        ONLY_ACTIVE_ARCH=NO \
        CODE_SIGNING_ALLOWED=NO \
        CODE_SIGNING_REQUIRED=NO \
        build

    local bundle="${FRONTEND_BUILD_DIR}/Release/${XCODE_PRODUCT_NAME}.bundle"
    [[ -d "${bundle}" ]] || { echo "error: expected ${bundle}" >&2; exit 1; }
    "${ROOT}/${BUNDLE_ARCHIVE_SCRIPT}" \
        "${bundle}" \
        "${STAGE_ROOT}/bundles" \
        "${BUNDLE_ARCHIVE_STEM}"
    install -m 0644 "${ROOT}/app-icon.png" "${STAGE_ROOT}/app-icon.png"
    install -m 0644 "${ROOT}/Resources/FilesWeb/index.html" "${ROOT}/Resources/FilesWeb/app.css" "${ROOT}/Resources/FilesWeb/app.js" "${ROOT}/Resources/FilesWeb/folder-icon.png" "${STAGE_ROOT}/web/"
}

backend_directory() {
    local root="$1" libc="$2" arch="$3"
    if [[ "${libc}" == musl ]]; then
        printf '%s/RemoteLinuxBinariesMusl/%s\n' "${root}" "${arch}"
    else
        printf '%s/RemoteLinuxBinaries/%s\n' "${root}" "${arch}"
    fi
}

backend_image() {
    local libc="$1" arch="$2"
    if [[ "${libc}" == musl ]]; then
        printf 'quay.io/pypa/musllinux_1_2_%s\n' "${arch}"
    else
        printf 'quay.io/pypa/manylinux2014_%s\n' "${arch}"
    fi
}

backend_platform() {
    [[ "$1" == aarch64 ]] && printf 'linux/arm64\n' || printf 'linux/amd64\n'
}

build_backend_in_container() {
    local libc="$1" arch="$2" output_root="$3"
    local image platform output_dir network
    image="$(backend_image "${libc}" "${arch}")"
    platform="$(backend_platform "${arch}")"
    output_dir="$(backend_directory "${output_root}" "${libc}" "${arch}")"
    network="bundled-app-build-$(printf '%s' "${APP_ID}" | tr '[:upper:]' '[:lower:]' | sed 's/[^a-z0-9._-]/-/g')-internal"
    if [[ -z "${BUILD_CONTAINER_RUNTIME}" ]]; then
        require_build_container_runtime
    fi
    echo "==> Building ${BACKEND_EXECUTABLE_NAME} for Linux/${arch}/${libc}"
    mkdir -p "${output_dir}"
    if [[ "${BUILD_CONTAINER_RUNTIME}" == container ]]; then
        ensure_internal_container_network "${network}"
        container run --rm --platform "${platform}" \
            --network "${network}" \
            --mount "type=bind,source=${ROOT}/Backend,target=/src/Backend,readonly" \
            --mount "type=bind,source=${output_dir},target=/out" \
            --env CC=gcc \
            --env "BACKEND_SOURCE=${BACKEND_SOURCE}" \
            --env "BACKEND_EXECUTABLE_NAME=${BACKEND_EXECUTABLE_NAME}" \
            --env "BACKEND_CFLAGS=${BACKEND_CFLAGS}" \
            --env "BACKEND_LDFLAGS=${BACKEND_LDFLAGS}" \
            "${image}" \
            bash -lc '"$CC" $BACKEND_CFLAGS -o "/out/$BACKEND_EXECUTABLE_NAME" "/src/$BACKEND_SOURCE" $BACKEND_LDFLAGS && strip --strip-unneeded "/out/$BACKEND_EXECUTABLE_NAME"'
    else
        docker run --rm --platform "${platform}" \
            --network none \
            --mount "type=bind,source=${ROOT}/Backend,target=/src/Backend,readonly" \
            --mount "type=bind,source=${output_dir},target=/out" \
            --env CC=gcc \
            --env "BACKEND_SOURCE=${BACKEND_SOURCE}" \
            --env "BACKEND_EXECUTABLE_NAME=${BACKEND_EXECUTABLE_NAME}" \
            --env "BACKEND_CFLAGS=${BACKEND_CFLAGS}" \
            --env "BACKEND_LDFLAGS=${BACKEND_LDFLAGS}" \
            "${image}" \
            bash -lc '"$CC" $BACKEND_CFLAGS -o "/out/$BACKEND_EXECUTABLE_NAME" "/src/$BACKEND_SOURCE" $BACKEND_LDFLAGS && strip --strip-unneeded "/out/$BACKEND_EXECUTABLE_NAME"'
    fi
}

build_backend_on_target() {
    local output_dir remote_build=".cache/outershell-app-build/${APP_ID}"
    output_dir="$(backend_directory "${STAGE_ROOT}" "${TARGET_LIBC}" "${TARGET_ARCH}")"
    echo "==> No build container runtime is available; building ${BACKEND_EXECUTABLE_NAME} on the ${TARGET_LIBC} target"
    COPYFILE_DISABLE=1 tar czf - -C "${ROOT}" Backend | run_ssh "
        set -e
        rm -rf \"\$HOME/${remote_build}\"
        mkdir -p \"\$HOME/${remote_build}\"
        tar xzf - -C \"\$HOME/${remote_build}\"
        cc ${BACKEND_CFLAGS} -o \"\$HOME/${remote_build}/${BACKEND_EXECUTABLE_NAME}\" \
            \"\$HOME/${remote_build}/${BACKEND_SOURCE}\" ${BACKEND_LDFLAGS}
        if command -v strip >/dev/null 2>&1; then
            strip --strip-unneeded \"\$HOME/${remote_build}/${BACKEND_EXECUTABLE_NAME}\"
        fi
    "
    mkdir -p "${output_dir}"
    run_ssh "cat \"\$HOME/${remote_build}/${BACKEND_EXECUTABLE_NAME}\"" > "${output_dir}/${BACKEND_EXECUTABLE_NAME}"
    chmod 0755 "${output_dir}/${BACKEND_EXECUTABLE_NAME}"
}

build_target_backend() {
    if [[ "${OUTER_BUILD_MODE:-auto}" != target ]] &&
       build_container_runtime_is_ready; then
        build_backend_in_container "${TARGET_LIBC}" "${TARGET_ARCH}" "${STAGE_ROOT}"
    else
        build_backend_on_target
    fi
}

cmd_build_backend() {
    probe_target
    build_target_backend
}

cmd_build_matrix() {
    require_build_container_runtime
    rm -rf "${MATRIX_ROOT}/RemoteLinuxBinaries" "${MATRIX_ROOT}/RemoteLinuxBinariesMusl"
    local libc arch
    for libc in glibc musl; do
        for arch in aarch64 x86_64; do
            build_backend_in_container "${libc}" "${arch}" "${MATRIX_ROOT}"
        done
    done
    echo "==> Linux backend matrix ready at ${MATRIX_ROOT}"
}

cmd_build() {
    probe_target
    rm -rf "${STAGE_ROOT}"
    mkdir -p "${STAGE_ROOT}"
    cmd_build_frontend
    build_target_backend
    echo "==> Payload ready at ${STAGE_ROOT}"
}

find_outerctl_command() {
    printf '%s' 'outerctl="${XDG_STATE_HOME:-$HOME/.local/state}/outershell/bin/outerctl"; '
    printf '%s' 'if [ ! -x "$outerctl" ]; then outerctl="$(command -v outerctl 2>/dev/null || true)"; fi; '
    printf '%s' 'if [ ! -x "$outerctl" ]; then echo "error: install the current Outer Shell on this target first" >&2; exit 1; fi; '
}

run_bundled_control() {
    local action="$1"
    local stage_root="${2:-}"
    local scope command
    scope="$(target_scope)"
    command="$(find_outerctl_command)"
    command+="\"\$outerctl\" bundled-app ${action} --backend $(shell_quote "${APP_ID}") --scope $(shell_quote "${scope}")"
    if [[ "${action}" == install ]]; then
        command+=" --stage-root $(shell_quote "${stage_root}")"
    fi
    if [[ "${scope}" == system && "${TARGET_UID}" != 0 ]]; then
        local password
        read -r -s -p "Administrator password for target: " password </dev/tty
        printf '\n' >/dev/tty
        printf '%s\n' "${password}" | run_ssh "${command} --sudo-password-stdin"
        unset password
    else
        run_ssh "${command}"
    fi
}

cmd_deploy() {
    cmd_build
    local remote_parent=".cache/outershell-app-deploy/${APP_ID}"
    local remote_stage="${TARGET_HOME}/${remote_parent}/${APP_STAGE_NAME}"
    echo "==> Uploading ${APP_NAME} payload"
    COPYFILE_DISABLE=1 tar czf - -C "${STAGE_PARENT}" "${APP_STAGE_NAME}" | run_ssh "
        set -e
        rm -rf \"\$HOME/${remote_parent}\"
        mkdir -p \"\$HOME/${remote_parent}\"
        tar xzf - -C \"\$HOME/${remote_parent}\"
    "
    echo "==> Installing ${APP_NAME} through Outer Shell"
    run_bundled_control install "${remote_stage}"
    echo "Deployed ${APP_NAME}."
}

cmd_uninstall() {
    probe_target
    run_bundled_control uninstall ""
}

cmd_target() {
    if [[ $# -ne 1 ]]; then
        echo "usage: ./app target \"ssh -p 22 you@server\"" >&2
        exit 1
    fi
    read -r -a words <<< "$1"
    {
        printf 'OUTER_TARGET_KIND=ssh\nOUTER_TARGET_SSH=(\n'
        local word
        for word in "${words[@]}"; do
            printf '    '
            shell_quote "${word}"
            printf '\n'
        done
        printf ')\nOUTER_TARGET_SCOPE=%s\n' "${DEFAULT_DEPLOY_SCOPE:-user}"
    } > "${ROOT}/target.env"
    echo "Wrote target.env"
}

cmd_status() {
    probe_target
    local scope
    scope="$(target_scope)"
    if [[ "${scope}" == system ]]; then
        run_ssh "systemctl --system status '${APP_ID}.service' --no-pager" || true
    else
        run_ssh "systemctl --user status '${APP_ID}.service' --no-pager" || true
    fi
}

cmd_logs() {
    probe_target
    local scope
    scope="$(target_scope)"
    if [[ "${scope}" == system ]]; then
        run_ssh -t "journalctl --system -u '${APP_ID}.service' -f"
    else
        run_ssh -t "journalctl --user -u '${APP_ID}.service' -f"
    fi
}

cmd_clean() {
    rm -rf "${BUILD_DIR}"
}

cmd_help() {
    cat <<EOF
${APP_NAME} development tasks

  ./app target "ssh ..."  write target.env
  ./app build             build a payload for target.env
  ./app build-matrix      build all Linux libc/architecture release variants
  ./app deploy            build, upload, and install through Outer Shell
  ./app status            show the target service status
  ./app logs              follow the target service log
  ./app ssh [command]     open a shell or run a target command
  ./app uninstall         uninstall the target app
  ./app clean             remove development build products

Builds auto-select Apple container on macOS and Docker elsewhere.
Set OUTER_BUILD_CONTAINER_RUNTIME=container|docker to override.
EOF
}

command="${1:-help}"
shift $(( $# > 0 ? 1 : 0 ))
case "${command}" in
    target) cmd_target "$@" ;;
    build) cmd_build ;;
    build-frontend) cmd_build_frontend ;;
    build-backend) cmd_build_backend ;;
    build-matrix) cmd_build_matrix ;;
    deploy) cmd_deploy ;;
    status) cmd_status ;;
    logs) cmd_logs ;;
    ssh) probe_target; run_ssh "$@" ;;
    uninstall) cmd_uninstall ;;
    clean) cmd_clean ;;
    help|--help|-h) cmd_help ;;
    *) echo "error: unknown command '${command}'" >&2; cmd_help >&2; exit 1 ;;
esac

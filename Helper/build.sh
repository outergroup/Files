#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
os=${1:?usage: build.sh OS ARCH OUTPUT}
arch=${2:?usage: build.sh OS ARCH OUTPUT}
output=${3:?usage: build.sh OS ARCH OUTPUT}
case "$arch" in aarch64) arch=arm64;; x86_64) arch=amd64;; esac
case "$output" in /*) ;; *) output="$PWD/$output";; esac
mkdir -p "$(dirname "$output")"
case "$os/$arch" in darwin/arm64|darwin/amd64|linux/arm64|linux/amd64) ;; *) echo "Unsupported target: $os/$arch" >&2; exit 1;; esac
if [ "$os" = linux ] && [ -z "${CC:-}" ]; then
    temporary=$(mktemp -d)
    trap 'rm -rf "$temporary"' EXIT HUP INT TERM
    docker build --platform "linux/$arch" --output "type=local,dest=$temporary" "$root"
    install -m 755 "$temporary/outer-files-helper" "$output"
    exit
fi
set --
if [ "$os" = darwin ]; then
    case "$arch" in arm64) set -- -arch arm64;; amd64) set -- -arch x86_64;; esac
else
    set -- -static
fi
${CC:-cc} -std=gnu17 -O2 -Wall -Wextra -Wno-unused-function -Wno-deprecated-declarations "$@" \
    "$root"/*.c -lpthread -lm -o "$output"

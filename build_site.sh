#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
echo "Files is backend-backed; building the runnable backend package instead of a static site."
exec "${SCRIPT_DIR}/build_run.sh"

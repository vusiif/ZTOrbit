#!/usr/bin/env bash
# Relative --file paths are resolved from the caller's working directory.
set -euo pipefail
base=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
exec python3 "$base/tools/publish-update.py" --root "$base" "$@"

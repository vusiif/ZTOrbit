#!/bin/bash
set -euo pipefail
exec bash "$(dirname "${BASH_SOURCE[0]}")/../.agent/skills/ztgui-build/scripts/build-linux.sh" "$@"

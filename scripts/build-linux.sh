#!/bin/bash
set -euo pipefail
exec bash "$(dirname "${BASH_SOURCE[0]}")/../.agents/skills/zto-build/scripts/build-linux.sh" "$@"

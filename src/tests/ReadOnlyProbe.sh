#!/bin/sh
# Manual native read-only check for POSIX hosts. Run on a normal user account;
# root bypasses permission bits and cannot prove the fallback path.
#   ./ReadOnlyProbe.sh <path-to-readonly_probe>
set -eu
if [ "${1:-}" = "" ]; then
    echo "usage: $0 <readonly_probe>" >&2
    exit 2
fi
probe=$1
if [ ! -x "$probe" ]; then
    echo "probe is not executable: $probe" >&2
    exit 2
fi
if [ "$(id -u)" = "0" ]; then
    echo "SKIP: running as root bypasses permission bits" >&2
    exit 77
fi
root=$(mktemp -d "${TMPDIR:-/tmp}/zto-readonly-XXXXXX")
shared="$root/application"
personal="$root/user-data"
mkdir -p "$shared"
restore() { chmod 0755 "$shared" 2>/dev/null || true; rm -rf "$root"; }
trap restore EXIT INT TERM
chmod 0555 "$shared"
output=$("$probe" --shared "$shared" --personal "$personal")
status=$?
printf '%s\n' "$output"
[ "$status" -eq 0 ] || { echo "read-only probe exited with $status" >&2; exit 1; }
case "$output" in
    *'"mode":"personal"'*) ;;
    *) echo "expected personal fallback" >&2; exit 1 ;;
esac
[ ! -e "$shared/settings.ini" ] || { echo "wrote into the read-only root" >&2; exit 1; }
echo "POSIX native read-only fallback passed"

#!/bin/bash
# Transactional, locally recoverable stage-7 egress fault. Run namespace tests
# before use on the real interface. No opt-out from the local watchdog exists.
# Environment: ZTO_FAULT_IFACE / PORT / STATE. State must be dedicated per iface.
# apply '<netem args>' [seconds] | change '<netem args>' | clear | show
set -Eeuo pipefail
iface="${ZTO_FAULT_IFACE:-ztd56sj2yi}"
port="${ZTO_FAULT_PORT:-18200}"
state_dir="${ZTO_FAULT_STATE:-/run/zto-stage7-fault}"
action="${1:-show}"
[[ "$iface" =~ ^[a-zA-Z0-9_.:-]+$ && "$port" =~ ^[0-9]+$ && "$port" -gt 0 && "$port" -le 65535 ]] || exit 2
[[ "$state_dir" == /* && "$state_dir" != / && "$state_dir" != *$'\n'* ]] || exit 2

restore_original() {
    echo "restore $(date -Ins) iface=$iface" >&2
    if ! timeout 10 tc qdisc replace dev "$iface" root fq_codel; then
        echo "ERROR: fq_codel restoration failed on $iface; watchdog remains armed" >&2
        return 1
    fi
    tc qdisc show dev "$iface" | grep -Eq '^qdisc fq_codel .* root' || {
        echo "ERROR: fq_codel restoration not confirmed" >&2; return 1;
    }
}

watchdog_identity() {
    [[ -f "$state_dir/watchdog.pid" ]] || return 1
    read -r watchdog_pid watchdog_start < "$state_dir/watchdog.pid"
    [[ "$watchdog_pid" =~ ^[0-9]+$ && -r /proc/$watchdog_pid/stat ]] || return 1
    local current_start
    current_start="$(awk '{print $22}' "/proc/$watchdog_pid/stat" 2>/dev/null)" || return 1
    [[ "$current_start" == "$watchdog_start" ]] || return 1
    grep -zFq '__watchdog' "/proc/$watchdog_pid/cmdline"
}

if [[ "$action" == show ]]; then
    tc -s qdisc show dev "$iface"
    tc filter show dev "$iface" parent 1:0
    [[ ! -f "$state_dir/restore.status" ]] || cat "$state_dir/restore.status"
    exit 0
fi

# Every state operation, executable prerequisite and permissions check precedes
# any network mutation. Failure here leaves the original queue untouched.
for command in tc timeout flock setsid bash awk grep date; do command -v "$command" >/dev/null; done
mkdir -p -- "$state_dir"
chmod 700 -- "$state_dir"
probe="$(mktemp "$state_dir/.write-test.XXXXXX")"
printf 'state verified\n' > "$probe"
rm -- "$probe"
exec 9>"$state_dir/lock"

if [[ "$action" == __watchdog ]]; then
    seconds="$2"
    # Independent process, no inherited parent lock. Handshake is written only
    # after it can execute the recovery path and access the state directory.
    printf '%s %s\n' "$$" "$(awk '{print $22}' /proc/$$/stat)" > "$state_dir/watchdog.pid"
    printf 'ready\n' > "$state_dir/watchdog.ready"
    deadline=$((SECONDS + seconds))
    while (( SECONDS < deadline )); do
        [[ ! -f "$state_dir/cancel" ]] || exit 0
        sleep 0.1
    done
    flock -w 30 9 || { echo 'ERROR: watchdog recovery lock timeout' >&2; exit 1; }
    [[ ! -f "$state_dir/cancel" ]] || exit 0
    if restore_original; then
        echo "restored $(date -Ins)" > "$state_dir/restore.status"
        rm -f -- "$state_dir/watchdog.ready" "$state_dir/watchdog.pid"
        exit 0
    fi
    echo "FAILED $(date -Ins)" > "$state_dir/restore.status"
    # Transient recovery failure is visible and retried locally.
    for retry in 1 2 3 4 5; do
        sleep 1
        if restore_original; then
            echo "restored-after-retry $(date -Ins)" > "$state_dir/restore.status"
            rm -f -- "$state_dir/watchdog.ready" "$state_dir/watchdog.pid"
            exit 0
        fi
    done
    exit 1
fi

flock -w 10 9
dirty=0
rollback() {
    rc=$?
    trap - EXIT INT TERM HUP
    if (( dirty )); then
        if restore_original; then
            echo "rolled-back $(date -Ins)" > "$state_dir/restore.status"
            touch "$state_dir/cancel"
        else
            echo "ROLLBACK FAILED; local watchdog retained, see $state_dir/watchdog.log" >&2
            rc=1
        fi
    fi
    exit "$rc"
}
trap rollback EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

case "$action" in
    apply)
        seconds="${3:-60}"
        [[ "$seconds" =~ ^[0-9]+$ && "$seconds" -ge 1 && "$seconds" -le 3600 && -n "${2:-}" ]] || exit 2
        if watchdog_identity; then echo 'ERROR: another fault watchdog is active; clear first' >&2; exit 1; fi
        # Refuse to overwrite an unrecognised queue. The test restores the
        # recorded distro fq_codel root, not arbitrary production shaping.
        tc qdisc show dev "$iface" | grep -Eq '^qdisc fq_codel .* root' || {
            echo 'ERROR: baseline must be fq_codel' >&2; exit 1;
        }
        rm -f -- "$state_dir/cancel" "$state_dir/watchdog.ready" "$state_dir/watchdog.pid" "$state_dir/restore.status"
        setsid bash "$(readlink -f "$0")" __watchdog "$seconds" 9>&- \
            >"$state_dir/watchdog.log" 2>&1 < /dev/null &
        child=$!
        ready=0
        for attempt in {1..50}; do
            if [[ -f "$state_dir/watchdog.ready" ]] && watchdog_identity; then ready=1; break; fi
            kill -0 "$child" 2>/dev/null || break
            sleep 0.02
        done
        if (( ! ready )); then
            echo "ERROR: local recovery task did not become ready; network untouched" >&2
            cat "$state_dir/watchdog.log" >&2
            touch "$state_dir/cancel"
            exit 1
        fi
        # EXIT + signal rollback remains armed throughout installation.
        dirty=1
        timeout 10 tc qdisc replace dev "$iface" root handle 1: prio bands 3 \
            priomap 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
        read -r -a netem_args <<< "$2"
        timeout 10 tc qdisc add dev "$iface" parent 1:2 handle 20: netem "${netem_args[@]}"
        timeout 10 tc filter add dev "$iface" protocol ip parent 1:0 prio 1 u32 \
            match ip protocol 17 0xff match ip sport "$port" 0xffff flowid 1:2
        watchdog_identity
        dirty=0
        echo "fault installed; watchdog=$watchdog_pid cwd=$PWD max_seconds=$seconds"
        ;;
    change)
        watchdog_identity || { echo 'ERROR: no live recovery task' >&2; exit 1; }
        dirty=1
        read -r -a netem_args <<< "${2:?netem arguments required}"
        timeout 10 tc qdisc change dev "$iface" parent 1:2 handle 20: netem "${netem_args[@]}"
        dirty=0
        ;;
    clear)
        # Never cancel the fallback until restoration has succeeded.
        restore_original
        echo "explicitly-restored $(date -Ins)" > "$state_dir/restore.status"
        touch "$state_dir/cancel"
        if watchdog_identity; then
            echo "cancel watchdog=$watchdog_pid start=$watchdog_start" >&2
            if ! kill "$watchdog_pid" 2>/dev/null; then
                # Cancellation may already have made the exact watchdog exit.
                # A live, unchanged process failing to stop is still an error.
                if watchdog_identity; then
                    echo "ERROR: cannot cancel watchdog $watchdog_pid" >&2
                    exit 1
                fi
            fi
        fi
        rm -f -- "$state_dir/watchdog.ready" "$state_dir/watchdog.pid"
        ;;
    *) echo "usage: $0 apply|change|clear|show [netem args] [max_seconds]" >&2; exit 2;;
esac

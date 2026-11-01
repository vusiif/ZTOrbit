#!/bin/bash
# Stage-4 weak-network shaping for the ZeroTier interface. Only UDP traffic with
# source port 18100 (the desktop-share server media socket) is affected; control
# TCP and SSH keep their normal path.
set -euo pipefail
IFACE="${IFACE:-ztd56sj2yi}"
MODE="${1:-apply}"
NETEM=(loss 5% duplicate 2% reorder 25% 50% delay 20ms)

apply_root() {
    tc qdisc del dev "$IFACE" root 2>/dev/null || true
    tc qdisc add dev "$IFACE" root handle 1: prio bands 3
    tc qdisc add dev "$IFACE" parent 1:3 handle 30: netem "${NETEM[@]}"
    tc filter add dev "$IFACE" protocol ip parent 1:0 prio 1 u32 \
        match ip protocol 17 0xff match ip sport 18100 0xffff flowid 1:3
}

case "$MODE" in
    apply)
        apply_root
        echo "netem applied on $IFACE for UDP sport 18100: ${NETEM[*]}"
        ;;
    blackout)
        tc qdisc change dev "$IFACE" parent 1:3 handle 30: netem loss 100%
        echo "blackout: UDP sport 18100 dropped on $IFACE"
        ;;
    restore)
        tc qdisc change dev "$IFACE" parent 1:3 handle 30: netem "${NETEM[@]}"
        echo "restored weak-network profile on $IFACE"
        ;;
    clear)
        tc qdisc del dev "$IFACE" root 2>/dev/null || true
        echo "qdisc cleared on $IFACE"
        ;;
    *)
        echo "usage: $0 {apply|blackout|restore|clear}" >&2
        exit 2
        ;;
esac

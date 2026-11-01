#!/bin/bash
# Run as root in unshare -nm; refuses the PID-1 network namespace.
set -Eeuo pipefail
[[ "$(readlink /proc/self/ns/net)" != "$(readlink /proc/1/ns/net)" ]] || { echo 'disposable namespace required' >&2; exit 77; }
script="$(readlink -f "${1:-$(dirname "$0")/fault-netem.sh}")"
work="$(mktemp -d /tmp/zto-r2-fault.XXXXXX)"
export ZTO_FAULT_IFACE=zdsr2 ZTO_FAULT_PORT=18260 ZTO_FAULT_STATE="$work/state"
base_path="$PATH"
tc_real="$(command -v tc)"
mkdir "$work/bin"
cleanup() {
    PATH="$base_path" bash "$script" clear || echo 'CLEANUP FAILED' >&2
    [[ -z "${peer:-}" ]] || { kill "$peer" 2>/dev/null || true; wait "$peer" 2>/dev/null || true; }
    rm -rf -- "$work"
}
trap cleanup EXIT
ip link add zdsr2 type veth peer name zdspeer
ip addr add 10.99.0.1/24 dev zdsr2
ip link set zdsr2 up
unshare -n sleep 240 & peer=$!
ip link set zdspeer netns "$peer"
nsenter -t "$peer" -n ip addr add 10.99.0.2/24 dev zdspeer
nsenter -t "$peer" -n ip link set zdspeer up
nsenter -t "$peer" -n ip link set lo up
tc qdisc replace dev zdsr2 root fq_codel
assert_clean() {
    tc qdisc show dev zdsr2
    tc qdisc show dev zdsr2 | grep -Eq '^qdisc fq_codel .* root'
    ! tc qdisc show dev zdsr2 | grep -q netem
}
expect_failure() {
    if "$@"; then echo 'UNEXPECTED SUCCESS' >&2; exit 1; fi
    assert_clean
}
echo 'TEST state storage failure (independent /dev/null counterexample)'
ZTO_FAULT_STATE=/dev/null/zto-review2 expect_failure bash "$script" apply 'loss 100%' 2

echo 'TEST recovery task startup failure'
printf '#!/bin/bash\nexit 71\n' > "$work/bin/setsid"; chmod +x "$work/bin/setsid"
PATH="$work/bin:$base_path" expect_failure bash "$script" apply 'loss 100%' 2
rm "$work/bin/setsid"

echo 'TEST installation fails midway (invalid netem arguments)'
expect_failure bash "$script" apply 'not-a-netem-option' 4
sleep .2

echo 'TEST normal expiry + driver has exited'
bash "$script" apply 'loss 100%' 2
tc qdisc show dev zdsr2 | grep -q 'loss 100%'
sleep 3
assert_clean
cat "$ZTO_FAULT_STATE/restore.status"

echo 'TEST explicit cleanup and change'
bash "$script" apply 'loss 100%' 15
bash "$script" change 'loss 5%'
tc qdisc show dev zdsr2 | grep -q 'loss 5%'
bash "$script" clear
assert_clean
sleep .2

echo 'TEST interrupted installer (TERM/HUP) and driver killed (KILL)'
cat > "$work/bin/tc" <<'WRAPPER'
#!/bin/bash
if [[ "$*" == *'qdisc add'* && "$*" == *'parent'* ]]; then
    echo "entered" > "$ZTO_FAULT_STATE/install.entered"
    sleep 1
fi
exec "$ZTO_TEST_REAL_TC" "$@"
WRAPPER
chmod +x "$work/bin/tc"
export ZTO_TEST_REAL_TC="$tc_real"
for sig in TERM HUP KILL; do
    rm -f "$ZTO_FAULT_STATE/install.entered"
    PATH="$work/bin:$base_path" bash "$script" apply 'loss 100%' 3 & driver=$!
    for attempt in {1..100}; do [[ ! -f "$ZTO_FAULT_STATE/install.entered" ]] || break; sleep .02; done
    [[ -f "$ZTO_FAULT_STATE/install.entered" ]]
    # Exact child PID and argv, not a process-name sweep.
    grep -zFq "$script" "/proc/$driver/cmdline"
    echo "signal=$sig driver=$driver cwd=$(readlink /proc/$driver/cwd) argv=$(tr '\0' ' ' </proc/$driver/cmdline)"
    kill -s "$sig" "$driver"
    wait "$driver" || true
    sleep 4
    assert_clean
done
rm "$work/bin/tc"

echo 'TEST restoration failure visible, watchdog retained and retries'
cat > "$work/bin/tc" <<'WRAPPER'
#!/bin/bash
if [[ "$*" == *'root fq_codel'* && -f "$ZTO_FAULT_STATE/fail.restore" ]]; then
    echo 'injected restoration failure' >&2
    exit 72
fi
exec "$ZTO_TEST_REAL_TC" "$@"
WRAPPER
chmod +x "$work/bin/tc"
PATH="$work/bin:$base_path" bash "$script" apply 'loss 100%' 2
touch "$ZTO_FAULT_STATE/fail.restore"
if PATH="$work/bin:$base_path" bash "$script" clear; then echo 'hidden restore failure' >&2; exit 1; fi
[[ -f "$ZTO_FAULT_STATE/watchdog.pid" ]]
sleep 2.5
grep -q FAILED "$ZTO_FAULT_STATE/restore.status"
cat "$ZTO_FAULT_STATE/watchdog.log"
rm "$ZTO_FAULT_STATE/fail.restore"
sleep 2
assert_clean
rm "$work/bin/tc"

echo 'TEST actual script isolates target UDP from other UDP and same-port TCP'
cat > "$work/isolation.py" <<'PY'
import json, select, socket, sys, time
if sys.argv[1] == 'receive':
    socks={}
    for p in (22,18100,18260):
        s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.bind(('10.99.0.2',p)); socks[s]=p
    tcp=socket.socket();tcp.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
    tcp.bind(('10.99.0.2',18260));tcp.listen();socks[tcp]='tcp'
    counts={'22':0,'18100':0,'18260':0,'tcp':0}
    end=time.monotonic()+2
    while time.monotonic()<end:
        for s in select.select(list(socks),[],[],.05)[0]:
            if s is tcp: c,_=s.accept();c.close()
            else: s.recvfrom(1024)
            counts[str(socks[s])]+=1
    print(json.dumps(counts))
    assert counts=={'22':10,'18100':10,'18260':0,'tcp':1},counts
else:
    for p in (22,18100,18260):
        s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.bind(('10.99.0.1',p))
        for i in range(10):s.sendto(b'x'*100,('10.99.0.2',p))
        s.close()
    s=socket.socket();s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
    s.bind(('10.99.0.1',18260));s.settimeout(1);s.connect(('10.99.0.2',18260));s.close()
PY
bash "$script" apply 'loss 100%' 10
nsenter -t "$peer" -n python3 "$work/isolation.py" receive & receiver=$!
sleep .2
python3 "$work/isolation.py" send
wait "$receiver"
bash "$script" clear
assert_clean
echo 'ALL fault lifecycle and isolation tests passed'

"""Clock offset bounds using one established SSH channel (exclude spawn cost)."""
import argparse, json, os, queue, shlex, subprocess, sys, threading, time
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--host', default='root@124.221.198.141')
p.add_argument('--remote-root', default='/root/ZTOrbitHub/stage7/remediation-r2-18260')
p.add_argument('--out', required=True)
p.add_argument('--samples', type=int, default=11)
a = p.parse_args()
if not a.remote_root.startswith('/root/ZTOrbitHub/stage7/'):
    raise SystemExit('dedicated test directory required')
cmd = ['ssh', '-T', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10', a.host,
       'cd -- '+shlex.quote(a.remote_root)+' && python3 -u clock_echo.py']
proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                        text=True, bufsize=1)
lines = queue.Queue()
def read_lines():
    for line in proc.stdout: lines.put(line)
threading.Thread(target=read_lines, daemon=True).start()
result = {'localProcess': {'pid': os.getpid(), 'cwd': os.getcwd(), 'argv': sys.argv},
          'sshProcess': {'pid': proc.pid, 'cwd': os.getcwd(), 'argv': cmd}, 'samples': []}
try:
    result['remote'] = json.loads(lines.get(timeout=15))
    assert result['remote']['ready'] and result['remote']['process']['cwd'] == a.remote_root
    for i in range(a.samples):
        t0 = time.time_ns()/1e6
        proc.stdin.write('sample\n'); proc.stdin.flush()
        remote = int(lines.get(timeout=10))
        t1 = time.time_ns()/1e6
        result['samples'].append({'sentLocalMs': t0, 'returnedLocalMs': t1,
                                  'serverEpochMs': remote, 'rttMs': t1-t0,
                                  'offsetMs': remote-(t0+t1)/2, 'errorMs': (t1-t0)/2+1})
    selected = min(result['samples'], key=lambda s: s['rttMs'])
    lo = max(s['offsetMs']-s['errorMs'] for s in result['samples'])
    hi = min(s['offsetMs']+s['errorMs'] for s in result['samples'])
    result.update(selected=selected, stationaryOffsetIntervalsOverlap=lo <= hi,
                  offsetIntersection=[lo, hi])
finally:
    proc.stdin.close()
    try: proc.wait(timeout=5)
    except subprocess.TimeoutExpired: proc.kill(); proc.wait()
    result['sshExitCode'] = proc.returncode
    result['sshStderr'] = proc.stderr.read()
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    Path(a.out).write_text(json.dumps(result,indent=2),encoding='utf-8')
print(json.dumps(result.get('selected')))
if proc.returncode or not result.get('stationaryOffsetIntervalsOverlap'):
    raise SystemExit(1)

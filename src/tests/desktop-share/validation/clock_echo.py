"""Read-only clock probe on the dedicated test host, terminates at stdin EOF."""
import json, os, sys, time
from pathlib import Path

print(json.dumps({'ready': True, 'process': {'pid': os.getpid(), 'cwd': os.getcwd(),
      'argv': Path('/proc/self/cmdline').read_bytes().decode().strip('\0').split('\0')}}), flush=True)
for _ in sys.stdin:
    print(time.time_ns() // 1000000, flush=True)

"""Record provenance from the actual controlled binaries, never from filenames."""
import ctypes
import hashlib
import json
import os
import pathlib
import sys

root = pathlib.Path(sys.argv[1]).resolve()
with os.add_dll_directory(str(root / "bin")):
    library = ctypes.CDLL(str(root / "bin" / "avutil-59.dll"))
    record = {}
    for symbol in ("av_version_info", "avutil_configuration", "avutil_license"):
        function = getattr(library, symbol)
        function.restype = ctypes.c_char_p
        record[symbol] = function().decode("utf-8")
record["binaries"] = {
    p.name: hashlib.sha256(p.read_bytes()).hexdigest()
    for p in sorted((root / "bin").glob("*.dll"))
}
record["sources"] = json.loads((root / "sources.json").read_text(encoding="utf-8-sig"))
(root / "actual-build.json").write_text(json.dumps(record, indent=2), encoding="utf-8")

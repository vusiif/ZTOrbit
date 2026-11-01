#!/usr/bin/env python3
"""Package the Java ZTOrbitHub plugin SDK from the Maven-built API module.

Layout of build/dist/Z2Hub-api-v<api>/ (also a Maven repository root):
  vusiif/ztohub/ztohub-api/<api>/ztohub-api-<api>.jar
  vusiif/ztohub/ztohub-api/<api>/ztohub-api-<api>-sources.jar
  vusiif/ztohub/ztohub-api/<api>/ztohub-api-<api>-javadoc.jar
  vusiif/ztohub/ztohub-api/<api>/ztohub-api-<api>.pom
  sample/                       sample plugin project
  sdk.json                      versions, JDK requirement and input fingerprint
  README.md

The SDK is only rebuilt when the API sources, POMs or SERVER_VERSION changed, and the ZIP is
re-created when it is missing or corrupt. No build backup directories are retained.
"""
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sdk_packaging  # noqa: E402  (local helper module)


def split_version(version):
    match = re.fullmatch(r"0\.([0-9]+)\.[0-9]+(-[0-9A-Za-z.-]+)?", version)
    if not match:
        raise SystemExit(f"SERVER_VERSION must be 0.<core>.<patch>[-qualifier]: {version}")
    return f"0.{match.group(1)}{match.group(2) or ''}"


def main():
    root = Path(sys.argv[1]).resolve()
    version = (root / "SERVER_VERSION").read_text(encoding="utf-8").strip()
    api_version = split_version(version)
    api_dir = root / "src" / "server" / "api"
    api_jar = api_dir / "target" / f"ztohub-api-{api_version}.jar"
    if not api_jar.is_file():
        raise SystemExit(f"Build the API first: missing {api_jar}")
    dist = root / "build" / "dist"
    sdk = dist / f"Z2Hub-api-v{api_version}"
    archive = sdk.with_suffix(sdk.suffix + ".zip")
    state_file = api_dir / "target" / "sdk-package-state.json"
    inputs = sdk_packaging.fingerprint_inputs(root, api_version)
    try:
        state = json.loads(state_file.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        state = {}
    zip_ok = sdk_packaging.zip_is_valid(archive)
    changed = (not sdk.is_dir() or state.get("inputs") != inputs
               or state.get("sdk") != sdk_packaging.fingerprint_tree(sdk))
    if changed:
        sdk_packaging.build_sdk(root, sdk, api_version, version, api_jar, inputs)
    if changed or not zip_ok:
        sdk_packaging.write_zip(sdk, archive)
    state_file.parent.mkdir(parents=True, exist_ok=True)
    state_file.write_text(json.dumps({
        "inputs": inputs,
        "sdk": sdk_packaging.fingerprint_tree(sdk),
        "archive": sdk_packaging.sha256_file(archive),
    }, indent=2) + "\n", encoding="utf-8")
    print(("Updated" if changed else "Reused") + " " + str(archive))


if __name__ == "__main__":
    main()

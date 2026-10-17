#!/usr/bin/env python3
"""Compile FixturePlugin.java once and package the fixture jars used by IntegrationTests.py."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

VARIANTS = [
    # file name, manifest id, display name, api version (None = host API), main class
    ("test-echo.jar", "test-echo", "test-echo", None, "fixture.FixturePlugin"),
    ("test-other.jar", "test-other", "test-other", None, "fixture.FixturePlugin"),
    ("test-failed.jar", "test-failed", "test-failed", None, "fixture.FixturePlugin"),
    ("test-cancellable.jar", "test-cancellable", "test-cancellable", None, "fixture.FixturePlugin"),
    ("wrong-info.jar", "test-mismatch", "test-mismatch", None, "fixture.FixturePlugin"),
    ("bad-api.jar", "test-bad-api", "test-bad-api", "0.999", "fixture.FixturePlugin"),
    ("missing-main.jar", "test-missing-main", "test-missing-main", None, "fixture.DoesNotExist"),
    ("zz_duplicate.jar", "test-echo", "test-echo", None, "fixture.FixturePlugin"),
]


def api_version(explicit, root):
    if explicit:
        return explicit
    version = (root / "SERVER_VERSION").read_text(encoding="utf-8").strip()
    match = re.fullmatch(r"0\.([0-9]+)\.[0-9]+(-[0-9A-Za-z.-]+)?", version)
    if not match:
        raise SystemExit(f"SERVER_VERSION must be 0.<core>.<patch>[-qualifier]: {version}")
    return f"0.{match.group(1)}{match.group(2) or ''}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--api", required=True, help="ztserver-api jar")
    parser.add_argument("--source", default=str(Path(__file__).resolve().parent / "fixtures"))
    parser.add_argument("--output", required=True, help="directory receiving the fixture jars")
    parser.add_argument("--api-version", help="API version (defaults to SERVER_VERSION)")
    parser.add_argument("--javac", help="javac executable (defaults to PATH)")
    options = parser.parse_args()

    root = Path(__file__).resolve().parents[3]
    api = Path(options.api).resolve()
    if not api.is_file():
        raise SystemExit(f"API jar not found: {api}")
    javac = options.javac or shutil.which("javac")
    if not javac:
        raise SystemExit("javac not found in PATH; pass --javac")
    output = Path(options.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    api_version_value = api_version(options.api_version, root)

    with tempfile.TemporaryDirectory(prefix="zt-fixtures-") as temporary:
        classes = Path(temporary) / "classes"
        classes.mkdir()
        sources = sorted(Path(options.source).glob("*.java"))
        if not sources:
            raise SystemExit("No fixture sources found")
        subprocess.run(
            [javac, "-encoding", "UTF-8", "-classpath", str(api), "-d", str(classes)]
            + [str(source) for source in sources],
            check=True,
        )
        compiled = sorted(path for path in classes.rglob("*.class"))
        for file_name, plugin_id, name, override_api, main_class in VARIANTS:
            manifest = {
                "formatVersion": 1,
                "host": "server",
                "id": plugin_id,
                "name": name,
                "version": "1.0.0",
                "apiVersion": override_api or api_version_value,
                "mainClass": main_class,
            }
            archive = output / file_name
            with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as jar:
                for compiled_file in compiled:
                    jar.write(compiled_file, compiled_file.relative_to(classes).as_posix())
                jar.writestr("META-INF/ztserver-plugin.json",
                             json.dumps(manifest, ensure_ascii=False, indent=2))
            print(archive)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as failure:
        sys.exit(failure.returncode)

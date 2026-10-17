"""Launcher regression tests; run with python3 src/server-old/tests/StartupTests.py."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class StartupTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="zt-server startup ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.launcher = self.root / "start-server.sh"
        source = Path(__file__).resolve().parents[1] / "main/start-server.sh"
        shutil.copyfile(source, self.launcher)
        self.server = self.root / "zt-server"
        self.server.write_text(
            "#!/usr/bin/python3\nimport json, os, sys\n"
            "print(json.dumps({'cwd': os.getcwd(), 'args': sys.argv[1:]}))\n"
            "sys.exit(int(os.environ.get('TEST_SERVER_STATUS', '0')))\n"
        )
        self.server.chmod(0o755)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.ldd = self.bin / "ldd"
        self.ldd.write_text(
            '#!/bin/bash\nprintf "%s\\n" "${TEST_LDD_OUTPUT:-libQt6Network.so.6 => /lib/libQt6Network.so.6}"\n'
            'exit "${TEST_LDD_STATUS:-0}"\n'
        )
        self.ldd.chmod(0o755)
        for command in ("dirname", "cat"):
            (self.bin / command).symlink_to(shutil.which(command))
        self.environment = dict(os.environ, PATH=str(self.bin))

    def run_launcher(self, *args, **environment):
        return subprocess.run(
            ["/bin/bash", str(self.launcher), *args], cwd="/",
            env=dict(self.environment, **environment), capture_output=True, text=True,
        )

    def test_default_service_and_directory_with_spaces(self):
        result = self.run_launcher()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), {
            "cwd": str(self.root),
            "args": ["--server", "--listen", "0.0.0.0", "--port", "18100"],
        })

    def test_arguments_and_exit_status_are_preserved(self):
        result = self.run_launcher("--server", "--port", "18200", "space argument", TEST_SERVER_STATUS="9")
        self.assertEqual(result.returncode, 9)
        self.assertEqual(json.loads(result.stdout)["args"], ["--server", "--port", "18200", "space argument"])

    def test_check_only_does_not_start_server(self):
        result = self.run_launcher("--check-deps", TEST_SERVER_STATUS="9")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("检查通过", result.stdout)
        self.assertNotIn('"args"', result.stdout)

    def test_missing_library_blocks_start_even_when_ldd_returns_zero(self):
        result = self.run_launcher(TEST_LDD_OUTPUT="libQt6Network.so.6 => not found")
        self.assertEqual(result.returncode, 78)
        self.assertIn("libQt6Network.so.6 => not found", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_version_mismatch_blocks_start(self):
        result = self.run_launcher(TEST_LDD_OUTPUT="libc.so.6: version `GLIBC_2.38' not found", TEST_LDD_STATUS="1")
        self.assertEqual(result.returncode, 78)
        self.assertIn("GLIBC_2.38", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_other_ldd_failure_blocks_start(self):
        result = self.run_launcher(TEST_LDD_OUTPUT="not a dynamic executable", TEST_LDD_STATUS="1")
        self.assertEqual(result.returncode, 78)
        self.assertIn("not a dynamic executable", result.stderr)

    def test_missing_ldd(self):
        self.ldd.unlink()
        result = self.run_launcher()
        self.assertEqual(result.returncode, 78)
        self.assertIn("缺少 ldd", result.stderr)

    def test_missing_or_nonexecutable_server(self):
        self.server.chmod(0o644)
        self.assertEqual(self.run_launcher().returncode, 78)
        self.server.unlink()
        self.assertEqual(self.run_launcher().returncode, 78)

    def test_check_only_rejects_service_arguments(self):
        self.assertEqual(self.run_launcher("--check-deps", "--server").returncode, 64)


if __name__ == "__main__":
    unittest.main()

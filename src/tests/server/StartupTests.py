"""Launcher regression tests for src/server/scripts/start-server.sh.

Run on Linux/WSL: python3 src/tests/server/StartupTests.py
The script is exercised with a fake `java` so no JDK 25 is required for this suite.
"""
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
        source = Path(__file__).resolve().parents[2] / "server/scripts/start-server.sh"
        shutil.copyfile(source, self.launcher)
        self.launcher.chmod(0o755)
        self.jar = self.root / "zt-server.jar"
        self.jar.write_bytes(b"fake jar")
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.fake_java = self.bin / "java"
        self.fake_java.write_text(
            "#!/bin/bash\n"
            "if [[ \"${1:-}\" == -version ]]; then\n"
            "  if [[ \"${TEST_JAVA_INVALID:-}\" == 1 ]]; then\n"
            "    echo 'unrecognized runtime' >&2\n"
            "  else\n"
            "    echo \"openjdk version \\\"${TEST_JAVA_VERSION:-25.0.2}\\\" 2026-01-20 LTS\" >&2\n"
            "  fi\n"
            "  exit 0\n"
            "fi\n"
            "python3 - \"$@\" <<'PY'\n"
            "import json, os, sys\n"
            "print(json.dumps({'cwd': os.getcwd(), 'args': sys.argv[1:]}))\n"
            "sys.exit(int(os.environ.get('TEST_SERVER_STATUS', '0')))\n"
            "PY\n"
        )
        self.fake_java.chmod(0o755)
        for command in ("dirname", "sed", "head", "python3", "cat"):
            executable = shutil.which(command)
            if executable:
                (self.bin / command).symlink_to(executable)
        self.environment = dict(os.environ, PATH=f"{self.bin}:/usr/bin:/bin",
                                JAVA_HOME="")
        self.environment.pop("JAVA_HOME", None)

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
            "args": ["-jar", str(self.jar), "--server", "--listen", "0.0.0.0",
                     "--port", "18100"],
        })

    def test_arguments_and_exit_status_are_preserved(self):
        result = self.run_launcher("--server", "--port", "18200", "space argument",
                                   TEST_SERVER_STATUS="9")
        self.assertEqual(result.returncode, 9)
        self.assertEqual(json.loads(result.stdout)["args"],
                         ["-jar", str(self.jar), "--server", "--port", "18200",
                          "space argument"])

    def test_check_only_does_not_start_server(self):
        result = self.run_launcher("--check-deps", TEST_SERVER_STATUS="9")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("检查通过", result.stdout)
        self.assertNotIn('"args"', result.stdout)

    def test_check_only_rejects_service_arguments(self):
        self.assertEqual(self.run_launcher("--check-deps", "--server").returncode, 64)

    def test_missing_jar_blocks_start(self):
        self.jar.unlink()
        result = self.run_launcher()
        self.assertEqual(result.returncode, 78)
        self.assertIn("zt-server.jar", result.stderr)

    def test_missing_java_blocks_start(self):
        self.fake_java.unlink()
        result = self.run_launcher(PATH=str(self.bin))
        self.assertEqual(result.returncode, 78)
        self.assertIn("Java", result.stderr)

    def test_old_java_version_blocks_start(self):
        result = self.run_launcher(TEST_JAVA_VERSION="21.0.1")
        self.assertEqual(result.returncode, 78)
        self.assertIn("JDK 25", result.stderr)
        self.assertIn("temurin-25-jdk", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_unrecognized_java_version_blocks_start(self):
        result = self.run_launcher(TEST_JAVA_INVALID="1")
        self.assertEqual(result.returncode, 78)
        self.assertIn("无法识别 Java 版本", result.stderr)

    def test_java_home_is_preferred(self):
        home = self.root / "jdk-home"
        (home / "bin").mkdir(parents=True)
        marker = home / "bin" / "java"
        marker.write_text(self.fake_java.read_text())
        marker.chmod(0o755)
        environment = dict(self.environment)
        environment.pop("PATH")
        result = subprocess.run(
            ["/bin/bash", str(self.launcher)], cwd="/",
            env=dict(environment, JAVA_HOME=str(home), PATH=f"{self.bin}:/usr/bin:/bin"),
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["cwd"], str(self.root))


if __name__ == "__main__":
    unittest.main()

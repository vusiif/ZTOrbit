"""Run the standalone installer script with sandboxed paths and fake system commands.

Run on Linux/WSL: python3 src/tests/server/InstallerTests.py --script src/server/scripts/zt-installer.sh
No real ZeroTier/ztncui/pylon installation or uninstallation is performed.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

OPTIONS = None


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.home = self.root / "zt-home"
        (self.home / "moons.d").mkdir(parents=True)
        (self.home / "identity.public").write_text("public-identity")
        (self.home / "identity.secret").write_text("private-identity")
        (self.home / "authtoken.secret").write_text("test-token")
        (self.home / "moons.d" / "0000000123456789.moon").write_bytes(b"signed-moon")
        self.bin = self.root / "bin"
        self.bin.mkdir()
        (self.root / "ztncui.state").write_text("running")
        commands = {
            "systemctl": 'echo "$*" >> "$MOCK_SYSTEMCTL_LOG"; echo zerotier-one.service',
            "ss": '''if [ "$1" = -ltn ]; then
    echo 'LISTEN 0 5 0.0.0.0:9993 0.0.0.0:*'
    if [ -n "${MOCK_BUSY_TCP_PORT:-}" ]; then
        echo "LISTEN 0 5 0.0.0.0:$MOCK_BUSY_TCP_PORT 0.0.0.0:*"
    fi
else
    echo 'UNCONN 0 0 0.0.0.0:9993 0.0.0.0:* users:(("zerotier-one",pid=1,fd=1))'
fi''',
            "zerotier-cli": "echo '200 info 0123456789 1.0 ONLINE'",
            "zerotier-idtool": "exit 0",
            "curl": "exit 0",
            "docker": '''printf '%s\\n' "$*" >> "$MOCK_DOCKER_LOG"
case "$1" in
    info) if [ "${MOCK_DOCKER_UNAVAILABLE:-0}" = 1 ]; then exit 1; fi ;;
    ps)
        if [ -f "$MOCK_ZTNCUI_STATE" ]; then echo ztncui; fi
        if [ -f "$MOCK_PYLON_STATE" ]; then
            if [ "${2:-}" = -a ] || [ "$(cat "$MOCK_PYLON_STATE")" = running ]; then
                echo ztpylon
            fi
        fi
        ;;
    image)
        if [ "${3:-}" = zerotier/pylon:latest ] && [ -f "$MOCK_PYLON_IMAGE" ]; then
            exit 0
        fi
        exit 1
        ;;
    container)
        if [ ! -f "$MOCK_PYLON_STATE" ]; then exit 1; fi
        case "$4" in
            *State.Status*) cat "$MOCK_PYLON_STATE" ;;
            *HostPort*)
                if [ -f "$MOCK_PYLON_PORT" ]; then cat "$MOCK_PYLON_PORT"; else echo 9443; fi
                ;;
        esac
        ;;
    run)
        if [ "${MOCK_FAIL_PYLON_RUN:-0}" = 1 ]; then exit 1; fi
        echo "${7%:443}" > "$MOCK_PYLON_PORT"
        if [ "${MOCK_FAIL_PYLON_BIND:-0}" = 1 ]; then
            echo created > "$MOCK_PYLON_STATE"
            exit 1
        fi
        if [ "${MOCK_PYLON_EXITS:-0}" = 1 ]; then
            echo stopped > "$MOCK_PYLON_STATE"
        else
            echo running > "$MOCK_PYLON_STATE"
        fi
        touch "$MOCK_PYLON_IMAGE"
        ;;
    start) echo running > "$MOCK_PYLON_STATE" ;;
    rm)
        case "${3:-$2}" in
            ztpylon)
                if [ "${MOCK_FAIL_PYLON_REMOVE:-0}" = 1 ]; then exit 1; fi
                rm -f -- "$MOCK_PYLON_STATE" "$MOCK_PYLON_PORT"
                ;;
            ztncui) rm -f -- "$MOCK_ZTNCUI_STATE" ;;
        esac
        ;;
    rmi)
        if [ "${3:-}" = zerotier/pylon:latest ]; then
            if [ "${MOCK_FAIL_PYLON_IMAGE_REMOVE:-0}" = 1 ]; then exit 1; fi
            rm -f -- "$MOCK_PYLON_IMAGE"
        fi
        ;;
esac''',
            "ufw": 'echo "$*" >> "$MOCK_UFW_LOG"; echo "Status: active"',
            "sleep": "exit 0",
            "dpkg": "exit 0",
            "apt-get": "if [ \"${MOCK_FAIL_PACKAGE:-0}\" = 1 ]; then exit 1; fi; "
                       "rm -f -- \"$MOCK_BIN/zerotier-cli\"",
        }
        for name, body in commands.items():
            path = self.bin / name
            path.write_text("#!/bin/bash\n" + body + "\n")
            path.chmod(0o755)
        text = Path(OPTIONS.script).read_text()
        text = text.replace('/var/lib/zerotier-one', str(self.home))
        for path in ['/etc/apt/sources.list.d/zerotier.list',
                     '/usr/share/keyrings/zerotier-debian-package-key.gpg',
                     '/etc/yum.repos.d/zerotier.repo']:
            text = text.replace(path, str(self.root / path.lstrip('/')))
        text = text.replace('command -v zerotier-cli >/dev/null 2>&1',
                            '[[ -f "$MOCK_BIN/zerotier-cli" ]]')
        text = text.replace('command -v docker >/dev/null 2>&1',
                            '[[ -f "$MOCK_BIN/docker" ]]')
        self.script = self.root / "installer.sh"
        self.script.write_text(text)
        self.env = dict(os.environ,
                        PATH=f"{self.bin}:/usr/bin:/bin:/usr/sbin:/sbin",
                        MOCK_BIN=str(self.bin),
                        MOCK_DOCKER_LOG=str(self.root / "docker.log"),
                        MOCK_SYSTEMCTL_LOG=str(self.root / "systemctl.log"),
                        MOCK_UFW_LOG=str(self.root / "ufw.log"),
                        MOCK_PYLON_STATE=str(self.root / "pylon.state"),
                        MOCK_PYLON_IMAGE=str(self.root / "pylon.image"),
                        MOCK_PYLON_PORT=str(self.root / "pylon.port"),
                        MOCK_ZTNCUI_STATE=str(self.root / "ztncui.state"))

    def tearDown(self):
        self.temp.cleanup()

    def run_action(self, action, *extra_args):
        return subprocess.run(["/bin/bash", str(self.script), "--action", str(action),
                               "--ip", "198.51.100.1", *extra_args], cwd=self.root, env=self.env,
                              capture_output=True, text=True, timeout=10)

    def run_menu(self, input_text):
        # Use stdin in the sandbox even if a controlling terminal is available.
        self.script.write_text(self.script.read_text().replace(
            'if [[ -r /dev/tty ]]; then', 'if false; then'))
        return subprocess.run(["/bin/bash", str(self.script), "--ip", "198.51.100.1"],
                              input=input_text, cwd=self.root, env=self.env,
                              capture_output=True, text=True, timeout=10)

    def docker_calls(self, command):
        return [line.split() for line in (self.root / "docker.log").read_text().splitlines()
                if line.split()[0] == command]

    def assert_pylon_run(self, port=9443):
        self.assertEqual(self.docker_calls("run"), [[
            "run", "--name", "ztpylon", "--init", "-d", "-p", f"{port}:443",
            "zerotier/pylon:latest", "reflect",
        ]])

    def seed_pylon(self, state="running", port=9443):
        (self.root / "pylon.state").write_text(state)
        (self.root / "pylon.image").touch()
        (self.root / "pylon.port").write_text(str(port))

    def test_full_install_includes_pylon(self):
        result = self.run_action(1)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_pylon_run()
        self.assertEqual((self.root / "0000000123456789.moon").read_bytes(), b"signed-moon")
        self.assertTrue((self.home / "ztncui" / "passwd").is_file())
        self.assertIn("allow 9443/tcp", (self.root / "ufw.log").read_text().splitlines())

    def test_pylon_only_needs_no_zerotier_or_ztncui_image(self):
        (self.bin / "zerotier-cli").unlink()
        (self.bin / "zerotier-idtool").unlink()
        self.home.rename(self.root / "existing-zt-home")
        result = self.run_action(4)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_pylon_run()
        self.assertFalse(self.home.exists())
        self.assertFalse((self.root / "0000000123456789.moon").exists())
        self.assertNotIn("keynetworks/ztncui", (self.root / "docker.log").read_text())
        self.assertNotIn("zerotier-one", (self.root / "systemctl.log").read_text())
        self.assertIn("9443/TCP -> 443/TCP", result.stdout)

    def test_pylon_custom_tcp_port(self):
        result = self.run_action(4, "--pylon-port", "8443")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_pylon_run(8443)
        self.assertIn("allow 8443/tcp", (self.root / "ufw.log").read_text().splitlines())
        self.assertIn("8443/TCP -> 443/TCP", result.stdout)

    def test_pylon_custom_tcp_port_equals_form_normalizes_leading_zeros(self):
        result = self.run_action(4, "--pylon-port=08443")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_pylon_run(8443)

    def test_pylon_invalid_port_is_rejected_before_system_changes(self):
        for value in ("", "0", "-1", "65536", "abc", "12345678901234567890"):
            with self.subTest(port=value):
                result = self.run_action(4, "--pylon-port", value)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("--pylon-port", result.stderr)
                self.assertFalse((self.root / "docker.log").exists())

    def test_pylon_rejects_zerotier_tcp_api_port_before_container_creation(self):
        result = self.run_action(4, "--pylon-port", "9993")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("9993/TCP 已被占用", result.stderr)
        self.assertEqual(self.docker_calls("run"), [])
        self.assertEqual(self.docker_calls("rm"), [])

    def test_pylon_force_keeps_existing_container_when_new_port_is_busy(self):
        self.seed_pylon(port=8443)
        self.env["MOCK_BUSY_TCP_PORT"] = "9443"
        result = self.run_action(4, "--force")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("9443/TCP 已被占用", result.stderr)
        self.assertEqual((self.root / "pylon.state").read_text(), "running")
        self.assertEqual(self.docker_calls("rm"), [])

    def test_pylon_running_container_port_change_requires_force(self):
        self.seed_pylon(port=8443)
        result = self.run_action(4)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--force", result.stderr)
        self.assertEqual(self.docker_calls("rm"), [])
        self.assertEqual(self.docker_calls("run"), [])

    def test_pylon_failed_created_container_is_remapped_on_retry(self):
        self.seed_pylon(state="created", port=9993)
        result = self.run_action(4)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.docker_calls("rm"), [["rm", "-f", "ztpylon"]])
        self.assert_pylon_run()
        self.assertEqual((self.root / "pylon.port").read_text().strip(), "9443")

    def test_pylon_failed_bind_cleans_created_container_and_allows_retry(self):
        self.env["MOCK_FAIL_PYLON_BIND"] = "1"
        result = self.run_action(4)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "pylon.state").exists())
        self.assertEqual(self.docker_calls("rm"), [["rm", "ztpylon"]])
        self.env["MOCK_FAIL_PYLON_BIND"] = "0"
        result = self.run_action(4)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.root / "pylon.state").read_text().strip(), "running")

    def test_pylon_running_container_is_reused(self):
        for _ in range(2):
            result = self.run_action(4)
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_pylon_run()
        self.assertEqual(self.docker_calls("start"), [])
        self.assertEqual(self.docker_calls("rm"), [])

    def test_pylon_stopped_container_is_started(self):
        (self.root / "pylon.state").write_text("stopped")
        result = self.run_action(4)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.docker_calls("start"), [["start", "ztpylon"]])
        self.assertEqual(self.docker_calls("run"), [])

    def test_pylon_force_recreates_existing_container(self):
        (self.root / "pylon.state").write_text("running")
        result = self.run_action(4, "--force")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.docker_calls("rm"), [["rm", "-f", "ztpylon"]])
        self.assert_pylon_run()

    def test_pylon_run_failure_is_reported(self):
        self.env["MOCK_FAIL_PYLON_RUN"] = "1"
        result = self.run_action(4)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("pylon 容器启动失败", result.stderr)

    def test_pylon_early_exit_is_reported_with_logs(self):
        self.env["MOCK_PYLON_EXITS"] = "1"
        result = self.run_action(4)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("pylon 容器启动异常", result.stderr)
        self.assertEqual(self.docker_calls("logs"), [["logs", "--tail", "50", "ztpylon"]])

    def test_pylon_menu_option_is_available(self):
        result = self.run_menu("4\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("4) 仅安装pylon", result.stdout)
        self.assert_pylon_run()

    def test_shifted_moon_export_keeps_identity_and_containers(self):
        result = self.run_action(5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.root / "0000000123456789.moon").read_bytes(), b"signed-moon")
        self.assertEqual((self.home / "identity.secret").read_text(), "private-identity")
        self.assertEqual(self.docker_calls("run"), [])
        self.assertEqual(self.docker_calls("rm"), [])

    def test_full_uninstall_removes_zerotier_ztncui_and_pylon(self):
        (self.home / "ztncui").mkdir()
        (self.home / "ztncui" / "passwd").write_text("test-password")
        self.seed_pylon()
        result = self.run_action(8)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.home.exists())
        self.assertFalse((self.root / "pylon.state").exists())
        self.assertFalse((self.root / "pylon.image").exists())
        self.assertEqual(self.docker_calls("rm"), [
            ["rm", "-f", "ztncui"], ["rm", "-f", "ztpylon"],
        ])
        self.assertEqual(self.docker_calls("rmi"), [["rmi", "-f", "zerotier/pylon:latest"]])

    def test_pylon_uninstall_keeps_zerotier_and_ztncui(self):
        (self.home / "ztncui").mkdir()
        (self.home / "ztncui" / "passwd").write_text("preserved")
        for state in ("running", "stopped"):
            with self.subTest(state=state):
                self.seed_pylon(state)
                result = self.run_action(7)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertFalse((self.root / "pylon.state").exists())
                self.assertFalse((self.root / "pylon.image").exists())
                self.assertEqual((self.home / "identity.secret").read_text(), "private-identity")
                self.assertEqual((self.home / "ztncui" / "passwd").read_text(), "preserved")
                self.assertTrue((self.root / "ztncui.state").is_file())
                self.assertEqual(self.docker_calls("rm")[-1], ["rm", "-f", "ztpylon"])
                self.assertEqual(self.docker_calls("rmi")[-1], ["rmi", "-f", "zerotier/pylon:latest"])
                self.assertEqual(self.docker_calls("run"), [])
                self.assertNotIn("节点身份已重置", result.stdout)

    def test_pylon_uninstall_needs_no_zerotier(self):
        self.seed_pylon()
        (self.bin / "zerotier-cli").unlink()
        self.home.rename(self.root / "existing-zt-home")
        result = self.run_action(7)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.home.exists())
        self.assertFalse((self.root / "pylon.state").exists())
        self.assertFalse((self.root / "pylon.image").exists())

    def test_pylon_uninstall_can_be_repeated(self):
        self.seed_pylon()
        for _ in range(2):
            result = self.run_action(7)
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.docker_calls("rm"), [["rm", "-f", "ztpylon"]])
        self.assertEqual(self.docker_calls("rmi"), [["rmi", "-f", "zerotier/pylon:latest"]])

    def test_pylon_uninstall_menu_confirmation(self):
        self.seed_pylon()
        result = self.run_menu("7\ny\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("7) 仅卸载pylon", result.stdout)
        self.assertIn("pylon 容器 ztpylon 与镜像 zerotier/pylon:latest", result.stdout)
        self.assertFalse((self.root / "pylon.state").exists())
        self.assertTrue((self.home / "identity.secret").is_file())

    def test_pylon_uninstall_without_docker_is_a_noop(self):
        (self.bin / "docker").unlink()
        result = self.run_action(7)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("未安装 Docker", result.stdout)
        self.assertTrue((self.home / "identity.secret").is_file())
        self.assertFalse((self.root / "docker.log").exists())

    def test_pylon_uninstall_reports_unavailable_docker(self):
        self.seed_pylon()
        self.env["MOCK_DOCKER_UNAVAILABLE"] = "1"
        result = self.run_action(7)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Docker 服务不可用", result.stdout)
        self.assertTrue((self.root / "pylon.state").is_file())
        self.assertTrue((self.home / "identity.secret").is_file())
        self.assertEqual(self.docker_calls("rm"), [])

    def test_full_uninstall_stops_before_zerotier_if_pylon_removal_fails(self):
        self.seed_pylon()
        self.env["MOCK_FAIL_PYLON_REMOVE"] = "1"
        result = self.run_action(8)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("删除容器失败", result.stdout)
        self.assertTrue((self.root / "pylon.state").is_file())
        self.assertTrue((self.home / "identity.secret").is_file())
        self.assertEqual(self.docker_calls("rmi"), [])

    def test_pylon_image_removal_failure_is_reported(self):
        self.seed_pylon()
        self.env["MOCK_FAIL_PYLON_IMAGE_REMOVE"] = "1"
        result = self.run_action(7)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("删除镜像失败", result.stdout)
        self.assertFalse((self.root / "pylon.state").exists())
        self.assertTrue((self.root / "pylon.image").is_file())
        self.assertTrue((self.home / "identity.secret").is_file())

    def test_shifted_uninstall_menu_options_require_confirmation(self):
        for action in (6, 7, 8):
            with self.subTest(action=action):
                result = self.run_menu(f"{action}\nn\n")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("确认继续？", result.stdout)
                self.assertIn("已取消", result.stdout)
                self.assertTrue((self.home / "identity.secret").is_file())
                self.assertFalse((self.root / "docker.log").exists())

    def test_zerotier_install_exports_existing_moon_and_preserves_identity(self):
        result = self.run_action(2)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.root / "0000000123456789.moon").read_bytes(), b"signed-moon")
        self.assertEqual((self.home / "identity.secret").read_text(), "private-identity")

    def test_ztncui_install_uninstall_keeps_zerotier(self):
        self.seed_pylon()
        result = self.run_action(3)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.home / "ztncui" / "passwd").is_file())
        result = self.run_action(6)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.home / "ztncui").exists())
        self.assertTrue((self.home / "identity.secret").is_file())
        self.assertTrue((self.root / "pylon.state").is_file())
        self.assertTrue((self.root / "pylon.image").is_file())

    def test_package_failure_reports_error_before_data_deletion(self):
        self.env['MOCK_FAIL_PACKAGE'] = '1'
        result = self.run_action(9)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue((self.home / "identity.secret").is_file())

    def test_zerotier_only_uninstall_preserves_ztncui_config_and_pylon(self):
        (self.home / "ztncui").mkdir()
        (self.home / "ztncui" / "passwd").write_text("preserved")
        self.seed_pylon()
        result = self.run_action(9)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.home / "identity.secret").exists())
        self.assertEqual((self.home / "ztncui" / "passwd").read_text(), "preserved")
        self.assertTrue((self.root / "pylon.state").is_file())
        self.assertTrue((self.root / "pylon.image").is_file())
        self.assertEqual(self.docker_calls("rm"), [])
        self.assertEqual(self.docker_calls("rmi"), [])


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--script', required=True)
    OPTIONS = parser.parse_args()
    unittest.main(argv=[__file__], verbosity=2)

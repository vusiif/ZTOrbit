"""Black-box tests for the Java ZTServer host, plugins and ZTS1 wire protocol.

The tests never touch real ZeroTier: an in-process mock HTTP service provides read-only query
responses and the server is started in a temporary working directory with fixture plugin jars.

Example:
  python3 src/tests/server/build-fixtures.py --api src/server/api/target/ztserver-api-0.2.jar \
      --output /tmp/zt-fixtures
  python3 src/tests/server/IntegrationTests.py --binary src/server/main/target/zt-server.jar \
      --fixtures /tmp/zt-fixtures
"""
import argparse
import http.server
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest

OPTIONS = None
TOKEN = "fixture-zt-token"


def spawn_options():
    if os.name == "nt":
        return {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP}
    return {}


def graceful_stop(process):
    if process is None or process.poll() is not None:
        return
    try:
        if os.name == "nt":
            process.send_signal(signal.CTRL_BREAK_EVENT)
        else:
            process.terminate()
        process.wait(timeout=15)
    except (subprocess.TimeoutExpired, OSError):
        process.kill()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass


def packet(route, payload):
    route = route.encode()
    return b"ZTS1" + bytes([1, len(route)]) + route + payload


def frame(route, payload):
    data = packet(route, payload)
    return struct.pack("!I", len(data)) + data


def read_frame(reader):
    prefix = reader.read(4)
    if len(prefix) != 4:
        raise EOFError()
    length = struct.unpack("!I", prefix)[0]
    if length > 1048576:
        raise ValueError("Oversized frame")
    data = reader.read(length)
    if len(data) != length or data[:5] != b"ZTS1\1":
        raise ValueError("Bad frame")
    size = data[5]
    return data[6:6 + size].decode(), data[6 + size:]


class ZeroTierMock(http.server.ThreadingHTTPServer):
    daemon_threads = True


class ZeroTierHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_GET(self):
        if self.headers.get("X-ZT1-Auth") != TOKEN:
            self.send_response(401)
            self.end_headers()
            return
        path = self.path
        if path == "/status":
            body = {"address": "0123456789", "version": "1.14.0", "online": True}
        elif path in ("/peer", "/network", "/moon"):
            body = []
        elif path == "/controller/network":
            body = {}
        elif path.startswith("/controller/network/") and "/member/" in path:
            body = {"address": path.rsplit("/", 1)[-1], "name": "member",
                    "authorized": True, "ipAssignments": []}
        elif path.endswith("/member"):
            body = {}
        else:
            self.send_response(404)
            self.end_headers()
            return
        encoded = json.dumps(body).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)


class IntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mock = ZeroTierMock(("127.0.0.1", 0), ZeroTierHandler)
        cls.mock_thread = threading.Thread(target=cls.mock.serve_forever, daemon=True)
        cls.mock_thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.mock.shutdown()
        cls.mock.server_close()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        (self.root / "plugins").mkdir()
        for source in sorted(Path(OPTIONS.fixtures).glob("*.jar")):
            shutil.copyfile(source, self.root / "plugins" / source.name)
        (self.root / "plugins" / "unsupported.ztplugin").write_bytes(b"old package")
        self.process = None
        if OPTIONS.host is None:
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", 0))
                self.port = probe.getsockname()[1]
            self.host = "127.0.0.1"
            self.command = self.prepare_command()
            self.process = subprocess.Popen(
                self.command + ["--server", "--listen", self.host, "--port", str(self.port)],
                cwd=self.root,
                env=dict(os.environ,
                         ZTSERVER_ZT_API=f"http://127.0.0.1:{self.mock.server_port}",
                         ZTSERVER_ZT_TOKEN=TOKEN),
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                **spawn_options(),
            )
            for _ in range(200):
                try:
                    with socket.create_connection((self.host, self.port), timeout=0.2):
                        break
                except OSError:
                    if self.process.poll() is not None:
                        self.fail("Server exited during startup")
                    time.sleep(0.02)
            else:
                self.fail("Server startup timeout")
            time.sleep(0.25)
        else:
            self.host, self.port = OPTIONS.host, OPTIONS.port
            self.command = None
        self.connection = socket.create_connection((self.host, self.port), timeout=2)
        self.reader = self.connection.makefile("rb")

    def prepare_command(self):
        binary = Path(OPTIONS.binary).resolve()
        if binary.suffix == ".jar":
            return [java(), "-jar", str(binary)]
        if binary.suffix == ".sh":
            if not OPTIONS.jar:
                self.fail("--jar is required when --binary is a launcher script")
            launcher = self.root / "start-server.sh"
            shutil.copyfile(binary, launcher)
            shutil.copyfile(Path(OPTIONS.jar).resolve(), self.root / "zt-server.jar")
            return ["/bin/bash", str(launcher)]
        return [str(binary)]

    def tearDown(self):
        try:
            self.reader.close()
            self.connection.close()
        except OSError:
            pass
        self.stop_server()
        self.temp.cleanup()

    def stop_server(self):
        graceful_stop(self.process)

    def test_tcp_routes_split_and_coalesced(self):
        message = frame("test-echo", b"binary\0payload")
        self.connection.sendall(message[:3])
        self.connection.sendall(message[3:])
        self.assertEqual(read_frame(self.reader), ("test-echo", b"binary\0payload"))
        self.connection.sendall(frame("test-other", b"other") + frame("test-echo", b"echo"))
        self.assertEqual(read_frame(self.reader), ("test-other", b"other"))
        self.assertEqual(read_frame(self.reader), ("test-echo", b"echo"))
        for route in ("test-failed", "missing-route", "test-bad-api"):
            self.connection.sendall(frame(route, b"no"))
            reply_route, body = read_frame(self.reader)
            result = json.loads(body)
            self.assertEqual(reply_route, "builtin")
            self.assertFalse(result["ok"])
            self.assertEqual(result["error"], "unknown_or_unready_plugin")
            self.assertEqual(result["plugin"], route)

    def test_udp_limits_and_isolation(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            udp.settimeout(0.3)
            destination = ("127.0.0.1", self.port)
            udp.sendto(packet("test-other", b"datagram"), destination)
            self.assertEqual(udp.recvfrom(1233)[0], packet("test-other", b"datagram"))
            for route, body in [
                ("unknown", b"no"),
                ("builtin", b'{"op":"network.probe"}'),
                ("test-echo", b"x" * 1233),
                ("test-failed", b"no"),
            ]:
                udp.sendto(packet(route, body), destination)
                with self.assertRaises(socket.timeout):
                    udp.recvfrom(1233)
            udp.sendto(packet("test-echo", b"still-alive"), destination)
            self.assertEqual(udp.recvfrom(1233)[0], packet("test-echo", b"still-alive"))

    def test_rejections_cleanup_and_dual_logs(self):
        self.connection.sendall(frame("test-echo", b"hello"))
        self.assertEqual(read_frame(self.reader), ("test-echo", b"hello"))
        self.connection.close()
        self.reader.close()
        self.stop_server()
        data = self.root / "plugins-data" / "test-echo"
        self.assertEqual((data / "stopped").read_bytes(), b"yes")
        self.assertTrue((data / "closed").is_file())
        host_log = "".join(path.read_text(encoding="utf-8", errors="replace")
                           for path in (self.root / "logs").glob("*.log"))
        plugin_log = "".join(path.read_text(encoding="utf-8", errors="replace")
                             for path in (data / "logs").glob("*.log"))
        self.assertIn("fixture-start", host_log)
        self.assertIn("fixture-start", plugin_log)
        self.assertIn("[test-echo]", plugin_log)
        self.assertIn("Rejected package", host_log)
        self.assertIn("Duplicate plugin ID", host_log)
        self.assertIn("unsupported package, use .jar", host_log)
        self.assertIn("0.999", host_log)
        self.assertIn("does not match host API", host_log)
        self.assertIn("plugin info does not match manifest", host_log)
        pending = self.root / "plugins-data" / "test-cancellable"
        self.assertEqual((pending / "cancelled").read_bytes(), b"yes")
        self.assertEqual((pending / "capture-destroyed").read_bytes(), b"yes")
        self.assertEqual((pending / "stopped").read_bytes(), b"yes")
        self.assertNotIn(TOKEN, host_log)
        self.assertNotIn(TOKEN, plugin_log)

    def test_bad_framing_and_query_errors(self):
        for prefix in (b"\x00\x10\x00\x01", b"\x00\x00\x00\x07garbage"):
            with socket.create_connection((self.host, self.port), timeout=2) as connection:
                connection.sendall(prefix)
                try:
                    self.assertEqual(connection.recv(32), b"")
                except ConnectionResetError:
                    pass
        for body in (b"{not-json}", b'{"id":1,"op":"info"}', b'{"v":2,"id":1,"op":"info"}'):
            self.connection.sendall(frame("builtin", body))
            try:
                self.assertEqual(self.connection.recv(32), b"")
            except ConnectionResetError:
                pass
            self.connection = socket.create_connection((self.host, self.port), timeout=2)
            self.reader = self.connection.makefile("rb")
        self.connection.sendall(frame("builtin", json.dumps(
            {"v": 1, "id": 5, "op": "invalid", "data": {}}).encode()))
        route, body = read_frame(self.reader)
        result = json.loads(body)
        self.assertEqual(route, "builtin")
        self.assertEqual(result["id"], 5)
        self.assertFalse(result["ok"])
        self.connection.sendall(frame("builtin", json.dumps(
            {"v": 1, "id": "query", "op": "info", "data": {}}).encode()))
        route, body = read_frame(self.reader)
        result = json.loads(body)
        self.assertEqual(result["id"], "query")
        self.assertTrue(result["ok"], result)
        self.assertEqual(result["data"]["address"], "0123456789")
        self.connection.sendall(frame("builtin", json.dumps(
            {"v": 1, "id": 6, "op": "controller.members",
             "data": {"networkId": "bad"}}).encode()))
        _, body = read_frame(self.reader)
        self.assertEqual(json.loads(body)["error"], "Invalid network ID")

    def test_rate_limit_closes_connection(self):
        with socket.create_connection((self.host, self.port), timeout=2) as connection:
            connection.settimeout(5)
            burst = b"".join(frame("test-echo", b"x") for _ in range(61))
            connection.sendall(burst)
            received = 0
            reader = connection.makefile("rb")
            try:
                while True:
                    read_frame(reader)
                    received += 1
            except (EOFError, ConnectionResetError, BrokenPipeError):
                pass
            self.assertLessEqual(received, 60)

    def run_cli(self, *arguments):
        if self.command is None:
            self.skipTest("CLI tests need a locally started server")
        return subprocess.run(self.command + list(arguments), cwd=self.root, capture_output=True,
                              text=True, timeout=30)

    def test_cli_validation_and_installer_mapping(self):
        version = (Path(__file__).resolve().parents[3] / "SERVER_VERSION").read_text().strip()
        result = self.run_cli("--version")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), version)
        for arguments in (("--install", "zerotier"), ("-u", "all"), ("--moon",)):
            result = self.run_cli(*arguments)
            self.assertEqual(result.returncode, 2)
            self.assertIn("Install/uninstall is handled by ./zt-installer.sh; see DEPLOYMENT.md",
                          result.stderr)
        for arguments in (("--port", "0", "--server"),
                          ("--listen", "::1", "--server"),
                          ("--listen", "not-an-ip", "--server"),
                          ("--server", "--bogus")):
            self.assertEqual(self.run_cli(*arguments).returncode, 2, arguments)
        with socket.socket() as blocker:
            blocker.bind(("127.0.0.1", 0))
            blocker.listen(1)
            port = blocker.getsockname()[1]
            result = self.run_cli("--server", "--listen", "127.0.0.1", "--port", str(port))
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn("bind failed", result.stderr)


def java():
    executable = shutil.which("java")
    if executable:
        return executable
    home = os.environ.get("JAVA_HOME")
    if home:
        candidate = Path(home) / "bin" / ("java.exe" if os.name == "nt" else "java")
        if candidate.is_file():
            return str(candidate)
    raise SystemExit("java not found")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", help="zt-server.jar, start-server.sh or native binary")
    parser.add_argument("--jar", help="zt-server.jar used when --binary is a launcher script")
    parser.add_argument("--fixtures", required=True, help="directory with fixture plugin jars")
    parser.add_argument("--host", help="test an already deployed server instead of starting one")
    parser.add_argument("--port", type=int, default=18100)
    OPTIONS = parser.parse_args()
    if OPTIONS.host is None and not OPTIONS.binary:
        parser.error("--binary is required unless --host is given")
    unittest.main(argv=[__file__], verbosity=2)

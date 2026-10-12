"""Isolated root/Linux host tests; never install/uninstall real ZeroTier."""
import argparse
import json
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import unittest

OPTIONS = None
def packet(route, payload):
    route = route.encode()
    return b"ZTS1" + bytes([1, len(route)]) + route + payload
def frame(route, payload):
    data = packet(route, payload)
    return struct.pack("!I", len(data)) + data
def read_frame(reader):
    prefix = reader.read(4)
    if len(prefix) != 4: raise EOFError()
    length = struct.unpack("!I", prefix)[0]
    if length > 1048576: raise ValueError("Oversized frame")
    data = reader.read(length)
    if len(data) != length or data[:5] != b"ZTS1\1": raise ValueError("Bad frame")
    size = data[5]
    return data[6:6+size].decode(), data[6+size:]

class IntegrationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        (self.root/"plugins").mkdir()
        for source in Path(OPTIONS.fixtures).glob("*.so"):
            name = "zz_duplicate.so" if "duplicate" in source.name else source.name
            shutil.copyfile(source,self.root/"plugins"/name)
        with socket.socket() as probe:
            probe.bind(("127.0.0.1",0)); self.port = probe.getsockname()[1]
        self.process = subprocess.Popen([OPTIONS.binary,"--server","--listen","127.0.0.1","--port",str(self.port)],cwd=self.root)
        for _ in range(100):
            try:
                self.connection = socket.create_connection(("127.0.0.1",self.port),timeout=2)
                self.reader = self.connection.makefile("rb")
                break
            except OSError:
                if self.process.poll() is not None: self.fail("Startup failed")
                time.sleep(.02)
        else: self.fail("Startup timeout")
        time.sleep(.15)
    def tearDown(self):
        self.reader.close(); self.connection.close()
        if self.process.poll() is None:
            self.process.terminate(); self.process.wait(timeout=5)
        self.temp.cleanup()
    def test_tcp_routes_split_and_coalesced(self):
        message = frame("test.echo",b"binary\0payload")
        self.connection.sendall(message[:3]); self.connection.sendall(message[3:])
        self.assertEqual(read_frame(self.reader),("test.echo",b"binary\0payload"))
        self.connection.sendall(frame("test.other",b"other")+frame("test.echo",b"echo"))
        self.assertEqual(read_frame(self.reader),("test.other",b"other"))
        self.assertEqual(read_frame(self.reader),("test.echo",b"echo"))
        self.connection.sendall(frame("test.failed",b"no"))
        route,body=read_frame(self.reader)
        self.assertEqual(route,"builtin"); self.assertFalse(json.loads(body)["ok"])
    def test_udp_limits_and_isolation(self):
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as udp:
            udp.settimeout(.2); destination=("127.0.0.1",self.port)
            udp.sendto(packet("test.other",b"datagram"),destination)
            self.assertEqual(udp.recvfrom(1233)[0],packet("test.other",b"datagram"))
            for route,body in [("unknown",b"no"),("builtin",b"no"),("test.echo",b"x"*1233)]:
                udp.sendto(packet(route,body),destination)
                with self.assertRaises(socket.timeout): udp.recvfrom(1233)
            udp.sendto(packet("test.echo",b"still-alive"),destination)
            self.assertEqual(udp.recvfrom(1233)[0],packet("test.echo",b"still-alive"))
    def test_rejections_cleanup_and_dual_logs(self):
        self.connection.sendall(frame("test.echo",b"hello")); read_frame(self.reader)
        self.connection.close(); self.reader.close()
        self.process.terminate(); self.process.wait(timeout=5)
        data=self.root/"plugins-data"/"test.echo"
        self.assertEqual((data/"stopped").read_bytes(),b"yes")
        self.assertTrue((data/"closed").is_file())
        main="".join(p.read_text() for p in (self.root/"logs").glob("*.log"))
        plugin="".join(p.read_text() for p in (data/"logs").glob("*.log"))
        self.assertIn("fixture-start",main); self.assertIn("fixture-start",plugin)
        self.assertIn("Rejected plugin metadata",main); self.assertIn("duplicate",main)
        pending=self.root/"plugins-data"/"test.cancellable"
        self.assertEqual((pending/"cancelled").read_bytes(),b"yes")
        self.assertEqual((pending/"capture-destroyed").read_bytes(),b"yes")
        self.assertEqual((pending/"stopped").read_bytes(),b"yes")
    def test_bad_framing_and_query_errors(self):
        for prefix in [b"\x00\x10\x00\x01",b"\x00\x00\x00\x07garbage"]:
            with socket.create_connection(("127.0.0.1",self.port),timeout=2) as connection:
                connection.sendall(prefix)
                try: self.assertEqual(connection.recv(32),b"")
                except ConnectionResetError: pass
        self.connection.sendall(frame("builtin",json.dumps({"v":1,"id":5,"op":"invalid","data":{}}).encode()))
        route,body=read_frame(self.reader); result=json.loads(body)
        self.assertEqual(route,"builtin"); self.assertEqual(result["id"],5); self.assertFalse(result["ok"])
    def test_cli_validation_and_installer_mapping(self):
        def run(*args): return subprocess.run([OPTIONS.binary,*args],capture_output=True,text=True)
        for args in [("--server","--moon"),("--port","0","--server"),("--install","invalid"),("--uninstall","all")]:
            self.assertNotEqual(run(*args).returncode,0)
        for mode,target,action in [("--install","all",1),("-i","zerotier",2),("--install","ztncui",3),("-u","ztncui",5),("--uninstall","all",6),("-u","zerotier",7)]:
            result=run(mode,target,"--dry-run"); self.assertEqual(result.returncode,0,result.stderr)
            self.assertIn(f"--action {action}",result.stdout)
        self.assertEqual(run("--moon","--dry-run").returncode,0)
        with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as blocker:
            blocker.bind(("127.0.0.1",0)); port=blocker.getsockname()[1]
            self.assertNotEqual(run("--server","--listen","127.0.0.1","--port",str(port)).returncode,0)

if __name__ == "__main__":
    parser=argparse.ArgumentParser(); parser.add_argument("--binary",required=True); parser.add_argument("--fixtures",required=True)
    OPTIONS=parser.parse_args(); unittest.main(argv=[__file__],verbosity=2)

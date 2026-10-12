"""Run the embedded script with sandboxed paths and fake system commands."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
OPTIONS=None
class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(); self.root=Path(self.temp.name)
        self.home=self.root/"zt-home"; (self.home/"moons.d").mkdir(parents=True)
        (self.home/"identity.public").write_text("public-identity")
        (self.home/"identity.secret").write_text("private-identity")
        (self.home/"authtoken.secret").write_text("test-token")
        (self.home/"moons.d"/"0000000123456789.moon").write_bytes(b"signed-moon")
        self.bin=self.root/"bin"; self.bin.mkdir()
        commands={
            "systemctl":"echo zerotier-one.service",
            "ss":"echo 'UNCONN 0 0 0.0.0.0:9993 0.0.0.0:* users:((\"zerotier-one\",pid=1,fd=1))'",
            "zerotier-cli":"echo '200 info 0123456789 1.0 ONLINE'",
            "zerotier-idtool":"exit 0",
            "curl":"exit 0",
            "docker":"case \"$1\" in ps) echo ztncui;; *) exit 0;; esac",
            "ufw":"echo 'Status: inactive'",
            "dpkg":"exit 0",
            "apt-get":"if [ \"${MOCK_FAIL_PACKAGE:-0}\" = 1 ]; then exit 1; fi; rm -f -- \"$MOCK_BIN/zerotier-cli\"",
        }
        for name,body in commands.items():
            path=self.bin/name; path.write_text("#!/bin/bash\n"+body+"\n"); path.chmod(0o755)
        text=Path(OPTIONS.script).read_text()
        text=text.replace('/var/lib/zerotier-one',str(self.home))
        for path in ['/etc/apt/sources.list.d/zerotier.list','/usr/share/keyrings/zerotier-debian-package-key.gpg','/etc/yum.repos.d/zerotier.repo']:
            text=text.replace(path,str(self.root/path.lstrip('/')))
        text=text.replace('command -v zerotier-cli >/dev/null 2>&1','[[ -f "$MOCK_BIN/zerotier-cli" ]]')
        self.script=self.root/"installer.sh"; self.script.write_text(text)
        self.env=dict(os.environ,PATH=str(self.bin)+":/usr/bin:/bin:/usr/sbin:/sbin",MOCK_BIN=str(self.bin))
    def tearDown(self): self.temp.cleanup()
    def run_action(self,action):
        return subprocess.run(["/bin/bash",str(self.script),"--action",str(action),"--ip","198.51.100.1"],cwd=self.root,env=self.env,capture_output=True,text=True,timeout=10)
    def test_zerotier_install_exports_existing_moon_and_preserves_identity(self):
        result=self.run_action(2); self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual((self.root/"0000000123456789.moon").read_bytes(),b"signed-moon")
        self.assertEqual((self.home/"identity.secret").read_text(),"private-identity")
    def test_ztncui_install_uninstall_keeps_zerotier(self):
        result=self.run_action(3); self.assertEqual(result.returncode,0,result.stderr)
        self.assertTrue((self.home/"ztncui"/"passwd").is_file())
        result=self.run_action(5); self.assertEqual(result.returncode,0,result.stderr)
        self.assertFalse((self.home/"ztncui").exists()); self.assertTrue((self.home/"identity.secret").is_file())
    def test_package_failure_reports_error_before_data_deletion(self):
        self.env['MOCK_FAIL_PACKAGE']='1'
        result=self.run_action(7); self.assertNotEqual(result.returncode,0)
        self.assertTrue((self.home/"identity.secret").is_file())
    def test_zerotier_only_uninstall_preserves_ztncui_config(self):
        (self.home/"ztncui").mkdir(); (self.home/"ztncui"/"passwd").write_text("preserved")
        result=self.run_action(7); self.assertEqual(result.returncode,0,result.stderr)
        self.assertFalse((self.home/"identity.secret").exists())
        self.assertEqual((self.home/"ztncui"/"passwd").read_text(),"preserved")
if __name__=='__main__':
    parser=argparse.ArgumentParser(); parser.add_argument('--script',required=True); OPTIONS=parser.parse_args()
    unittest.main(argv=[__file__],verbosity=2)

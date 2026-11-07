#!/usr/bin/env python3
"""Exercise the publishing transaction against isolated directories and real PE metadata."""
import base64
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('publish_update', REPO / '.agents/skills/zto-build/scripts/publish-update.py')
publisher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publisher)
CLIENT = (REPO / 'build/dist/Z2-windows-x64/Z2.exe').read_bytes()
VERSION = (REPO / 'VERSION').read_text().strip()


class PublishTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for directory in ('tools', 'conf', 'uploads', 'secrets'):
            (self.root / directory).mkdir(mode=0o700)
        shutil.copyfile(REPO / 'scripts/update-manifest.py', self.root / 'tools/update-manifest.py')
        fixtures = REPO / 'src/tests/update/fixtures/keys'
        shutil.copyfile(fixtures / 'update-signing.key', self.root / 'secrets/update-signing.key')
        (self.root / 'secrets/update-signing.key').chmod(0o600)
        shutil.copyfile(fixtures / 'update-signing.pub', self.root / 'conf/update-signing.pub')
        (self.root / 'conf/publish.json').write_text(json.dumps({
            'baseUrl': 'http://10.10.10.1:18102', 'minUpdatableVersion': VERSION}))
        self.source = self.root / 'uploads/Z2-windows-x64.zip'
        self.activations = []

    def package(self, extra=None, wrapped=True):
        prefix = 'Z2-windows-x64/' if wrapped else ''
        files = {'Z2.exe': CLIENT, 'z2_updater.exe': b'helper payload', 'Qt6Core.dll': b'runtime'}
        files.update(extra or {})
        with zipfile.ZipFile(self.source, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            for name, data in files.items():
                archive.writestr(prefix + name, data)

    def publish(self, **options):
        publisher.publish(self.root, self.source, VERSION,
                          activate_release=lambda *values: self.activations.append(values), **options)

    def test_real_pe_version(self):
        self.assertEqual(publisher.pe_version(CLIENT), VERSION)
        with self.assertRaises(ValueError):
            publisher.pe_version(b'MZ' + b'\0' * 200)

    def test_wrapped_package_is_signed_and_published(self):
        self.package()
        old_umask = os.umask(0o077)
        try:
            self.publish()
        finally:
            os.umask(old_umask)
        package = self.root / f'www/z2/releases/windows-x64/{VERSION}/Z2-{VERSION}-windows-x64.zip'
        metadata = self.root / f'www/api/windows-x64/{VERSION}/manifest.json'
        with zipfile.ZipFile(package) as archive:
            self.assertEqual(archive.namelist(), ['Z2.exe', 'z2_updater.exe', 'Qt6Core.dll'])
            self.assertEqual(archive.read('Z2.exe'), CLIENT)
        signer, _, public = publisher.signer_for(self.root)
        signature = base64.b64decode(metadata.with_suffix('.json.sig').read_bytes())
        self.assertTrue(signer.verify_detached(public, metadata.read_bytes(), signature))
        info = json.loads(metadata.read_bytes())
        self.assertEqual(info['artifacts'][0]['sha256'], publisher.file_hash(package))
        self.assertEqual(info['artifacts'][0]['url'], f'http://10.10.10.1:18102/file?version={VERSION}')
        self.assertEqual(package.stat().st_mode & 0o777, 0o644)
        self.assertEqual(metadata.parent.stat().st_mode & 0o777, 0o755)
        self.assertEqual(package.parent.stat().st_mode & 0o777, 0o755)
        self.assertFalse(list((self.root / 'work').iterdir()))
        self.assertEqual(len(self.activations), 1)

    def test_flat_input_and_identical_republish_are_supported(self):
        self.package(wrapped=False)
        self.publish()
        metadata = self.root / f'www/api/windows-x64/{VERSION}/manifest.json'
        old = metadata.read_bytes()
        self.package(wrapped=True)
        self.publish()
        self.assertEqual(metadata.read_bytes(), old)

    def test_changed_existing_version_is_rejected(self):
        self.package()
        self.publish()
        self.package({'Qt6Core.dll': b'changed runtime'})
        with self.assertRaisesRegex(ValueError, '已发布且文件内容不同'):
            self.publish()
        self.assertEqual(len(self.activations), 1)

    def test_version_must_match_executable(self):
        self.package()
        with self.assertRaisesRegex(ValueError, '版本不匹配'):
            publisher.publish(self.root, self.source, '0.99.0', activate_release=lambda *args: None)
        self.assertFalse((self.root / 'www').exists())

    def test_check_only_does_not_publish(self):
        self.package()
        self.publish(check_only=True)
        self.assertFalse((self.root / 'www').exists())
        self.assertFalse(self.activations)

    def test_key_mismatch_does_not_publish(self):
        self.package()
        (self.root / 'conf/update-signing.pub').write_text(base64.b64encode(bytes(32)).decode())
        with self.assertRaisesRegex(ValueError, '公钥不匹配'):
            self.publish()
        self.assertFalse((self.root / 'www').exists())

    def test_unsafe_files_are_rejected(self):
        for name in ('settings.ini', 'logs/test.log', '../escape', 'cache/entry', 'CON.txt', 'C:/evil'):
            with self.subTest(name=name):
                self.package({name: b'data'})
                with self.assertRaises(ValueError):
                    self.publish()
                self.assertFalse(self.activations)

    def test_case_collision_and_links_are_rejected(self):
        self.package({'z2.EXE': b'conflict'})
        with self.assertRaisesRegex(ValueError, '重复'):
            self.publish()
        self.package()
        with zipfile.ZipFile(self.source, 'a') as archive:
            info = zipfile.ZipInfo('Z2-windows-x64/link')
            info.create_system = 3
            info.external_attr = 0o120777 << 16
            archive.writestr(info, 'elsewhere')
        with self.assertRaisesRegex(ValueError, '链接'):
            self.publish()

    def test_failed_activation_restores_both_config_files(self):
        active = self.root / 'conf/active-release.conf'
        file = self.root / 'conf/active-file.conf'
        active.write_bytes(b'old updater config')
        file.write_bytes(b'old file config')
        calls = []

        def run(command, **kwargs):
            calls.append(command)
            if command[:2] == ['systemctl', 'show']:
                return subprocess.CompletedProcess(command, 0, stdout='123\n')
            if command[0] == 'nsenter':
                raise subprocess.CalledProcessError(1, command, stderr=b'invalid nginx config')
            return subprocess.CompletedProcess(command, 0)

        with patch.object(publisher.subprocess, 'run', side_effect=run):
            with self.assertRaises(subprocess.CalledProcessError):
                publisher.activate(self.root, VERSION, b'{}', bytes(64), 'http://10.10.10.1:18102')
        self.assertEqual(active.read_bytes(), b'old updater config')
        self.assertEqual(file.read_bytes(), b'old file config')
        self.assertEqual(calls[-1], ['systemctl', 'reload', 'z2-updater.service'])
        self.assertFalse(list((self.root / 'conf').glob('.publish-*')))

    def test_version_ordering(self):
        self.assertGreater(publisher.compare_versions('0.5.10', '0.5.9'), 0)
        self.assertGreater(publisher.compare_versions('0.5.3', '0.5.3-rc.1'), 0)
        self.assertLess(publisher.compare_versions('0.5.3-rc.2', '0.5.3-rc.10'), 0)
        with self.assertRaises(ValueError):
            publisher.version_parts('../0.5.3')

    def test_file_argument_is_required_even_if_old_default_exists(self):
        self.package()
        result = subprocess.run([sys.executable, publisher.__file__, '--root', str(self.root),
                                 '--version', VERSION, '--check'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn('--file', result.stderr)
        self.assertFalse((self.root / '.publish.lock').exists())

    def test_file_can_be_relative_to_caller_or_absolute_outside_uploads(self):
        self.package()
        caller = self.root / 'other directory'
        caller.mkdir()
        source = caller / 'example file.zip'
        self.source.rename(source)
        for argument in (source.name, str(source)):
            with self.subTest(argument=argument):
                result = subprocess.run([sys.executable, publisher.__file__, '--root', str(self.root),
                                         '--version', VERSION, '--file', argument, '--check'],
                                        cwd=caller, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('校验通过', result.stdout)
        self.assertTrue(source.is_file())
        self.assertFalse((self.root / 'www').exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)

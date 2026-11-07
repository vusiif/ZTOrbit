#!/usr/bin/env python3
"""Prepare, sign and publish an uploaded Z2 distribution ZIP on the update host."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import base64
import fcntl
import hashlib
import importlib.util
import ipaddress
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import tempfile
import time
import urllib.request
from urllib.parse import urlsplit
import zipfile

MAX_PACKAGE = 512 * 1024 * 1024
MAX_EXPANDED = 1024 * 1024 * 1024
PROTECTED = {'settings.ini', 'plugins', 'plugins-data', 'logs', 'cache', 'updates', '.zto'}
VERSION = re.compile(r'0\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?\Z')
DEVICE = re.compile(r'(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?\Z', re.IGNORECASE)


def version_parts(value: str):
    match = VERSION.fullmatch(value)
    if not match:
        raise ValueError(f'版本无效：{value}')
    prerelease = match[3].split('.') if match[3] else []
    if any(part.isdigit() and len(part) > 1 and part.startswith('0') for part in prerelease):
        raise ValueError(f'预发布版本含前导零：{value}')
    return (0, int(match[1]), int(match[2])), prerelease


def compare_versions(left: str, right: str) -> int:
    numeric_left, pre_left = version_parts(left)
    numeric_right, pre_right = version_parts(right)
    if numeric_left != numeric_right:
        return 1 if numeric_left > numeric_right else -1
    if not pre_left or not pre_right:
        return (not pre_left) - (not pre_right)
    for a, b in zip(pre_left, pre_right):
        if a == b:
            continue
        if a.isdigit() and b.isdigit():
            return 1 if int(a) > int(b) else -1
        if a.isdigit() != b.isdigit():
            return -1 if a.isdigit() else 1
        return 1 if a > b else -1
    return (len(pre_left) > len(pre_right)) - (len(pre_left) < len(pre_right))


def safe_name(name: str) -> str:
    if not name or len(name) > 240 or '\\' in name:
        raise ValueError(f'包内路径无效：{name!r}')
    parts = name.split('/')
    for part in parts:
        if (not part or part in {'.', '..'} or part.endswith((' ', '.'))
                or DEVICE.fullmatch(part)
                or any(ord(c) < 32 or c in ':<>"|?*' for c in part)):
            raise ValueError(f'包内路径不安全：{name!r}')
    if parts[0].casefold() in PROTECTED:
        raise ValueError(f'ZIP 含用户数据：{name}')
    return name


def entries(archive: zipfile.ZipFile):
    infos = archive.infolist()
    if not 1 <= len(infos) <= 4096:
        raise ValueError('ZIP 条目数量超限')
    wrapped = all(info.filename.startswith('Z2-windows-x64/') for info in infos)
    files, seen, total = [], {}, 0
    for info in infos:
        name = info.filename.removeprefix('Z2-windows-x64/') if wrapped else info.filename
        if not name and info.is_dir():
            continue
        name = safe_name(name.rstrip('/') if info.is_dir() else name)
        folded = name.casefold()
        if folded in seen:
            raise ValueError(f'ZIP 路径重复：{name}')
        seen[folded] = info.is_dir()
        if (info.flag_bits & (1 | 0x20 | 0x40 | 0x2000)
                or info.compress_type not in {zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED}
                or stat.S_ISLNK(info.external_attr >> 16) or info.external_attr & 0x400):
            raise ValueError(f'ZIP 含加密、链接或不支持的条目：{name}')
        if (info.file_size > MAX_PACKAGE or info.file_size < 0
                or info.file_size > info.compress_size * 1000 + 1024 * 1024):
            raise ValueError(f'ZIP 解压大小或压缩比超限：{name}')
        if not info.is_dir():
            total += info.file_size
            files.append((info, name))
    if total > MAX_EXPANDED:
        raise ValueError('ZIP 总解压大小超限')
    for name in seen:
        parts = name.split('/')
        if any(seen.get('/'.join(parts[:i])) is False for i in range(1, len(parts))):
            raise ValueError(f'ZIP 文件和目录冲突：{name}')
    if not {'z2.exe', 'z2_updater.exe'} <= {name.casefold() for _, name in files}:
        raise ValueError('ZIP 缺少 Z2.exe 或 z2_updater.exe')
    return files


def pe_version(data: bytes) -> str:
    """Read the FileVersion string from the PE RT_VERSION resource, not strings in code."""
    def number(offset, size=4):
        if offset < 0 or offset + size > len(data):
            raise ValueError('Z2.exe PE 数据越界')
        return int.from_bytes(data[offset:offset + size], 'little')

    if data[:2] != b'MZ':
        raise ValueError('Z2.exe 不是 Windows 可执行文件')
    pe = number(0x3c)
    if data[pe:pe + 4] != b'PE\0\0':
        raise ValueError('Z2.exe PE 签名无效')
    section_count, optional_size = number(pe + 6, 2), number(pe + 20, 2)
    optional, magic = pe + 24, number(pe + 24, 2)
    directory = optional + (112 if magic == 0x20b else 96 if magic == 0x10b else 0)
    if directory == optional or directory + 24 > optional + optional_size or not 1 <= section_count <= 96:
        raise ValueError('Z2.exe PE 头无效')

    def offset(rva, size):
        for i in range(section_count):
            section = optional + optional_size + i * 40
            address, raw_size, raw_offset = number(section + 12), number(section + 16), number(section + 20)
            if address <= rva and rva + size <= address + raw_size:
                result = raw_offset + rva - address
                if result + size <= len(data):
                    return result
        raise ValueError('Z2.exe 资源地址无效')

    resource_size = number(directory + 20)
    resource_base = offset(number(directory + 16), resource_size)
    if not 16 <= resource_size <= 16 * 1024 * 1024:
        raise ValueError('Z2.exe 资源大小无效')

    def resource_number(relative, size=4):
        if relative < 0 or relative + size > resource_size:
            raise ValueError('Z2.exe 版本资源越界')
        return number(resource_base + relative, size)

    def directory_entries(relative):
        count = resource_number(relative + 12, 2) + resource_number(relative + 14, 2)
        if count > 4096:
            raise ValueError('Z2.exe 资源目录超限')
        return [(resource_number(relative + 16 + i * 8), resource_number(relative + 20 + i * 8))
                for i in range(count)]

    versions, visited = [], set()

    def read_blocks(blob):
        values = []

        def block(start, limit, depth):
            if depth > 8 or start + 6 > limit:
                raise ValueError('Z2.exe 版本信息结构无效')
            length, value_length, kind = struct.unpack_from('<HHH', blob, start)
            end, cursor = start + length, start + 6
            if length < 6 or end > limit:
                raise ValueError('Z2.exe 版本信息长度无效')
            key_end = cursor
            while key_end + 2 <= end and blob[key_end:key_end + 2] != b'\0\0':
                key_end += 2
            if key_end + 2 > end:
                raise ValueError('Z2.exe 版本信息键无效')
            key = blob[cursor:key_end].decode('utf-16-le')
            value_start = (key_end + 2 + 3) & ~3
            value_end = value_start + value_length * (2 if kind == 1 else 1)
            if value_end > end:
                raise ValueError('Z2.exe 版本信息值越界')
            if key == 'FileVersion':
                values.append(blob[value_start:value_end].decode('utf-16-le').rstrip('\0'))
            cursor = (value_end + 3) & ~3
            while cursor + 6 <= end:
                child_length = struct.unpack_from('<H', blob, cursor)[0]
                if not child_length:
                    break
                block(cursor, end, depth + 1)
                cursor = (cursor + child_length + 3) & ~3

        block(0, len(blob), 0)
        return values

    def walk(relative, depth):
        if relative in visited or depth > 2:
            raise ValueError('Z2.exe 版本资源目录无效')
        visited.add(relative)
        for _, target in directory_entries(relative):
            if target & 0x80000000:
                walk(target & 0x7fffffff, depth + 1)
            else:
                size = resource_number(target + 4)
                if not 1 <= size <= 1024 * 1024:
                    raise ValueError('Z2.exe 版本资源大小无效')
                start = offset(resource_number(target), size)
                versions.extend(read_blocks(data[start:start + size]))

    for kind, target in directory_entries(0):
        if kind == 16 and target & 0x80000000:
            walk(target & 0x7fffffff, 0)
    if not versions or len(set(versions)) != 1:
        raise ValueError('Z2.exe 缺少唯一 FileVersion')
    version_parts(versions[0])
    return versions[0]


def file_hash(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def tree_hashes(path: Path):
    with zipfile.ZipFile(path) as archive:
        result = {}
        for info, name in entries(archive):
            with archive.open(info) as stream:
                result[name] = (info.file_size, hashlib.file_digest(stream, 'sha256').hexdigest())
        return result


def normalize_package(source: Path, target: Path, version: str):
    if source.is_symlink() or not source.is_file() or not 1 <= source.stat().st_size <= MAX_PACKAGE:
        raise ValueError('上传 ZIP 不存在、是链接或超过 512 MiB')
    with zipfile.ZipFile(source) as archive:
        files = entries(archive)
        executable = next(info for info, name in files if name.casefold() == 'z2.exe')
        embedded = pe_version(archive.read(executable))
        if embedded != version:
            raise ValueError(f'版本不匹配：Z2.exe 为 {embedded}，命令指定 {version}')
        with zipfile.ZipFile(target, 'x', compression=zipfile.ZIP_DEFLATED,
                             compresslevel=6, allowZip64=False) as output:
            for info, name in files:
                item = zipfile.ZipInfo(name, info.date_time)
                item.create_system, item.external_attr = 0, 0x20
                item.compress_type = zipfile.ZIP_DEFLATED
                with archive.open(info) as reader, output.open(item, 'w') as writer:
                    shutil.copyfileobj(reader, writer, 1024 * 1024)
    if target.stat().st_size > MAX_PACKAGE:
        raise ValueError('生成的更新 ZIP 超过 512 MiB')


def signer_for(root: Path):
    spec = importlib.util.spec_from_file_location('z2_update_signer', root / 'tools/update-manifest.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    private = root / 'secrets/update-signing.key'
    if private.is_symlink() or private.stat().st_mode & 0o077:
        raise ValueError('签名私钥必须是仅当前账户可读的普通文件（chmod 600）')
    seed = module.read_seed(private)
    public = module.read_public(root / 'conf/update-signing.pub')
    if module.public_from_seed(seed) != public:
        raise ValueError('签名私钥与现有客户端公钥不匹配，拒绝发布')
    return module, seed, public


def configuration(root: Path):
    config = json.loads((root / 'conf/publish.json').read_bytes())
    url = urlsplit(config['baseUrl'])
    if (url.scheme != 'http' or url.username or url.password or url.path or url.query or url.fragment
            or not 1 <= (url.port or 80) <= 65535):
        raise ValueError('conf/publish.json 的 baseUrl 无效')
    ipaddress.IPv4Address(url.hostname)
    version_parts(config['minUpdatableVersion'])
    return config


def atomic_write(path: Path, data: bytes, mode=0o644):
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix='.publish-', delete=False) as stream:
        candidate = Path(stream.name)
        try:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
            os.chmod(candidate, mode)
            os.replace(candidate, path)
        finally:
            candidate.unlink(missing_ok=True)


def public_directory(path: Path, root: Path):
    path.mkdir(parents=True, exist_ok=True)
    for directory in (path, *path.parents):
        if not directory.is_relative_to(root / 'www'):
            break
        if directory.is_symlink():
            raise ValueError(f'发布目录不得为链接：{directory}')
        directory.chmod(0o755)


def activate(root: Path, version: str, manifest: bytes, signature: bytes, base_url: str):
    encoded = base64.b64encode(signature).decode('ascii')
    namespace = '/srv/z2-updater'
    configs = {
        root / 'conf/active-release.conf': (
            'location = /updater {\n'
            '    limit_except GET HEAD { deny all; }\n'
            f'    alias {namespace}/www/api/windows-x64/{version}/manifest.json;\n'
            '    default_type application/json;\n'
            f'    add_header X-Z2-Signature "{encoded}" always;\n'
            '    add_header Cache-Control "no-store" always;\n'
            '    add_header X-Content-Type-Options nosniff always;\n}\n').encode(),
        root / 'conf/active-file.conf': (
            'if ($arg_version = "") { add_header Cache-Control "no-store" always; '
            f'return 302 /file?version={version}; }}\n').encode(),
    }
    previous = {path: path.read_bytes() if path.exists() else None for path in configs}
    pid = subprocess.run(['systemctl', 'show', '-p', 'MainPID', '--value', 'z2-updater.service'],
                         check=True, capture_output=True, text=True).stdout.strip()
    if not pid.isdigit() or int(pid) <= 0:
        raise ValueError('更新服务未运行，请先启动 z2-updater.service')
    validate = ['nsenter', '-t', pid, '-m', '--', '/usr/sbin/nginx', '-t',
                '-c', f'{namespace}/conf/nginx.conf', '-p', '/run/z2-updater']
    reload = ['systemctl', 'reload', 'z2-updater.service']
    try:
        for path, content in configs.items():
            atomic_write(path, content)
        subprocess.run(validate, check=True, capture_output=True)
        subprocess.run(reload, check=True, capture_output=True)
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        last_error = None
        for _ in range(20):
            try:
                with opener.open(base_url + '/updater', timeout=1) as response:
                    if (response.read(len(manifest) + 1) == manifest and response.headers.get('X-Z2-Signature') == encoded):
                        return
            except OSError as error:
                last_error = error
            time.sleep(0.1)
        raise ValueError(f'重载后接口验证失败：{last_error or "仍返回其他版本"}')
    except BaseException:
        for path, content in previous.items():
            if content is None:
                path.unlink(missing_ok=True)
            else:
                atomic_write(path, content)
        subprocess.run(reload, check=True, capture_output=True)
        raise


def publish(root: Path, source: Path, version: str, minimum: str | None = None,
            check_only=False, activate_release=activate):
    version_parts(version)
    config = configuration(root)
    minimum = minimum or config['minUpdatableVersion']
    if compare_versions(minimum, version) > 0:
        raise ValueError('最低可升级版本高于发布版本')
    signer, seed, public = signer_for(root)
    work = root / 'work'
    work.mkdir(mode=0o700, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=work, prefix='publish-') as temporary:
        temporary = Path(temporary)
        normalized = temporary / 'package.zip'
        normalize_package(source, normalized, version)
        package_dir = root / 'www/z2/releases/windows-x64' / version
        package = package_dir / f'Z2-{version}-windows-x64.zip'
        metadata_dir = root / 'www/api/windows-x64' / version
        existing = package.exists()
        if existing and tree_hashes(package) != tree_hashes(normalized):
            raise ValueError(f'{version} 已发布且文件内容不同，请分配新版本并重新编译')
        payload = package if existing else normalized
        artifact = {'platform': 'windows', 'arch': 'x64', 'version': version,
                    'url': config['baseUrl'] + f'/file?version={version}',
                    'size': payload.stat().st_size, 'sha256': file_hash(payload)}
        manifest_file, signature_file = metadata_dir / 'manifest.json', metadata_dir / 'manifest.json.sig'
        if manifest_file.exists() or signature_file.exists():
            manifest = manifest_file.read_bytes()
            signature = base64.b64decode(signature_file.read_text().strip(), validate=True)
            info = json.loads(manifest)
            if (not signer.verify_detached(public, manifest, signature)
                    or info.get('schemaVersion') != 1 or info.get('channel') != 'stable'
                    or info.get('artifacts') != [artifact] or info.get('minUpdatableVersion') != minimum):
                raise ValueError('已发布版本的签名或元数据不匹配，拒绝覆盖')
        else:
            info = {'schemaVersion': 1, 'channel': 'stable',
                    'publishedAt': datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace('+00:00', 'Z'),
                    'minUpdatableVersion': minimum, 'artifacts': [artifact]}
            manifest = (json.dumps(info, ensure_ascii=False, indent=2) + '\n').encode('utf-8')
            signature = signer.sign_seed(seed, manifest)
            if not signer.verify_detached(public, manifest, signature):
                raise ValueError('生成签名验证失败')
        if check_only:
            print(f'校验通过：{version}，{len(entries_from_path(normalized))} 个发行文件')
            return
        if not existing:
            public_directory(package_dir, root)
            os.chmod(normalized, 0o644)
            os.replace(normalized, package)
        public_directory(metadata_dir, root)
        if not manifest_file.exists():
            atomic_write(signature_file, base64.b64encode(signature) + b'\n')
            atomic_write(manifest_file, manifest)
        activate_release(root, version, manifest, signature, config['baseUrl'])
        print(f'已发布 {version}：{artifact["size"]} 字节，/updater 与 /file 已切换')


def entries_from_path(path):
    with zipfile.ZipFile(path) as archive:
        return entries(archive)


def main():
    parser = argparse.ArgumentParser(prog='bash ./update.sh', description='校验、签名并发布指定的 Z2 ZIP')
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parent.parent,
                        help=argparse.SUPPRESS)
    parser.add_argument('--version', required=True)
    parser.add_argument('--file', type=Path, required=True, help='ZIP 路径，可为绝对路径或相对当前工作目录的路径')
    parser.add_argument('--min-version', default=None)
    parser.add_argument('--check', action='store_true', help='仅校验，不切换发布')
    args = parser.parse_args()
    os.umask(0o077)
    root = args.root.resolve()
    source = args.file.expanduser().absolute()
    try:
        with (root / '.publish.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            publish(root, source, args.version, args.min_version, args.check)
    except BlockingIOError:
        parser.exit(1, '已有发布任务正在进行\n')
    except subprocess.CalledProcessError as error:
        detail = error.stderr.decode('utf-8', errors='replace').strip() if isinstance(error.stderr, bytes) else error.stderr
        parser.exit(1, f'发布失败：{detail or error}\n')
    except KeyboardInterrupt:
        parser.exit(130, '发布已取消\n')
    except (ValueError, OSError, zipfile.BadZipFile, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f'发布失败：{error}\n')


if __name__ == '__main__':
    main()

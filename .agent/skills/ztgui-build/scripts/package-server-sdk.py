"""Install and archive the headless SDK only when its inputs or outputs changed."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

root=Path(sys.argv[1]).resolve()
version='.'.join((root/'VERSION').read_text().strip().split('.')[:2])
build=root/'cmake-build-server-api'
sdk=root/'dist'/f'ZTServer-api-v{version}'
archive=sdk.with_suffix(sdk.suffix+'.zip')
state_file=build/'package-state.json'
def fingerprint(path):
    digest=hashlib.sha256()
    for file in sorted(path.rglob('*')):
        if file.is_file():
            digest.update(file.relative_to(path).as_posix().encode()); digest.update(file.read_bytes())
    return digest.hexdigest()
inputs=version+':'+fingerprint(root/'src/server/api')+':'+fingerprint(root/'src/shared/include')+':'+fingerprint(root/'src/shared/package')+':'+hashlib.sha256((root/'src/shared/CMakeLists.txt').read_bytes()).hexdigest()+':'+hashlib.sha256((root/'cmake/Version.cmake').read_bytes()).hexdigest()
try: state=json.loads(state_file.read_text())
except (OSError,ValueError): state={}
changed=not sdk.is_dir() or not (sdk/'lib/cmake/ZTServerPluginApi/ZTServerPluginApiTargets.cmake').is_file() or state.get('inputs')!=inputs or state.get('sdk')!=fingerprint(sdk)
if changed:
    cache = build/'CMakeCache.txt'
    if cache.exists():
        homes = [line.split('=',1)[1] for line in cache.read_text().splitlines() if line.startswith('CMAKE_HOME_DIRECTORY:INTERNAL=')]
        if homes and Path(homes[0]).resolve() != (root/'src/server/api').resolve():
            build.rename(build.with_name(build.name+f'.previous-{time.time_ns()}'))
    subprocess.run(['cmake','-S',str(root/'src/server/api'),'-B',str(build),'-DCMAKE_INSTALL_LIBDIR:STRING=lib'],check=True)
    subprocess.run(['cmake','--build',str(build),'--target','ztplugin_pack','--parallel'],check=True)
    root.joinpath('dist').mkdir(exist_ok=True)
    staging=Path(tempfile.mkdtemp(prefix='.ztserver-api-',dir=root/'dist'))
    subprocess.run(['cmake','--install',str(build),'--prefix',str(staging)],check=True)
    if sdk.exists(): sdk.rename(sdk.with_name(sdk.name+f'.previous-{time.time_ns()}'))
    staging.rename(sdk)
zip_hash=lambda: hashlib.sha256(archive.read_bytes()).hexdigest()
if changed or not archive.is_file() or state.get('archive')!=zip_hash():
    temporary=archive.with_name(archive.name+'.tmp')
    with zipfile.ZipFile(temporary,'w',zipfile.ZIP_DEFLATED) as output:
        for file in sorted(sdk.rglob('*')):
            if file.is_file(): output.write(file,file.relative_to(sdk).as_posix())
    temporary.replace(archive)
    with zipfile.ZipFile(archive) as output:
        if output.testzip() is not None: raise RuntimeError('SDK ZIP verification failed')
build.mkdir(exist_ok=True)
state_file.write_text(json.dumps(dict(inputs=inputs,sdk=fingerprint(sdk),archive=zip_hash()),indent=2)+'\n')
print(('Updated' if changed else 'Reused')+' '+str(archive))

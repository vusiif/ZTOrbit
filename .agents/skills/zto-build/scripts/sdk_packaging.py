"""Helpers for package-server-sdk.py (kept separate so the entry script stays small)."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import zipfile

SAMPLE_POM = """<?xml version="1.0" encoding="UTF-8"?>
<project xmlns="http://maven.apache.org/POM/4.0.0"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         xsi:schemaLocation="http://maven.apache.org/POM/4.0.0 https://maven.apache.org/xsd/maven-4.0.0.xsd">
    <modelVersion>4.0.0</modelVersion>

    <groupId>sample</groupId>
    <artifactId>ztohub-api-sample</artifactId>
    <version>1.0.0</version>

    <properties>
        <maven.compiler.release>25</maven.compiler.release>
        <project.build.sourceEncoding>UTF-8</project.build.sourceEncoding>
    </properties>

    <repositories>
        <repository>
            <id>ztohub-sdk</id>
            <url>file://${project.basedir}/../</url>
        </repository>
    </repositories>

    <dependencies>
        <dependency>
            <groupId>vusiif.ztohub</groupId>
            <artifactId>ztohub-api</artifactId>
            <version>%API_VERSION%</version>
        </dependency>
    </dependencies>
</project>
"""

SAMPLE_SOURCE = """package sample;

import vusiif.ztohub.api.PluginInfo;
import vusiif.ztohub.api.ServerPlugin;

/** Minimal plugin compiled against the installed SDK repository. */
public final class SamplePlugin implements ServerPlugin {
    @Override
    public PluginInfo info() {
        return new PluginInfo("sample", "Sample", "1.0.0");
    }
}
"""

README = """# ZTOrbitHub Plugin API %API_VERSION%

Java 25 SDK for ZTOrbitHub JAR plugins (application %VERSION%). Put the plugin jar in the
server's `plugins/` directory and restart the server; plugins are trusted code and are
not hot-loaded.

## Contents

- `vusiif/ztohub/ztohub-api/%API_VERSION%/` — jar, sources, javadoc and POM (this
  directory is a Maven repository root)
- `sample/` — a minimal Maven plugin project
- `sdk.json` — API/application versions, JDK requirement and build fingerprint

## Build the sample

```bash
mvn -f sample/pom.xml package
```

The sample resolves the SDK from this directory through a `file:` repository. To
verify an installed-repository workflow, copy the repository tree into a temporary
local repository and build with that repository (the copy is required when a Maven
mirror redirects the `file:` repository):

```bash
repo=$(mktemp -d)
cp -R vusiif "$repo"/
mvn -f sample/pom.xml -Dmaven.repo.local="$repo" package
```

The plugin jar must contain `META-INF/ztohub-plugin.json`:

```json
{
  "formatVersion": 1,
  "host": "server",
  "id": "sample",
  "name": "Sample",
  "version": "1.0.0",
  "apiVersion": "%API_VERSION%",
  "mainClass": "sample.SamplePlugin"
}
```

Plugin IDs match `[A-Za-z0-9_-]{1,128}` and may not be `builtin`. The API version must
match the host exactly (`%API_VERSION%`). See `docs/server-api.md` for the complete
lifecycle and threading contract.
"""


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for chunk in iter(lambda: source.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fingerprint_tree(path):
    path = Path(path)
    if not path.is_dir():
        return "missing"
    digest = hashlib.sha256()
    for file in sorted(path.rglob("*")):
        if file.is_file():
            digest.update(file.relative_to(path).as_posix().encode())
            digest.update(file.read_bytes())
    return digest.hexdigest()


def fingerprint_inputs(root, api_version):
    root = Path(root)
    digest = hashlib.sha256()
    digest.update(api_version.encode())
    for relative in ("src/server/api", "src/server/pom.xml", "SERVER_VERSION",
                     ".agents/skills/zto-build/scripts/package-server-sdk.py",
                     ".agents/skills/zto-build/scripts/sdk_packaging.py"):
        item = root / relative
        if item.is_dir():
            digest.update(fingerprint_tree(item).encode())
        elif item.is_file():
            digest.update(item.read_bytes())
    return digest.hexdigest()


def zip_is_valid(path):
    path = Path(path)
    if not path.is_file():
        return False
    try:
        with zipfile.ZipFile(path) as archive:
            return archive.testzip() is None
    except (OSError, zipfile.BadZipFile):
        return False


def write_zip(directory, archive):
    directory = Path(directory)
    archive = Path(archive)
    archive.parent.mkdir(parents=True, exist_ok=True)
    temporary = archive.with_name(archive.name + ".tmp")
    with zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED) as output:
        for file in sorted(directory.rglob("*")):
            if file.is_file():
                output.write(file, file.relative_to(directory).as_posix())
    with zipfile.ZipFile(temporary) as output:
        if output.testzip() is not None:
            temporary.unlink(missing_ok=True)
            raise RuntimeError("SDK ZIP verification failed")
    temporary.replace(archive)


def jackson_version(root):
    pom = (Path(root) / "src/server/pom.xml").read_text(encoding="utf-8")
    match = re.search(r"<jackson\.version>([^<]+)</jackson\.version>", pom)
    if not match:
        raise RuntimeError("jackson.version missing from the parent POM")
    return match.group(1)


def local_repository():
    configured = Path.home() / ".m2" / "settings.xml"
    if configured.is_file():
        text = configured.read_text(encoding="utf-8", errors="replace")
        text = re.sub(r"<!--.*?-->", "", text, flags=re.DOTALL)
        matches = re.findall(r"<localRepository>\s*([^<]+?)\s*</localRepository>", text)
        if matches:
            return Path(matches[-1]).expanduser()
    return Path.home() / ".m2" / "repository"


def jackson_classpath(root, version):
    candidates = sorted(local_repository().glob(
        f"com/fasterxml/jackson/core/jackson-databind/{version}/jackson-databind-{version}.jar"))
    if candidates:
        return str(candidates[0])
    fallback = sorted(local_repository().glob(
        "com/fasterxml/jackson/core/jackson-databind/**/jackson-databind-*.jar"))
    if fallback:
        return str(fallback[-1])
    return ""


def build_sources_jar(api_dir, target):
    root = Path(api_dir)
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as archive:
        for file in sorted((root / "src/main/java").rglob("*.java")):
            archive.write(file, file.relative_to(root / "src/main/java").as_posix())


def build_javadoc_jar(root, api_dir, target, version):
    with tempfile.TemporaryDirectory(prefix="zt-javadoc-") as temporary:
        output = Path(temporary) / "docs"
        sources = sorted((Path(api_dir) / "src/main/java").rglob("*.java"))
        classpath = jackson_classpath(root, version)
        command = ["javadoc", "--release", "25", "-Xdoclint:none", "-quiet",
                   "-d", str(output)]
        if classpath:
            command += ["-classpath", classpath]
        command += [str(source) for source in sources]
        subprocess.run(command, check=True)
        with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as archive:
            for file in sorted(output.rglob("*")):
                if file.is_file():
                    archive.write(file, file.relative_to(output).as_posix())


def build_sdk(root, sdk, api_version, version, api_jar, inputs):
    root = Path(root)
    sdk = Path(sdk)
    api_jar = Path(api_jar)
    if sdk.exists():
        shutil.rmtree(sdk)
    sdk.parent.mkdir(parents=True, exist_ok=True)
    repository = sdk / "vusiif" / "ztohub" / "ztohub-api" / api_version
    repository.mkdir(parents=True)
    jar_name = f"ztohub-api-{api_version}.jar"
    shutil.copyfile(api_jar, repository / jar_name)
    build_sources_jar(root / "src/server/api", repository / f"ztohub-api-{api_version}-sources.jar")
    build_javadoc_jar(root, root / "src/server/api",
                      repository / f"ztohub-api-{api_version}-javadoc.jar",
                      jackson_version(root))
    pom = f"""<?xml version="1.0" encoding="UTF-8"?>
<project xmlns="http://maven.apache.org/POM/4.0.0"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         xsi:schemaLocation="http://maven.apache.org/POM/4.0.0 https://maven.apache.org/xsd/maven-4.0.0.xsd">
    <modelVersion>4.0.0</modelVersion>
    <groupId>vusiif.ztohub</groupId>
    <artifactId>ztohub-api</artifactId>
    <version>{api_version}</version>
    <packaging>jar</packaging>
    <name>ZTOrbitHub Plugin API</name>
    <description>Public Java plugin SDK for ZTOrbitHub (API {api_version}).</description>
    <properties>
        <maven.compiler.release>25</maven.compiler.release>
        <project.build.sourceEncoding>UTF-8</project.build.sourceEncoding>
    </properties>
    <dependencies>
        <dependency>
            <groupId>com.fasterxml.jackson.core</groupId>
            <artifactId>jackson-databind</artifactId>
            <version>{jackson_version(root)}</version>
        </dependency>
    </dependencies>
</project>
"""
    (repository / f"ztohub-api-{api_version}.pom").write_text(pom, encoding="utf-8")
    sample = sdk / "sample"
    (sample / "src/main/java/sample").mkdir(parents=True)
    (sample / "pom.xml").write_text(SAMPLE_POM.replace("%API_VERSION%", api_version),
                                    encoding="utf-8")
    (sample / "src/main/java/sample/SamplePlugin.java").write_text(SAMPLE_SOURCE, encoding="utf-8")
    (sdk / "README.md").write_text(README.replace("%API_VERSION%", api_version)
                                   .replace("%VERSION%", version), encoding="utf-8")
    (sdk / "sdk.json").write_text(json.dumps({
        "apiVersion": api_version,
        "applicationVersion": version,
        "jdk": "25",
        "inputs": inputs,
    }, indent=2) + "\n", encoding="utf-8")

#include "LocalConfig.h"
#include <QAbstractSocket>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

namespace {
using namespace vusiif::zto::zerotier::localconfig;

QString psQuote(const QString &value)
{
    QString escaped = value;
    escaped.replace('\\', '/');
    escaped.replace('\'', "''");
    return '\'' + escaped + '\'';
}

QString shQuote(const QString &value)
{
    QString escaped = value;
    escaped.replace('\'', "'\\''");
    return '\'' + escaped + '\'';
}

bool ipv4(const QString &text, QString &canonical)
{
    const auto trimmed = text.trimmed();
    if (trimmed.isEmpty()) return false;
    QHostAddress address(trimmed);
    bool ok = false;
    const quint32 value = address.toIPv4Address(&ok);
    if (!ok || address.protocol() != QAbstractSocket::IPv4Protocol) return false;
    canonical = QHostAddress(value).toString();
    return true;
}

QString normalizeMoonId(const QString &value)
{
    const auto id = value.trimmed().toLower();
    if (!QRegularExpression("^[0-9a-f]{1,16}$").match(id).hasMatch()) return {};
    return id.rightJustified(16, '0');
}

QString moonIdForNetwork(const QString &networkId)
{
    return QStringLiteral("000000") + networkId.left(10).toLower();
}
}

namespace vusiif::zto::zerotier::localconfig {

QByteArray defaultContent()
{
    const QJsonObject settings{{"allowTcpFallbackRelay", true}, {"forceTcpRelay", false}};
    const QJsonObject root{{"settings", settings}};
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

State parse(const QByteArray &bytes, bool exists)
{
    State state;
    state.exists = exists;
    state.raw = bytes;
    if (!exists) return state;

    QByteArray json = bytes;
    if (json.startsWith("\xEF\xBB\xBF")) json.remove(0, 3);
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        state.error = QStringLiteral("local.conf 不是合法 JSON：%1（偏移 %2）")
                          .arg(parseError.errorString()).arg(parseError.offset);
        return state;
    }
    if (!document.isObject()) {
        state.error = QStringLiteral("local.conf 根值必须是 JSON 对象。");
        return state;
    }
    state.root = document.object();
    if (state.root.contains("settings") && !state.root.value("settings").isObject()) {
        state.error = QStringLiteral("local.conf 的 settings 必须是 JSON 对象。");
        return state;
    }
    const auto settings = state.root.value("settings").toObject();
    if (settings.contains("allowTcpFallbackRelay") && !settings.value("allowTcpFallbackRelay").isBool()) {
        state.error = QStringLiteral("local.conf 的 settings.allowTcpFallbackRelay 必须是布尔值。");
        return state;
    }
    if (settings.contains("forceTcpRelay") && !settings.value("forceTcpRelay").isBool()) {
        state.error = QStringLiteral("local.conf 的 settings.forceTcpRelay 必须是布尔值。");
        return state;
    }
    if (settings.contains("tcpFallbackRelay") && !settings.value("tcpFallbackRelay").isString()) {
        state.error = QStringLiteral("local.conf 的 settings.tcpFallbackRelay 必须是字符串。");
        return state;
    }
    // Missing fields keep their documented defaults for display only. The file
    // is not rewritten just because a default was projected.
    state.allowTcpFallbackRelay = settings.value("allowTcpFallbackRelay").toBool(true);
    state.forceTcpRelay = settings.value("forceTcpRelay").toBool(false);
    state.tcpFallbackRelay = settings.value("tcpFallbackRelay").toString();
    if (!state.tcpFallbackRelay.isEmpty()) {
        QString ip;
        int port = 0;
        if (parseRelay(state.tcpFallbackRelay, ip, port)) {
            state.relayIp = ip;
            state.relayPort = port;
        }
    }
    return state;
}

ReadResult read(const QString &path)
{
    ReadResult result;
    const QFileInfo info(path);
    if (!info.exists()) {
        result.ok = true;
        result.state = State{};
        result.state.error.clear();
        return result;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("无法读取 local.conf：%1").arg(file.errorString());
        result.permissionDenied = file.error() == QFileDevice::PermissionsError || !info.isReadable();
        return result;
    }
    if (file.size() > MaxConfigSize) {
        result.error = QStringLiteral("local.conf 超出 %1 字节上限，已拒绝读取。").arg(MaxConfigSize);
        return result;
    }
    const auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        result.error = QStringLiteral("无法读取 local.conf：%1").arg(file.errorString());
        result.permissionDenied = file.error() == QFileDevice::PermissionsError;
        return result;
    }
    result.state = parse(bytes, true);
    result.ok = result.state.valid();
    result.error = result.state.error;
    return result;
}

WriteResult writeAtomic(const QString &path, const QByteArray &bytes)
{
    WriteResult result;
    const QFileInfo info(path);
    const QFileInfo directory(info.absolutePath());
    if (!directory.exists() || !directory.isDir()) {
        result.error = QStringLiteral("ZeroTier 配置目录不存在：%1").arg(directory.absoluteFilePath());
        return result;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        result.error = QStringLiteral("无法写入 local.conf：%1").arg(file.errorString());
        result.permissionDenied = file.error() == QFileDevice::PermissionsError
            || !directory.isWritable() || (info.exists() && !info.isWritable());
        return result;
    }
    if (file.write(bytes) != bytes.size()) {
        result.error = QStringLiteral("无法写入 local.conf：%1").arg(file.errorString());
        result.permissionDenied = file.error() == QFileDevice::PermissionsError
            || !directory.isWritable() || (info.exists() && !info.isWritable());
        file.cancelWriting();
        return result;
    }
    if (!file.commit()) {
        result.error = QStringLiteral("无法保存 local.conf：%1").arg(file.errorString());
        result.permissionDenied = file.error() == QFileDevice::PermissionsError
            || !directory.isWritable() || (info.exists() && !info.isWritable());
        return result;
    }
    result.ok = true;
    return result;
}

WriteResult createDefault(const QString &path)
{
    const QFileInfo info(path);
    if (info.exists()) {
        WriteResult result;
        result.ok = true;
        return result;
    }
    return writeAtomic(path, defaultContent());
}

bool parseRelay(const QString &text, QString &ip, int &port)
{
    const auto trimmed = text.trimmed();
    const int slash = trimmed.lastIndexOf('/');
    if (slash <= 0 || slash == trimmed.size() - 1) return false;
    if (trimmed.indexOf('/') != slash) return false;
    bool ok = false;
    const int parsedPort = trimmed.mid(slash + 1).toInt(&ok);
    if (!ok || parsedPort < 1 || parsedPort > 65535) return false;
    QString canonical;
    if (!ipv4(trimmed.left(slash), canonical)) return false;
    ip = canonical;
    port = parsedPort;
    return true;
}

bool parseDisplayRelay(const QString &text, QString &ip, int &port)
{
    const auto trimmed = text.trimmed();
    if (trimmed.count(':') != 1) return false;
    const int colon = trimmed.indexOf(':');
    if (colon <= 0 || colon == trimmed.size() - 1) return false;
    bool ok = false;
    const int parsedPort = trimmed.mid(colon + 1).toInt(&ok);
    if (!ok || parsedPort < 1 || parsedPort > 65535) return false;
    QString canonical;
    if (!ipv4(trimmed.left(colon), canonical)) return false;
    ip = canonical;
    port = parsedPort;
    return true;
}

QString nativeRelay(const QString &ip, int port)
{
    return ip + '/' + QString::number(port);
}

QString displayRelay(const QString &ip, int port)
{
    return ip + ':' + QString::number(port);
}

QString relayText(const State &state)
{
    if (!state.tcpFallbackRelay.isEmpty()) {
        if (!state.relayIp.isEmpty()) return displayRelay(state.relayIp, state.relayPort);
        return state.tcpFallbackRelay + QStringLiteral("（无法解析）");
    }
    return QStringLiteral("未设置");
}

bool mergeSwitches(const State &state, bool allow, bool force, QByteArray &content, QString &error)
{
    if (!state.valid()) {
        error = state.error;
        return false;
    }
    // Persist the invariant, not just the clicked field.
    if (!allow) force = false;
    auto root = state.exists ? state.root : QJsonDocument::fromJson(defaultContent()).object();
    auto settings = root.value("settings").toObject();
    settings["allowTcpFallbackRelay"] = allow;
    settings["forceTcpRelay"] = force;
    root["settings"] = settings;
    content = QJsonDocument(root).toJson(QJsonDocument::Indented);
    error.clear();
    return true;
}

bool mergeRelay(const State &state, const QString &ip, int port, QByteArray &content, QString &error)
{
    if (!state.valid()) {
        error = state.error;
        return false;
    }
    if (ip.isEmpty() || port < 1 || port > 65535) {
        error = QStringLiteral("TCP 回退中转地址无效。");
        return false;
    }
    // A missing local.conf is created with the documented initial settings and
    // then merged, rather than leaving only tcpFallbackRelay in the file.
    auto root = state.exists ? state.root : QJsonDocument::fromJson(defaultContent()).object();
    auto settings = root.value("settings").toObject();
    settings["tcpFallbackRelay"] = nativeRelay(ip, port);
    root["settings"] = settings;
    content = QJsonDocument(root).toJson(QJsonDocument::Indented);
    error.clear();
    return true;
}

bool mergeRelayDisplay(const State &state, const QString &display, QByteArray &content, QString &error)
{
    QString ip;
    int port = 0;
    if (!parseDisplayRelay(display, ip, port) || port != RelayPort) {
        error = QStringLiteral("TCP 回退中转地址必须是有效的 IPv4:9443。");
        return false;
    }
    return mergeRelay(state, ip, port, content, error);
}

QString serverIpv4(const QJsonObject &moon, const QString &networkId, QString *reason, QString *note)
{
    if (reason) reason->clear();
    if (note) note->clear();
    const auto network = networkId.toLower();
    const auto serverId = network.left(10);
    if (moon.value("id").toString().isEmpty()) {
        if (reason) *reason = QStringLiteral("Moon 元数据缺少 ID。");
        return {};
    }
    if (normalizeMoonId(moon.value("id").toString()) != moonIdForNetwork(network)) {
        if (reason) *reason = QStringLiteral("Moon ID 与网络 ID 不匹配。");
        return {};
    }
    QJsonObject serverRoot;
    for (const auto &value : moon.value("roots").toArray()) {
        const auto root = value.toObject();
        const auto identity = root.value("identity").toString().toLower();
        const auto address = identity.section(':', 0, 0);
        if (address != serverId) continue;
        serverRoot = root;
        break;
    }
    if (serverRoot.isEmpty()) {
        if (reason) *reason = QStringLiteral("Moon 未包含与网络 ID 对应的服务器根节点。");
        return {};
    }
    QStringList addresses;
    for (const auto &value : serverRoot.value("stableEndpoints").toArray()) {
        const auto endpoint = value.toString();
        const int slash = endpoint.lastIndexOf('/');
        if (slash <= 0) continue;
        QString ip;
        if (!ipv4(endpoint.left(slash), ip)) continue;
        if (!addresses.contains(ip)) addresses.append(ip);
    }
    if (addresses.isEmpty()) {
        if (reason) *reason = QStringLiteral("Moon 未公布有效的 IPv4 stableEndpoints 地址。");
        return {};
    }
    if (addresses.size() > 1 && note)
        *note = QStringLiteral("Moon 公布了 %1 个地址，按顺序使用第一个。").arg(addresses.size());
    return addresses.first();
}

QList<Candidate> candidates(const QJsonArray &networks, const QJsonDocument &moons,
                            const std::function<bool(const QString &)> &exportable,
                            const State &state)
{
    QHash<QString, QJsonObject> metadata;
    if (moons.isArray()) {
        for (const auto &value : moons.array()) {
            const auto object = value.toObject();
            const auto id = normalizeMoonId(object.value("id").toString());
            if (!id.isEmpty()) metadata.insert(id, object);
        }
    }
    QList<Candidate> result;
    QSet<QString> seen;
    for (const auto &value : networks) {
        const auto network = value.toObject();
        auto id = network.value("nwid").toString(network.value("id").toString()).trimmed().toLower();
        if (!QRegularExpression("^[0-9a-f]{16}$").match(id).hasMatch() || seen.contains(id)) continue;
        seen.insert(id);
        if (!exportable || !exportable(id)) continue;
        Candidate candidate;
        candidate.networkId = id;
        candidate.name = network.value("name").toString();
        if (candidate.name.isEmpty()) candidate.name = id;
        const auto moon = metadata.value(moonIdForNetwork(id));
        if (moon.isEmpty()) {
            candidate.reason = QStringLiteral("未找到该网络对应的 Moon 元数据，无法确定服务器公网 IPv4。");
        } else {
            QString reason;
            QString note;
            candidate.ip = serverIpv4(moon, id, &reason, &note);
            candidate.reason = reason;
            candidate.note = note;
            if (!candidate.ip.isEmpty()) {
                candidate.enabled = true;
                candidate.relay = displayRelay(candidate.ip, RelayPort);
                candidate.current = state.valid() && state.relayPort == RelayPort
                    && state.relayIp == candidate.ip;
            }
        }
        result.append(candidate);
    }
    return result;
}

QString exportRelay(const State &state, const QString &networkId,
                    const QJsonDocument &moons, QString *reason)
{
    if (reason) reason->clear();
    if (!state.valid()) {
        if (reason) *reason = state.error;
        return {};
    }
    QJsonObject moon;
    const auto expected = moonIdForNetwork(networkId.toLower());
    if (moons.isArray()) {
        for (const auto &value : moons.array()) {
            const auto object = value.toObject();
            if (normalizeMoonId(object.value("id").toString()) == expected) {
                moon = object;
                break;
            }
        }
    }
    if (moon.isEmpty()) {
        if (reason) *reason = QStringLiteral("未找到该网络对应的 Moon 元数据。");
        return {};
    }
    const auto ip = serverIpv4(moon, networkId, reason);
    if (ip.isEmpty()) return {};
    if (state.relayPort != RelayPort || state.relayIp != ip) return {};
    return displayRelay(ip, RelayPort);
}

QString ensureScript(const QString &path, const QString &snapshotPath)
{
    const auto content = QString::fromLatin1(defaultContent().toBase64());
#ifdef Q_OS_WIN
    return QStringLiteral(
        "$ErrorActionPreference = 'Stop'\n"
        "$target = %1\n"
        "$snapshot = %2\n"
        "$directory = Split-Path -Parent $target\n"
        "if (!(Test-Path -LiteralPath $directory -PathType Container)) { throw 'ZeroTier configuration directory is missing' }\n"
        "if (Test-Path -LiteralPath $target) {\n"
        "  $item = Get-Item -LiteralPath $target\n"
        "  if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'local.conf is not a regular file' }\n"
        "} else {\n"
        "  $temporary = Join-Path $directory ([IO.Path]::GetRandomFileName())\n"
        "  try {\n"
        "    [IO.File]::WriteAllBytes($temporary, [Convert]::FromBase64String(%3))\n"
        "    [IO.File]::Move($temporary, $target)\n"
        "  } finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force } }\n"
        "}\n"
        "Copy-Item -LiteralPath $target -Destination $snapshot -Force\n")
        .arg(psQuote(path), psQuote(snapshotPath), psQuote(content));
#elif defined(Q_OS_MACOS)
    return QStringLiteral(
        "set -eu\n"
        "PATH=/usr/bin:/bin:/usr/sbin:/sbin\nexport PATH\n"
        "target=%1\n"
        "snapshot=%2\n"
        "directory=$(/usr/bin/dirname \"$target\")\n"
        "[ -d \"$directory\" ] || { echo 'ZeroTier configuration directory is missing' >&2; exit 1; }\n"
        "if [ -e \"$target\" ]; then\n"
        "  [ ! -L \"$target\" ] || { echo 'local.conf is not a regular file' >&2; exit 1; }\n"
        "else\n"
        "  temporary=\"$target.zto-$$\"\n"
        "  trap '/bin/rm -f -- \"$temporary\"' EXIT HUP INT TERM\n"
        "  printf '%s' %3 > \"$temporary\"\n"
        "  /bin/chmod 600 \"$temporary\"\n"
        "  /bin/mv -f \"$temporary\" \"$target\"\n"
        "  trap - EXIT HUP INT TERM\n"
        "fi\n"
        "/bin/cp -f \"$target\" \"$snapshot\"\n"
        "/bin/chmod 644 \"$snapshot\"\n")
        .arg(shQuote(path), shQuote(snapshotPath), shQuote(QString::fromUtf8(defaultContent())));
#else
    Q_UNUSED(path)
    Q_UNUSED(snapshotPath)
    return {};
#endif
}

QString installCommands(const Install &install)
{
    const auto expected = install.expectedSha256.toLower();
#ifdef Q_OS_WIN
    return QStringLiteral(
        "$localTarget = %1\n"
        "$localProposed = %2\n"
        "$localExpected = %3\n"
        "$localDirectory = Split-Path -Parent $localTarget\n"
        "if (!(Test-Path -LiteralPath $localDirectory -PathType Container)) { throw 'ZeroTier configuration directory is missing' }\n"
        "if (Test-Path -LiteralPath $localTarget) {\n"
        "  $localItem = Get-Item -LiteralPath $localTarget\n"
        "  if ($localItem.PSIsContainer -or ($localItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'local.conf is not a regular file' }\n"
        "  $localActual = (Get-FileHash -LiteralPath $localTarget -Algorithm SHA256).Hash.ToLowerInvariant()\n"
        "  if ($localActual -ne $localExpected) { throw 'local.conf changed since it was validated' }\n"
        "} else {\n"
        "  if ($localExpected -ne '') { throw 'local.conf was removed since it was validated' }\n"
        "}\n"
        "$localTemporary = Join-Path $localDirectory ([IO.Path]::GetRandomFileName())\n"
        "$localBackup = Join-Path $localDirectory ([IO.Path]::GetRandomFileName())\n"
        "try {\n"
        "  [IO.File]::WriteAllBytes($localTemporary, [IO.File]::ReadAllBytes($localProposed))\n"
        "  if (Test-Path -LiteralPath $localTarget) { [IO.File]::Replace($localTemporary, $localTarget, $localBackup) }\n"
        "  else { [IO.File]::Move($localTemporary, $localTarget) }\n"
        "} finally {\n"
        "  if (Test-Path -LiteralPath $localTemporary) { Remove-Item -LiteralPath $localTemporary -Force }\n"
        "  if (Test-Path -LiteralPath $localBackup) { Remove-Item -LiteralPath $localBackup -Force }\n"
        "}\n")
        .arg(psQuote(install.path), psQuote(install.proposedPath), psQuote(expected));
#elif defined(Q_OS_MACOS)
    return QStringLiteral(
        "set -eu\n"
        "PATH=/usr/bin:/bin:/usr/sbin:/sbin\nexport PATH\n"
        "target=%1\n"
        "proposed=%2\n"
        "expected=%3\n"
        "mode=600\n"
        "[ ! -L \"$target\" ] || { echo 'local.conf is not a regular file' >&2; exit 1; }\n"
        "if [ -e \"$target\" ]; then\n"
        "  actual=$(/usr/bin/shasum -a 256 \"$target\" | /usr/bin/awk '{print $1}')\n"
        "  [ \"$actual\" = \"$expected\" ] || { echo 'local.conf changed since it was validated' >&2; exit 1; }\n"
        "  mode=$(/usr/bin/stat -f '%Lp' \"$target\")\n"
        "else\n"
        "  [ -z \"$expected\" ] || { echo 'local.conf was removed since it was validated' >&2; exit 1; }\n"
        "fi\n"
        "temporary=\"$target.zto-$$\"\n"
        "trap '/bin/rm -f -- \"$temporary\"' EXIT HUP INT TERM\n"
        "/bin/cp -f \"$proposed\" \"$temporary\"\n"
        "/bin/chmod \"$mode\" \"$temporary\"\n"
        "/bin/mv -f \"$temporary\" \"$target\"\n"
        "trap - EXIT HUP INT TERM\n")
        .arg(shQuote(install.path), shQuote(install.proposedPath), shQuote(expected));
#else
    Q_UNUSED(install)
    return {};
#endif
}

QString installScript(const Install &install)
{
    const auto commands = installCommands(install);
    if (commands.isEmpty()) return {};
#ifdef Q_OS_MACOS
    return "set -eu\nPATH=/usr/bin:/bin:/usr/sbin:/sbin\nexport PATH\n" + commands;
#else
    return "$ErrorActionPreference = 'Stop'\ntry {\n" + commands + "exit 0\n} catch { Write-Output $_; exit 1 }";
#endif
}

} // namespace vusiif::zto::zerotier::localconfig

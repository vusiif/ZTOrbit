#include "MacPlatform.h"
#include <QDir>
#include <QFileInfo>

QString vusiif::zto::platform::quote(const QString &value)
{
    QString escaped = value;
    escaped.replace("'", "'\\''");
    return "'" + escaped + "'";
}

QString vusiif::zto::platform::appleScriptString(const QString &value)
{
    QString escaped = value;
    escaped.replace('\\', "\\\\");
    escaped.replace('"', "\\\"");
    escaped.replace('\n', "\\n");
    escaped.replace('\r', "\\r");
    return '"' + escaped + '"';
}

QString vusiif::zto::platform::serviceCommands(const QString &action, const QString &cli)
{
    const QString stop = "launchctl bootout system/com.zerotier.one >/dev/null 2>&1 || true\n";
    const QString probe = quote(cli) + (QFileInfo(cli).fileName().startsWith("zerotier-one") ? " -q" : "") + " -j info";
    const QString start = "launchctl enable system/com.zerotier.one\n"
        "launchctl bootstrap system '/Library/LaunchDaemons/com.zerotier.one.plist' >/dev/null 2>&1 || "
        "launchctl print system/com.zerotier.one >/dev/null 2>&1\n"
        "launchctl kickstart system/com.zerotier.one\n"
        "attempt=0\nwhile ! " + probe + " >/dev/null 2>&1; do\n"
        "  attempt=$((attempt + 1))\n  [ \"$attempt\" -lt 30 ] || { echo 'ZeroTier API did not start'; exit 1; }\n"
        "  sleep 1\ndone\n";
    if (action == "stop") return stop;
    if (action == "start") return start;
    if (action == "restart") return stop + start;
    return {};
}

QString vusiif::zto::platform::packageScript(bool uninstall, const QString &dataDirectory)
{
    if (!uninstall) return QStringLiteral(R"SH(set -eu
PATH=/usr/bin:/bin:/usr/sbin:/sbin
export PATH
temporary=$(mktemp -d /private/tmp/zto-install.XXXXXX)
trap '/bin/rm -rf -- "$temporary"' EXIT HUP INT TERM
/usr/bin/curl --fail --location --proto '=https' --tlsv1.2 'https://download.zerotier.com/dist/ZeroTier%20One.pkg' --output "$temporary/ZeroTier.pkg"
/usr/sbin/pkgutil --check-signature "$temporary/ZeroTier.pkg"
/usr/sbin/installer -pkg "$temporary/ZeroTier.pkg" -target /
)SH");
    const QString home = QDir(dataDirectory).filePath("One");
    return "set -eu\nPATH=/usr/bin:/bin:/usr/sbin:/sbin\nexport PATH\n"
        "uninstaller=" + quote(QDir(home).filePath("uninstall.sh")) + "\n"
        "[ -f \"$uninstaller\" ] && [ ! -L \"$uninstaller\" ] || { echo 'Official ZeroTier uninstaller missing'; exit 1; }\n"
        "/bin/bash \"$uninstaller\"\n"
        "target=" + quote(dataDirectory) + "\n"
        "[ ! -L \"$target\" ] || { echo 'Refusing symbolic-link data directory'; exit 1; }\n"
        "if [ -d \"$target\" ]; then\n"
        "  [ \"$(cd \"$target\" && pwd -P)\" = \"$target\" ] || exit 1\n"
        "  /bin/rm -rf -- \"$target\"\nfi\n";
}

QString vusiif::zto::platform::copyMoons(const QList<QPair<QString, QString>> &files, const QString &directory)
{
    QString commands = "/bin/mkdir -p " + quote(directory) + "\n";
    for (const auto &file : files) {
        if (QFileInfo(file.second).absolutePath() != QDir(directory).absolutePath()
            || QFileInfo(file.second).suffix().compare("moon", Qt::CaseInsensitive) != 0) return "exit 1\n";
        commands += "/bin/cp -f " + quote(file.first) + " " + quote(file.second) + "\n";
    }
    return commands;
}

QString vusiif::zto::platform::deleteMoon(const QString &path, const QString &directory)
{
    if (QFileInfo(path).absolutePath() != QDir(directory).absolutePath()
        || QFileInfo(path).suffix().compare("moon", Qt::CaseInsensitive) != 0) return "exit 1\n";
    return "directory=" + quote(directory) + "\n"
        "[ ! -L \"$directory\" ] || exit 1\n"
        "[ \"$(cd \"$directory\" && pwd -P)\" = \"$directory\" ] || exit 1\n"
        "/bin/rm -f -- " + quote(path) + "\n";
}

QString vusiif::zto::platform::synchronizeSession(const QString &systemHome, const QString &sessionHome, unsigned int userId)
{
    // The session contains only local API credentials and public signed Moon files.
    // Never copy identity.secret or print authentication contents to the log.
    return "source=" + quote(systemHome) + "\nsession=" + quote(sessionHome) + "\n"
        "[ -d \"$session\" ] && [ ! -L \"$session\" ] || exit 1\n"
        "[ \"$(/usr/bin/stat -f '%u' \"$session\")\" = '" + QString::number(userId) + "' ] || exit 1\n"
        "[ -z \"$(/usr/bin/find \"$session\" -type l -print)\" ] || exit 1\n"
        "[ ! -L \"$source/authtoken.secret\" ] && [ ! -L \"$source/zerotier-one.port\" ] || exit 1\n"
        "if [ -f \"$source/authtoken.secret\" ]; then\n"
        "  /bin/cp -f \"$source/authtoken.secret\" \"$session/authtoken.secret\"\n"
        "  if [ -f \"$source/zerotier-one.port\" ]; then\n"
        "    /bin/cp -f \"$source/zerotier-one.port\" \"$session/zerotier-one.port\"\n"
        "  else printf '9993' > \"$session/zerotier-one.port\"; fi\n"
        "  /bin/mkdir -p \"$session/moons.d\"\n"
        "  /usr/bin/find \"$session/moons.d\" -type f -name '*.moon' -delete\n"
        "  for moon in \"$source/moons.d/\"*.moon; do\n"
        "    [ -f \"$moon\" ] && [ ! -L \"$moon\" ] || continue\n"
        "    /bin/cp -f \"$moon\" \"$session/moons.d/\"\n"
        "  done\n"
        "  /usr/sbin/chown -R '" + QString::number(userId) + "' \"$session\"\n"
        "  /bin/chmod 700 \"$session\" \"$session/moons.d\"\n"
        "  /usr/bin/find \"$session\" -type f -exec /bin/chmod 600 {} +\n"
        "else\n"
        "  /bin/rm -f \"$session/authtoken.secret\" \"$session/zerotier-one.port\"\n"
        "  /usr/bin/find \"$session/moons.d\" -type f -name '*.moon' -delete 2>/dev/null || true\n"
        "fi\n";
}

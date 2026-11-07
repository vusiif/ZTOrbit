#include "ResetScript.h"

#include "LocalConfig.h"
#include "MacPlatform.h"
#include "ResetPlan.h"
#include "WindowsPortScript.h"

#include <QStringList>

namespace vusiif::zto::reset {
namespace {

QString psLiteral(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

QString psArray(const QStringList &values)
{
    QStringList quoted;
    quoted.reserve(values.size());
    for (const auto &value : values) quoted.append(psLiteral(value));
    return QStringLiteral("@(") + quoted.join(QStringLiteral(", ")) + QLatin1Char(')');
}

QString shLiteral(const QString &value)
{
    return vusiif::zto::platform::quote(value);
}

} // namespace

QString windowsResetScript(const ScriptOptions &options)
{
    const auto whitelist = psArray(deletionWhitelist());
    QStringList script;
    script << QStringLiteral("$ErrorActionPreference = 'Stop'");
    script << QStringLiteral("$dataRoot = [IO.Path]::GetFullPath(%1).TrimEnd([IO.Path]::DirectorySeparatorChar)")
                  .arg(psLiteral(options.dataRoot));
    script << QStringLiteral("$stage = 'begin'");
    script << QStringLiteral("try {");
    // Root validation happens before any service operation, so a bad data
    // directory is reported without stopping ZeroTier.
    script << QStringLiteral("  $rootItem = Get-Item -LiteralPath $dataRoot -Force -ErrorAction Stop");
    script << QStringLiteral("  if (!$rootItem.PSIsContainer) { throw 'ZeroTier data directory is not a directory' }");
    script << QStringLiteral("  if ($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'ZeroTier data directory is a reparse point' }");
    script << QStringLiteral("  $rootActual = [IO.Path]::GetFullPath($rootItem.FullName)");
    script << QStringLiteral("  if (![string]::Equals($rootActual, $dataRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'ZeroTier data directory changed after validation' }");
    script << QStringLiteral("  foreach ($stateDirectory in @('networks.d','peers.d','moons.d')) {");
    script << QStringLiteral("    $statePath = [IO.Path]::Combine($dataRoot, $stateDirectory)");
    script << QStringLiteral("    if (Test-Path -LiteralPath $statePath) {");
    script << QStringLiteral("      $stateItem = Get-Item -LiteralPath $statePath -Force -ErrorAction Stop");
    script << QStringLiteral("      if (!$stateItem.PSIsContainer -or ($stateItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw ('Unsafe ZeroTier state directory: ' + $stateDirectory) }");
    script << QStringLiteral("    }");
    script << QStringLiteral("  }");
    script << QStringLiteral("  $removalListPath = %1").arg(psLiteral(options.removalListPath));
    script << QStringLiteral("  $removalListItem = Get-Item -LiteralPath $removalListPath -Force -ErrorAction Stop");
    script << QStringLiteral("  if ($removalListItem.PSIsContainer -or ($removalListItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Reset removal list is not a regular file' }");
    script << QStringLiteral("  Write-Output 'ZTO-RESET stage=elevated status=ok'");
    if (options.manageService) {
        script << QStringLiteral("  $stage = 'stop'");
        script << QStringLiteral("  $service = Get-Service -Name 'ZeroTierOneService' -ErrorAction SilentlyContinue");
        script << QStringLiteral("  if (!$service) { throw 'not-installed' }");
        script << QStringLiteral("  if ($service.Status -ne 'Stopped') {");
        script << QStringLiteral("    Stop-Service -Name 'ZeroTierOneService' -ErrorAction Stop");
        script << QStringLiteral("    $service = Get-Service -Name 'ZeroTierOneService' -ErrorAction Stop");
        script << QStringLiteral("    $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))");
        script << QStringLiteral("  }");
        script << QStringLiteral("  $service = Get-Service -Name 'ZeroTierOneService' -ErrorAction Stop");
        script << QStringLiteral("  if ($service.Status -ne 'Stopped') { throw 'ZeroTier service did not stop' }");
        script << QStringLiteral("  Write-Output 'ZTO-RESET stage=stop status=ok'");
    }
    // Everything below this point runs only after the stop step succeeded:
    // a Stop-Service/verification failure throws and is handled by the catch
    // block before the delete loop is reached.
    script << QStringLiteral("  $stage = 'cleanup'");
    script << QStringLiteral("  $allowedPatterns = %1").arg(whitelist);
    script << QStringLiteral("  $rootPrefix = $dataRoot + [IO.Path]::DirectorySeparatorChar");
    script << QStringLiteral("  foreach ($rawRelative in (Get-Content -LiteralPath $removalListPath)) {");
    script << QStringLiteral("    $relative = $rawRelative.Trim()");
    script << QStringLiteral("    if ([string]::IsNullOrEmpty($relative)) { continue }");
    script << QStringLiteral("    if ([IO.Path]::IsPathRooted($relative)) { throw 'Unsafe reset path' }");
    script << QStringLiteral("    $normalized = $relative.Replace('\\', '/')");
    script << QStringLiteral("    $allowed = $false");
    script << QStringLiteral("    foreach ($pattern in $allowedPatterns) { if ($normalized -cmatch $pattern) { $allowed = $true; break } }");
    script << QStringLiteral("    if (!$allowed) { throw ('Path is not whitelisted: ' + $relative) }");
    script << QStringLiteral("    $target = [IO.Path]::GetFullPath([IO.Path]::Combine($dataRoot, $relative))");
    script << QStringLiteral("    if (![string]::Equals($target, $dataRoot, [StringComparison]::OrdinalIgnoreCase) -and !$target.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw ('Path escapes the data directory: ' + $relative) }");
    script << QStringLiteral("    $targetItem = Get-Item -LiteralPath $target -Force -ErrorAction SilentlyContinue");
    script << QStringLiteral("    if (!$targetItem) { continue }");
    script << QStringLiteral("    if ($targetItem.PSIsContainer -or ($targetItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw ('Refusing non-regular file: ' + $relative) }");
    script << QStringLiteral("    $parent = [IO.Path]::GetDirectoryName($target)");
    script << QStringLiteral("    while ($parent) {");
    script << QStringLiteral("      $parentItem = Get-Item -LiteralPath $parent -Force -ErrorAction SilentlyContinue");
    script << QStringLiteral("      if (!$parentItem) { throw ('Missing parent directory: ' + $relative) }");
    script << QStringLiteral("      if (!$parentItem.PSIsContainer -or ($parentItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw ('Unsafe parent directory: ' + $relative) }");
    script << QStringLiteral("      if ([string]::Equals($parent, $dataRoot, [StringComparison]::OrdinalIgnoreCase)) { break }");
    script << QStringLiteral("      $parent = [IO.Path]::GetDirectoryName($parent)");
    script << QStringLiteral("    }");
    script << QStringLiteral("    Remove-Item -LiteralPath $target -Force -ErrorAction Stop");
    script << QStringLiteral("  }");
    script << QStringLiteral("  Write-Output 'ZTO-RESET stage=cleanup status=ok'");
    script << QStringLiteral("  $stage = 'localconf'");
    if (options.localConfigChanged) {
        // Same atomic installer used by the settings page: it re-checks the
        // current file hash, refuses reparse points and replaces in place.
        const vusiif::zto::zerotier::localconfig::Install install{
            options.localConfigPath, options.proposedLocalConfigPath, options.expectedLocalConfigSha256};
        const auto commands = vusiif::zto::zerotier::localconfig::installCommands(install);
        if (!commands.isEmpty()) script << commands;
    }
    script << QStringLiteral("  Write-Output 'ZTO-RESET stage=localconf status=ok'");
    if (options.manageService) {
        script << QStringLiteral("  $stage = 'start'");
        script << QStringLiteral("  Start-Service -Name 'ZeroTierOneService' -ErrorAction Stop");
        script << QStringLiteral("  $service = Get-Service -Name 'ZeroTierOneService' -ErrorAction Stop");
        script << QStringLiteral("  $service.WaitForStatus('Running', [TimeSpan]::FromSeconds(30))");
        script << QStringLiteral("  $ztDirectory = $dataRoot");
        // Existing local API readiness script: it loads zerotier-one.port and
        // authtoken.secret and only succeeds once /status answers.
        script << vusiif::zto::zerotier::windowsServiceReady();
        script << QStringLiteral("  Write-Output 'ZTO-RESET stage=start status=ok'");
    }
    script << QStringLiteral("  Write-Output 'ZTO-RESET stage=done status=ok'");
    script << QStringLiteral("  exit 0");
    script << QStringLiteral("} catch {");
    script << QStringLiteral("  $detail = [string]$_.Exception.Message");
    script << QStringLiteral("  $detail = $detail -replace '\\r?\\n', ' '");
    script << QStringLiteral("  Write-Output ('ZTO-RESET stage=' + $stage + ' status=failed message=' + $detail)");
    if (options.manageService) {
        // Keep the service running whenever possible, including after a
        // partial cleanup or a failed start/readiness verification.
        script << QStringLiteral("  $stage = 'recovery'");
        script << QStringLiteral("  try {");
        script << QStringLiteral("    $service = Get-Service -Name 'ZeroTierOneService' -ErrorAction SilentlyContinue");
        script << QStringLiteral("    if ($service) {");
        script << QStringLiteral("      if ($service.Status -ne 'Running') {");
        script << QStringLiteral("        Start-Service -Name 'ZeroTierOneService' -ErrorAction Stop");
        script << QStringLiteral("        $service = Get-Service -Name 'ZeroTierOneService' -ErrorAction Stop");
        script << QStringLiteral("        $service.WaitForStatus('Running', [TimeSpan]::FromSeconds(30))");
        script << QStringLiteral("      }");
        script << QStringLiteral("      Write-Output 'ZTO-RESET stage=recovery status=ok'");
        script << QStringLiteral("    }");
        script << QStringLiteral("  } catch {");
        script << QStringLiteral("    $recoveryDetail = [string]$_.Exception.Message");
        script << QStringLiteral("    $recoveryDetail = $recoveryDetail -replace '\\r?\\n', ' '");
        script << QStringLiteral("    Write-Output ('ZTO-RESET stage=recovery status=failed message=' + $recoveryDetail)");
        script << QStringLiteral("  }");
    }
    script << QStringLiteral("  exit 1");
    script << QStringLiteral("}");
    return script.join(QLatin1Char('\n'));
}

QString macResetScript(const ScriptOptions &options)
{
    QStringList script;
    script << QStringLiteral("set -eu");
    script << QStringLiteral("PATH=/usr/bin:/bin:/usr/sbin:/sbin");
    script << QStringLiteral("export PATH");
    script << QStringLiteral("dataRoot=%1").arg(shLiteral(options.dataRoot));
    script << QStringLiteral("removalListPath=%1").arg(shLiteral(options.removalListPath));
    script << QStringLiteral("stage=begin");
    script << QStringLiteral("marker() { printf 'ZTO-RESET stage=%s status=%s%s\\n' \"$1\" \"$2\" \"${3:-}\"; }");
    script << QStringLiteral("recover() {");
    script << QStringLiteral("  [ -f '/Library/LaunchDaemons/com.zerotier.one.plist' ] || return 0");
    script << QStringLiteral("  if ( ");
    for (const auto &line : vusiif::zto::platform::serviceCommands(QStringLiteral("start"), options.cliPath).split(QLatin1Char('\n'), Qt::SkipEmptyParts))
        script << QStringLiteral("    ") + line;
    script << QStringLiteral("  ) >/dev/null 2>&1; then marker recovery ok; else marker recovery failed; fi");
    script << QStringLiteral("}");
    script << QStringLiteral("fail() {");
    script << QStringLiteral("  detail=$(printf '%s' \"$1\" | /usr/bin/tr '\\r\\n' '  ')");
    script << QStringLiteral("  marker \"$stage\" failed \" message=$detail\"");
    script << QStringLiteral("  stage=recovery");
    script << QStringLiteral("  recover");
    script << QStringLiteral("  exit 1");
    script << QStringLiteral("}");
    script << QStringLiteral("[ -d \"$dataRoot\" ] || fail 'ZeroTier data directory is missing'");
    script << QStringLiteral("[ ! -L \"$dataRoot\" ] || fail 'ZeroTier data directory is a symbolic link'");
    script << QStringLiteral("[ \"$(cd \"$dataRoot\" && /bin/pwd -P)\" = \"$dataRoot\" ] || fail 'ZeroTier data directory path changed after validation'");
    script << QStringLiteral("for stateDirectory in networks.d peers.d moons.d; do");
    script << QStringLiteral("  statePath=\"$dataRoot/$stateDirectory\"");
    script << QStringLiteral("  if [ -e \"$statePath\" ]; then");
    script << QStringLiteral("    [ -d \"$statePath\" ] && [ ! -L \"$statePath\" ] || fail \"Unsafe state directory: $stateDirectory\"");
    script << QStringLiteral("  fi");
    script << QStringLiteral("done");
    script << QStringLiteral("[ -f \"$removalListPath\" ] && [ ! -L \"$removalListPath\" ] || fail 'Reset removal list is missing'");
    script << QStringLiteral("marker elevated ok");
    if (options.manageService) {
        script << QStringLiteral("stage=stop");
        script << QStringLiteral("[ -f '/Library/LaunchDaemons/com.zerotier.one.plist' ] || fail 'not-installed'");
        for (const auto &line : vusiif::zto::platform::serviceCommands(QStringLiteral("stop"), options.cliPath).split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            script << line;
        script << QStringLiteral("attempt=0");
        script << QStringLiteral("while /bin/launchctl print system/com.zerotier.one >/dev/null 2>&1; do");
        script << QStringLiteral("  attempt=$((attempt + 1))");
        script << QStringLiteral("  [ \"$attempt\" -lt 30 ] || fail 'ZeroTier service did not stop'");
        script << QStringLiteral("  sleep 1");
        script << QStringLiteral("done");
        script << QStringLiteral("marker stop ok");
    }
    script << QStringLiteral("stage=cleanup");
    script << QStringLiteral("while IFS= read -r relative; do");
    script << QStringLiteral("  [ -n \"$relative\" ] || continue");
    script << QStringLiteral("  case \"$relative\" in /*|*\\\\*|*../*|..|*/../*) fail 'Unsafe reset path';; esac");
    script << QStringLiteral("  allowed=0");
    for (const auto &pattern : deletionWhitelist()) {
        script << QStringLiteral("  if printf '%s\\n' \"$relative\" | /usr/bin/grep -Eq %1; then allowed=1; fi")
                      .arg(shLiteral(pattern));
    }
    script << QStringLiteral("  [ \"$allowed\" = 1 ] || fail \"Path is not whitelisted: $relative\"");
    script << QStringLiteral("  target=\"$dataRoot/$relative\"");
    script << QStringLiteral("  case \"$target\" in \"$dataRoot\"/*) ;; *) fail 'Path escapes the data directory';; esac");
    script << QStringLiteral("  parent=$(/usr/bin/dirname \"$target\")");
    script << QStringLiteral("  while [ \"$parent\" != \"$dataRoot\" ]; do");
    script << QStringLiteral("    [ -d \"$parent\" ] && [ ! -L \"$parent\" ] || fail \"Unsafe parent directory: $relative\"");
    script << QStringLiteral("    parent=$(/usr/bin/dirname \"$parent\")");
    script << QStringLiteral("  done");
    script << QStringLiteral("  [ ! -L \"$target\" ] || fail \"Refusing link: $relative\"");
    script << QStringLiteral("  if [ -f \"$target\" ]; then /bin/rm -f -- \"$target\" || fail \"Cannot delete: $relative\"; fi");
    script << QStringLiteral("done < \"$removalListPath\"");
    script << QStringLiteral("marker cleanup ok");
    script << QStringLiteral("stage=localconf");
    if (options.localConfigChanged) {
#ifdef Q_OS_MACOS
        const vusiif::zto::zerotier::localconfig::Install install{
            options.localConfigPath, options.proposedLocalConfigPath, options.expectedLocalConfigSha256};
        const auto commands = vusiif::zto::zerotier::localconfig::installCommands(install);
        if (!commands.isEmpty()) {
            // The installer uses `exit 1` on validation failure; run it in a
            // subshell so the script can emit the stage marker and recover.
            script << QStringLiteral("if ! (");
            for (const auto &line : commands.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
                script << QStringLiteral("  ") + line;
            script << QStringLiteral("); then fail 'local.conf update failed'; fi");
        }
#endif
    }
    script << QStringLiteral("marker localconf ok");
    if (options.manageService) {
        script << QStringLiteral("stage=start");
        script << QStringLiteral("if ! ( ");
        for (const auto &line : vusiif::zto::platform::serviceCommands(QStringLiteral("start"), options.cliPath).split(QLatin1Char('\n'), Qt::SkipEmptyParts))
            script << QStringLiteral("  ") + line;
        script << QStringLiteral("); then fail 'ZeroTier service or local API did not start'; fi");
        script << QStringLiteral("marker start ok");
    }
    script << QStringLiteral("marker done ok");
    script << QStringLiteral("exit 0");
    return script.join(QLatin1Char('\n'));
}

QString resetScript(const ScriptOptions &options)
{
#if defined(Q_OS_WIN)
    return windowsResetScript(options);
#elif defined(Q_OS_MACOS)
    return macResetScript(options);
#else
    Q_UNUSED(options)
    return {};
#endif
}

} // namespace vusiif::zto::reset

$ErrorActionPreference = 'Stop'
$scriptSource = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../client/main/WindowsPortScript.h') -Raw
$preflight = [regex]::Match($scriptSource, 'R"PS\((.*?)\)PS"', [Text.RegularExpressions.RegexOptions]::Singleline).Groups[1].Value
if (!$preflight) { throw 'Port preflight script missing' }
$originalProgramData = $env:ProgramData
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('ztgui-port-test-' + [guid]::NewGuid())
$env:ProgramData = $testRoot
New-Item -ItemType Directory -Path (Join-Path $testRoot 'ZeroTier/One') -Force | Out-Null
$configPath = Join-Path $testRoot 'ZeroTier/One/local.conf'
$script:reserved = $true
$script:stops = 0
function netsh {
    if ($script:reserved) { '       9900        10000'; '      20000        20002 *' }
    $global:LASTEXITCODE = 0
}
function Get-Service {
    [CmdletBinding()] param([string]$Name)
    $value = [pscustomobject]@{Status='Running'}
    $value | Add-Member -MemberType ScriptMethod -Name WaitForStatus -Value { param($status,$timeout) }
    return $value
}
function Stop-Service { [CmdletBinding()] param([string]$Name) $script:stops++ }
try {
    [IO.File]::WriteAllText($configPath, '{"settings":{"primaryPort":9993,"allowTcpFallbackRelay":false},"extra":{"keep":42}}')
    & ([scriptblock]::Create($preflight))
    $config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
    if ($config.settings.primaryPort -lt 20003 -or $config.settings.primaryPort -gt 29999) { throw 'Selected a reserved port' }
    if ($config.settings.allowTcpFallbackRelay -ne $false -or $config.extra.keep -ne 42) { throw 'Lost existing configuration' }
    if ($script:stops -ne 1) { throw 'Did not stop conflicting ZeroTier service' }
    if (!(Test-Path -LiteralPath ($configPath + '.ztgui-backup'))) { throw 'Backup missing' }
    $saved = Get-Content -LiteralPath $configPath -Raw
    $script:reserved = $false
    & ([scriptblock]::Create($preflight))
    if ((Get-Content -LiteralPath $configPath -Raw) -ne $saved -or $script:stops -ne 1) { throw 'Changed an unreserved configuration' }
    [IO.File]::WriteAllText($configPath, 'invalid json')
    $failed = $false
    try { & ([scriptblock]::Create($preflight)) } catch { $failed=$true }
    if (!$failed -or (Get-Content -LiteralPath $configPath -Raw) -ne 'invalid json') { throw 'Invalid config was overwritten' }
    Write-Output 'Windows port preflight tests passed (mocked service/reservations)'
} finally {
    $env:ProgramData = $originalProgramData
    $resolved = [IO.Path]::GetFullPath($testRoot)
    if (![IO.Path]::GetFileName($resolved).StartsWith('ztgui-port-test-') -or
        [IO.Path]::GetDirectoryName($resolved) -ne [IO.Path]::GetTempPath().TrimEnd('\','/')) { throw 'Unsafe test cleanup' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

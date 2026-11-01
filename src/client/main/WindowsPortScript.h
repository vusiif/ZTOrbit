#pragma once
#include <QString>

namespace vusiif::zto::zerotier {
// Runs only inside the already elevated service operation. Do not stop WinNAT
// or delete system reservations: select a ZeroTier port outside those ranges.
inline QString windowsPortPreflight() {
    return QString::fromUtf8(R"PS(
$ztDirectory = Join-Path $env:ProgramData 'ZeroTier/One'
$ztConfigPath = Join-Path $ztDirectory 'local.conf'
$ztConfig = [pscustomobject]@{}
if (Test-Path -LiteralPath $ztConfigPath) {
    if ((Get-Item -LiteralPath $ztConfigPath).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'ZeroTier configuration is a link' }
    $ztConfig = Get-Content -LiteralPath $ztConfigPath -Raw | ConvertFrom-Json
    if ($null -eq $ztConfig -or $ztConfig -isnot [pscustomobject]) { throw 'Invalid ZeroTier local.conf' }
}
$ztPort = 9993
if ($ztConfig.settings -and $null -ne $ztConfig.settings.primaryPort -and [int]$ztConfig.settings.primaryPort -gt 0) {
    $ztPort = [int]$ztConfig.settings.primaryPort
}
$ztRanges = @()
foreach ($ztFamily in @('ipv4', 'ipv6')) {
    foreach ($ztProtocol in @('tcp', 'udp')) {
        $ztOutput = & netsh interface $ztFamily show excludedportrange "protocol=$ztProtocol"
        if ($LASTEXITCODE -ne 0) { throw "Cannot inspect $ztFamily $ztProtocol excluded ports" }
        foreach ($ztLine in $ztOutput) {
            if ($ztLine -match '^\s*(\d+)\s+(\d+)\s*\*?\s*$') {
                $ztRanges += ,@([int]$Matches[1], [int]$Matches[2])
            }
        }
    }
}
function Test-ZtReserved([int]$ztCandidate) {
    foreach ($ztRange in $ztRanges) { if ($ztCandidate -ge $ztRange[0] -and $ztCandidate -le $ztRange[1]) { return $true } }
    return $false
}
function Test-ZtBindable([int]$ztCandidate) {
    $ztTcp = $null; $ztUdp = $null
    try {
        $ztTcp = [Net.Sockets.Socket]::new([Net.Sockets.AddressFamily]::InterNetwork, [Net.Sockets.SocketType]::Stream, [Net.Sockets.ProtocolType]::Tcp)
        $ztUdp = [Net.Sockets.Socket]::new([Net.Sockets.AddressFamily]::InterNetwork, [Net.Sockets.SocketType]::Dgram, [Net.Sockets.ProtocolType]::Udp)
        $ztTcp.ExclusiveAddressUse = $true; $ztUdp.ExclusiveAddressUse = $true
        $ztEndpoint = [Net.IPEndPoint]::new([Net.IPAddress]::Any, $ztCandidate)
        $ztTcp.Bind($ztEndpoint); $ztUdp.Bind($ztEndpoint)
        return $true
    } catch { return $false } finally {
        if ($ztTcp) { $ztTcp.Dispose() }; if ($ztUdp) { $ztUdp.Dispose() }
    }
}
if (Test-ZtReserved $ztPort) {
    Write-Output "ZeroTier port $ztPort overlaps Windows excluded ports; selecting another port"
    $ztService = Get-Service -Name ZeroTierOneService -ErrorAction Stop
    if ($ztService.Status -ne 'Stopped') {
        Stop-Service -Name ZeroTierOneService -ErrorAction Stop
        $ztService.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
    }
    $ztNewPort = 0
    foreach ($ztCandidate in 20000..29999) {
        if (!(Test-ZtReserved $ztCandidate) -and (Test-ZtBindable $ztCandidate)) { $ztNewPort = $ztCandidate; break }
    }
    if (!$ztNewPort) { throw 'No available ZeroTier TCP/UDP port' }
    if (!$ztConfig.settings) { $ztConfig | Add-Member -NotePropertyName settings -NotePropertyValue ([pscustomobject]@{}) -Force }
    if ($ztConfig.settings -isnot [pscustomobject]) { throw 'Invalid ZeroTier settings' }
    $ztConfig.settings | Add-Member -NotePropertyName primaryPort -NotePropertyValue $ztNewPort -Force
    $ztJson = $ztConfig | ConvertTo-Json -Depth 100
    $ztTemporary = Join-Path $ztDirectory ([IO.Path]::GetRandomFileName())
    try {
        [IO.File]::WriteAllText($ztTemporary, $ztJson, [Text.UTF8Encoding]::new($false))
        if (Test-Path -LiteralPath $ztConfigPath) {
            [IO.File]::Replace($ztTemporary, $ztConfigPath, ($ztConfigPath + '.zto-backup'))
        } else { [IO.File]::Move($ztTemporary, $ztConfigPath) }
    } finally { if (Test-Path -LiteralPath $ztTemporary) { Remove-Item -LiteralPath $ztTemporary -Force } }
    Write-Output "ZeroTier primaryPort: $ztPort -> $ztNewPort (other settings preserved)"
}
)PS");
}
inline QString windowsServiceReady() {
    return QString::fromUtf8(R"PS(
$ztReady = $false
for ($ztAttempt = 0; $ztAttempt -lt 10; ++$ztAttempt) {
    try {
        $ztActualPort = [int](Get-Content -LiteralPath (Join-Path $ztDirectory 'zerotier-one.port') -Raw).Trim()
        $ztToken = (Get-Content -LiteralPath (Join-Path $ztDirectory 'authtoken.secret') -Raw).Trim()
        $ztStatus = Invoke-RestMethod -Uri "http://127.0.0.1:$ztActualPort/status" -Headers @{'X-ZT1-Auth'=$ztToken} -TimeoutSec 2
        if ($ztStatus.address) { $ztReady = $true; break }
    } catch {}
    Start-Sleep -Milliseconds 500
}
if (!$ztReady) { throw 'ZeroTier service is running but its local API is unavailable; inspect port reservations and ZeroTier logs' }
)PS");
}
}

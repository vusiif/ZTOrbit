# Stage-5 real-chain driver: real Java host + desktop-share JAR + real client plugin
# processes. It reproduces a bounded server->client UDP interruption (egress stall)
# through a transparent localhost relay, and a single slow viewer, then checks the
# recorded frame timeline and per-viewer counters.
param(
    [Parameter(Mandatory = $true)] [string]$QtRoot,
    [Parameter(Mandatory = $true)] [string]$OutDir,
    [string]$Exe = '',
    [string]$HostJar = 'build/dist/Z2Hub-linux-x64/Z2Hub.jar',
    [string]$PluginJar = 'build/dist/server-plugins/z2hub_desktop_share-v0.1.0.jar',
    [int]$Port = 18170,
    [int]$ProxyPort = 18171,
    [ValidateSet('egress-block', 'slow-viewer', 'all')] [string]$Scenario = 'all'
)

$ErrorActionPreference = 'Stop'
$PSDefaultParameterValues['Get-Content:Encoding'] = 'utf8'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$OutDir = Join-Path $root $OutDir
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$QtRoot = (Resolve-Path $QtRoot).Path
if ([string]::IsNullOrWhiteSpace($Exe)) {
    $Exe = Join-Path $root 'build\cmake-build-tests-host\Release\desktop_share_network_e2e.exe'
}
$Exe = (Resolve-Path $Exe).Path
$HostJar = (Resolve-Path (Join-Path $root $HostJar)).Path
$PluginJar = (Resolve-Path (Join-Path $root $PluginJar)).Path

$env:PATH = "$QtRoot\bin;$env:PATH"
$env:QML2_IMPORT_PATH = "$QtRoot\qml"
$env:QT_QUICK_BACKEND = 'software'
$script:failures = @()

function New-HostDirectory {
    param([string]$Name)
    $dir = Join-Path $OutDir "host-$Name"
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    New-Item -ItemType Directory -Force -Path (Join-Path $dir 'plugins') | Out-Null
    Copy-Item $HostJar (Join-Path $dir 'Z2Hub.jar')
    Copy-Item $PluginJar (Join-Path $dir 'plugins\z2hub_desktop_share.jar')
    return $dir
}

function Wait-Port {
    param([int]$PortNumber, [int]$TimeoutSeconds = 30)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        try {
            $client = New-Object System.Net.Sockets.TcpClient
            $client.Connect('127.0.0.1', $PortNumber)
            $client.Close()
            return $true
        } catch {
            Start-Sleep -Milliseconds 200
        }
    }
    return $false
}

function Assert-PortFree {
    param([int[]]$PortNumbers)
    foreach ($number in $PortNumbers) {
        if (Wait-Port -PortNumber $number -TimeoutSeconds 1) {
            throw "refusing to reuse an existing listener on 127.0.0.1:$number"
        }
    }
}

function Start-IsolatedHost {
    param([string]$Name, [int]$HostPort)
    $dir = New-HostDirectory -Name $Name
    $out = Join-Path $OutDir "host-$Name.out.log"
    $err = Join-Path $OutDir "host-$Name.err.log"
    $process = Start-Process -FilePath 'java' `
        -ArgumentList @('-jar', 'Z2Hub.jar', '--server', '--listen', '127.0.0.1', '--port', "$HostPort") `
        -WorkingDirectory $dir -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $out -RedirectStandardError $err
    if (-not (Wait-Port -PortNumber $HostPort -TimeoutSeconds 40)) {
        try { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue } catch {}
        throw "isolated host did not listen on $HostPort"
    }
    Start-Sleep -Milliseconds 1500
    return @{ Process = $process; Directory = $dir; Out = $out; Err = $err; Port = $HostPort }
}

function Stop-IsolatedHost {
    param($HostInfo)
    if ($HostInfo -and $HostInfo.Process) {
        # Windows PowerShell 5.1's Process object has no Kill(bool) overload; use
        # the native Stop-Process path so the listener really goes away.
        try { Stop-Process -Id $HostInfo.Process.Id -Force -ErrorAction SilentlyContinue } catch {}
        try { $HostInfo.Process.WaitForExit(10000) | Out-Null } catch {}
        if (Wait-Port -PortNumber $HostInfo.Port -TimeoutSeconds 5) {
            throw "isolated host still listens on $($HostInfo.Port) after stop"
        }
    }
}

function Read-Json {
    param([string]$Path)
    if (-not (Test-Path $Path)) { return $null }
    try { return Get-Content $Path -Raw | ConvertFrom-Json } catch { return $null }
}

function Measure-LongestPlateau {
    param($Json)
    $samples = $Json.frameSamples
    if (-not $samples -or $samples.Count -lt 2) { return 0 }
    $longest = 0
    $startIndex = 0
    for ($index = 1; $index -le $samples.Count; $index++) {
        $broken = ($index -eq $samples.Count) -or ($samples[$index].frames -ne $samples[$startIndex].frames)
        if ($broken) {
            if ($index - $startIndex -ge 2) {
                $span = [long]$samples[$index - 1].epochMs - [long]$samples[$startIndex].epochMs
                if ($span -gt $longest) { $longest = $span }
            }
            $startIndex = $index
        }
    }
    return $longest
}

function Invoke-EgressBlock {
    Write-Host '=== egress-block: real host, relay cuts server->client UDP for 2500ms ==='
    Assert-PortFree @($Port, $ProxyPort)
    $hostInfo = Start-IsolatedHost -Name 'egress' -HostPort $Port
    $proxyOut = Join-Path $OutDir 'proxy-egress.out.log'
    $proxyErr = Join-Path $OutDir 'proxy-egress.err.log'
    $proxyStats = Join-Path $OutDir 'proxy-egress.json'
    $proxy = Start-Process -FilePath 'python' -ArgumentList @(
        (Join-Path $root 'src\tests\desktop-share\loss_proxy.py'),
        '--listen-port', "$ProxyPort", '--server-port', "$Port",
        '--stats', $proxyStats, '--seconds', '90', '--drop-mode', 'none',
        '--block-after-server-udp', '400', '--block-for-ms', '2500'
    ) -PassThru -WindowStyle Hidden -RedirectStandardOutput $proxyOut -RedirectStandardError $proxyErr
    Start-Sleep -Milliseconds 1200
    $runOut = Join-Path $OutDir 'egress-block'
    $code = 1
    try {
        & (Join-Path $root 'src\tests\desktop-share\run-stage4-e2e.ps1') `
            -QtRoot $QtRoot -Server "127.0.0.1:$ProxyPort" -OutDir $runOut -Exe $Exe `
            -Scenario single -Profile '720p30' -Codec 'h264' -DurationMs 18000 `
            -LateJoinMs 0 -SampleMs 500 `
            -PublisherLifetimeMs 40000 `
            -HostRuntime (Join-Path $root 'build/dist/Z2-windows-x64')
        $code = $LASTEXITCODE
    } catch {
        $script:failures += "egress-block e2e: $_"
    }
    Start-Sleep -Milliseconds 500
    if ($proxy) { try { Stop-Process -Id $proxy.Id -Force -ErrorAction SilentlyContinue } catch {} }
    $stats = Read-Json $proxyStats
    if (-not $stats -or [int]$stats.blockWindows -lt 1 -or [int]$stats.blockedServerUdp -lt 1) {
        $script:failures += 'egress-block: relay never cut the server->client direction'
    }
    $viewer = Read-Json (Join-Path $runOut 'single-h264-720p30-viewer1.json')
    if (-not $viewer -or $viewer.ok -ne $true) {
        $script:failures += 'egress-block: viewer1 did not pass'
    } else {
        $statsMap = $viewer.stats
        $plateau = Measure-LongestPlateau $viewer
        $script:blockPlateau = $plateau
        if ([int]$viewer.endFrames -le [int]$viewer.startFrames) {
            $script:failures += 'egress-block: no continued decoding after recovery'
        }
        if ([int]$statsMap.gapsDetected -lt 1) {
            $script:failures += 'egress-block: viewer did not detect the stall gap'
        }
        if ($plateau -lt 1000) {
            $script:failures += "egress-block: frame timeline has no >=1000ms plateau (got $plateau)"
        }
        if ([int]$statsMap.completePackets -lt 1 -or [int]$statsMap.networkSubmitted -lt 1) {
            $script:failures += 'egress-block: no completed/submitted packets after recovery'
        }
    }
    Stop-IsolatedHost $hostInfo
    if ($code -ne 0) { $script:failures += "egress-block: runner exit=$code" }
    return @{ RunnerExit = $code; BlockWindows = [int]$stats.blockWindows
              BlockedServerUdp = [int]$stats.blockedServerUdp
              PlateauMs = $script:blockPlateau }
}

function Invoke-SlowViewer {
    Write-Host '=== slow-viewer: viewer2 GUI stalls 1500ms while viewer1 keeps decoding ==='
    Assert-PortFree @($Port)
    $hostInfo = Start-IsolatedHost -Name 'slow' -HostPort $Port
    $runOut = Join-Path $OutDir 'slow-viewer'
    $code = 1
    try {
        & (Join-Path $root 'src\tests\desktop-share\run-stage4-e2e.ps1') `
            -QtRoot $QtRoot -Server "127.0.0.1:$Port" -OutDir $runOut -Exe $Exe `
            -Scenario single -Profile '720p30' -Codec 'h264' -DurationMs 14000 `
            -LateJoinMs 0 -SlowViewerMs 1500 -SlowViewerAtMs 4000 -SampleMs 400 `
            -PublisherLifetimeMs 40000 `
            -HostRuntime (Join-Path $root 'build/dist/Z2-windows-x64')
        $code = $LASTEXITCODE
    } catch {
        $script:failures += "slow-viewer e2e: $_"
    }
    $first = Read-Json (Join-Path $runOut 'single-h264-720p30-viewer1.json')
    $second = Read-Json (Join-Path $runOut 'single-h264-720p30-viewer2.json')
    if (-not $first -or $first.ok -ne $true) { $script:failures += 'slow-viewer: viewer1 did not pass' }
    if (-not $second -or $second.ok -ne $true) { $script:failures += 'slow-viewer: viewer2 did not recover' }
    if ($second) {
        if ([int]$second.stats.inboundDroppedBudget -lt 1) {
            $script:failures += 'slow-viewer: viewer2 never overflowed the GUI mailbox'
        }
        if ([int]$second.endFrames -le [int]$second.stallStartFrames) {
            $script:failures += 'slow-viewer: viewer2 did not resume decoding after the stall'
        }
    }
    if ($first -and $second -and $first.frameSamples -and $second.stallStartFrames -ne $null) {
        $start = [long]$second.stallStartEpochMs
        $end = [long]$second.stallEndEpochMs
        $before = $first.frameSamples | Where-Object { [long]$_.epochMs -le $start } | Select-Object -Last 1
        $after = $first.frameSamples | Where-Object { [long]$_.epochMs -ge $end } | Select-Object -First 1
        if (-not $before -or -not $after) {
            $script:failures += 'slow-viewer: viewer1 samples do not cover the stall window'
        } elseif ([int]$after.frames -le [int]$before.frames) {
            $script:failures += 'slow-viewer: viewer1 stopped decoding during viewer2 stall'
        } else {
            $script:viewer1DuringStall = @{ beforeFrames = $before.frames; afterFrames = $after.frames }
        }
    }
    Stop-IsolatedHost $hostInfo
    if ($code -ne 0) { $script:failures += "slow-viewer: runner exit=$code" }
    return @{ RunnerExit = $code; Viewer1Ok = [bool]($first -and $first.ok)
              Viewer2Ok = [bool]($second -and $second.ok)
              Viewer1DuringStall = $script:viewer1DuringStall }
}

$result = @{}
if ($Scenario -eq 'egress-block' -or $Scenario -eq 'all') {
    $result.egressBlock = Invoke-EgressBlock
}
if ($Scenario -eq 'slow-viewer' -or $Scenario -eq 'all') {
    $result.slowViewer = Invoke-SlowViewer
}
$result.failures = $script:failures
$result.blockPlateauMs = $script:blockPlateau
$result.viewer1DuringStall = $script:viewer1DuringStall
$result | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $OutDir 'stage5-e2e-summary.json') -Encoding utf8

if ($script:failures.Count -gt 0) {
    Write-Host 'FAILURES:'
    $script:failures | ForEach-Object { Write-Host " - $_" }
    exit 1
}
Write-Host 'STAGE5 E2E PASSED'
exit 0

# Stage-4 end-to-end driver: real desktop-share .z2plugin + media worker against the
# real Java server. It starts real client processes, waits for their exit codes and
# copies their JSON/screenshot evidence into the output directory.
#
# Every process loads the independently built plugin package through the host
# PluginLoader. The publisher captures a dedicated synthetic window only; viewer
# screenshots show the page and the decoded remote video.
param(
    [Parameter(Mandatory = $true)] [string]$QtRoot,
    [Parameter(Mandatory = $true)] [string]$Server,
    [Parameter(Mandatory = $true)] [string]$OutDir,
    [string]$Exe = '',
    [ValidateSet('matrix', 'single', 'switch', 'tworoom')] [string]$Scenario = 'matrix',
    [string]$Profile = '720p30',
    [string]$Codec = 'bgra-tiles',
    [int]$DurationMs = 20000,
    [int]$LateJoinMs = 5000,
    [int]$SlowViewerMs = 0,
    [int]$SlowViewerAtMs = 0,
    [int]$SwitchAtMs = 6000,
    [int]$SampleMs = 0,
    [string]$HostRuntime = '',
    [int]$PublisherLifetimeMs = 0
)

$ErrorActionPreference = 'Stop'
$QtRoot = (Resolve-Path $QtRoot).Path
if ([string]::IsNullOrWhiteSpace($Exe)) {
    $Exe = Join-Path $PSScriptRoot '..\..\..\build\cmake-build-tests-host\Release\desktop_share_network_e2e.exe'
}
$Exe = (Resolve-Path $Exe).Path
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$script:failures = @()

$env:PATH = "$QtRoot\bin;$env:PATH"
$env:QML2_IMPORT_PATH = "$QtRoot\qml"
$env:QML_IMPORT_PATH = "$QtRoot\qml"
$env:QT_PLUGIN_PATH = "$QtRoot\plugins"
$env:QT_QPA_PLATFORM_PLUGIN_PATH = "$QtRoot\plugins\platforms"
$env:QT_QUICK_BACKEND = 'software'

# The e2e executable is not a windeployqt target. Deploy the matching host runtime
# DLLs next to it (the same rule the media test launcher uses) so QML style plugins
# can resolve their Qt dependencies without a stale local copy.
if (-not [string]::IsNullOrWhiteSpace($HostRuntime)) {
    $runtime = (Resolve-Path $HostRuntime).Path
    $kitCore = Join-Path $QtRoot 'bin\Qt6Core.dll'
    if ((Get-FileHash (Join-Path $runtime 'Qt6Core.dll')).Hash -ne (Get-FileHash $kitCore).Hash) {
        throw "HostRuntime Qt Core does not match QtRoot: $runtime"
    }
    $destination = Split-Path $Exe
    foreach ($file in (Get-ChildItem -LiteralPath $runtime -File -Filter '*.dll')) {
        if ($file.Name -match '^(Qt6Multimedia|avcodec-|avformat-|avutil-|avfilter-|avdevice-|swresample-|swscale-)') {
            throw "HostRuntime contains media implementation libraries: $($file.Name)"
        }
        $target = Join-Path $destination $file.Name
        if (!(Test-Path $target) -or
            (Get-FileHash $target).Hash -ne (Get-FileHash $file.FullName).Hash) {
            Copy-Item -LiteralPath $file.FullName -Destination $target -Force
        }
    }
}

function Start-ZdsClient {
    param(
        [string]$Role,
        [string]$Name,
        [string]$Tag,
        [string]$ProfileName,
        [string]$CodecName,
        [int]$Duration,
        [string]$RoomFile,
        [int]$ExpectViewers = 0,
        [string]$ExpectTag = '',
        [string]$ExpectEndTag = '',
        [int]$ExpectWidth = 0,
        [int]$ExpectHeight = 0,
        [int]$ExpectEndWidth = 0,
        [int]$ExpectEndHeight = 0,
        [int]$AfterSwitchMs = 0,
        [int]$SwitchAt = -1,
        [string]$SwitchProfile = '',
        [string]$SwitchTagName = '',
        [int]$StallMs = 0,
        [int]$StallAtMs = 0,
        [int]$SampleMs = 0
    )
    $env:ZDS_E2E_ROLE = $Role
    $env:ZDS_E2E_SERVER = $Server
    $env:ZDS_E2E_PROFILE = $ProfileName
    $env:ZDS_E2E_CODEC = $CodecName
    $env:ZDS_E2E_TAG = $Tag
    $env:ZDS_E2E_DURATION_MS = "$Duration"
    $env:ZDS_E2E_ROOMCODE_FILE = $RoomFile
    $env:ZDS_E2E_OUT = Join-Path $OutDir "$Name.json"
    $env:ZDS_E2E_SCREENSHOT = Join-Path $OutDir "$Name.png"
    $env:ZDS_E2E_EXPECT_VIEWERS = "$ExpectViewers"
    $env:ZDS_E2E_EXPECT_TAG = $ExpectTag
    $env:ZDS_E2E_EXPECT_END_TAG = $ExpectEndTag
    $env:ZDS_E2E_EXPECT_WIDTH = "$ExpectWidth"
    $env:ZDS_E2E_EXPECT_HEIGHT = "$ExpectHeight"
    $env:ZDS_E2E_EXPECT_END_WIDTH = "$ExpectEndWidth"
    $env:ZDS_E2E_EXPECT_END_HEIGHT = "$ExpectEndHeight"
    $env:ZDS_E2E_AFTER_SWITCH_MS = "$AfterSwitchMs"
    $env:ZDS_E2E_SWITCH_AT_MS = "$SwitchAt"
    $env:ZDS_E2E_SWITCH_PROFILE = $SwitchProfile
    $env:ZDS_E2E_SWITCH_TAG = $SwitchTagName
    $env:ZDS_E2E_STALL_MS = "$StallMs"
    $env:ZDS_E2E_STALL_AT_MS = "$StallAtMs"
    $env:ZDS_E2E_SAMPLE_MS = "$SampleMs"
    $process = Start-Process -FilePath $Exe -ArgumentList @('-platform', 'windows') -PassThru `
        -WindowStyle Hidden -RedirectStandardOutput (Join-Path $OutDir "$Name.out.log") `
        -RedirectStandardError (Join-Path $OutDir "$Name.err.log")
    # Keep the native handle open: with redirected output PowerShell may otherwise
    # report an empty ExitCode after the process exits.
    $null = $process.Handle
    Write-Host "started $Name pid=$($process.Id)"
    return $process
}

function Wait-RoomFile {
    param([string]$Path, [int]$TimeoutSeconds = 90)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $Path) {
            $code = (Get-Content $Path -Raw).Trim()
            if ($code.Length -eq 8) { return $code }
        }
        Start-Sleep -Milliseconds 200
    }
    throw "room file timeout: $Path"
}

function Wait-Clients {
    param([System.Collections.ArrayList]$Processes, [int]$TimeoutSeconds = 180)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    foreach ($entry in $Processes) {
        while (-not $entry.Process.HasExited -and (Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 200
        }
        if (-not $entry.Process.HasExited) {
            Write-Warning "timeout: killing $($entry.Name)"
            try { $entry.Process.Kill($true) } catch {}
            $script:failures += "$($entry.Name): timeout"
        } else {
            $null = $entry.Process.Handle
            $entry.Process.WaitForExit()
            $entry.Process.Refresh()
            $code = $entry.Process.ExitCode
            $json = $null
            $jsonPath = Join-Path $OutDir "$($entry.Name).json"
            if (Test-Path $jsonPath) {
                try { $json = Get-Content $jsonPath -Raw -Encoding utf8 | ConvertFrom-Json } catch {}
            }
            $ok = ($json -and ($json.ok -eq $true) -and ($null -ne $code) -and ($code -eq 0))
            if (-not $ok) {
                $reason = if ($json) { $json.error } else { "no json" }
                $script:failures += "$($entry.Name): exit=$code error=$reason"
                Write-Host "FAIL $($entry.Name) exit=$code $reason"
            } else {
                Write-Host "PASS $($entry.Name) exit=$code"
            }
        }
    }
}

function Invoke-Single {
    param(
        [string]$Label,
        [string]$ProfileName,
        [string]$CodecName,
        [string]$Tag,
        [int]$ViewerCount = 2,
        [int]$ViewerDelayMs = 0,
        [int]$Duration = 20000
    )
    Write-Host "=== scenario $Label ($CodecName $ProfileName, viewers=$ViewerCount) ==="
    $roomFile = Join-Path $OutDir "$Label.room.tmp"
    Remove-Item $roomFile -ErrorAction SilentlyContinue
    $width = if ($ProfileName -eq '1080p30') { 1920 } else { 1280 }
    $height = if ($ProfileName -eq '1080p30') { 1080 } else { 720 }
    $processes = [System.Collections.ArrayList]@()
    # The publisher outlives the viewers so a viewer never observes the room
    # closing while its own observation window is still running.
    $publisherDuration = if ($PublisherLifetimeMs -gt 0) { $PublisherLifetimeMs } else { $Duration + 8000 }
    $publisher = Start-ZdsClient -Role 'publisher' -Name "$Label-publisher" -Tag $Tag `
        -ProfileName $ProfileName -CodecName $CodecName -Duration $publisherDuration -RoomFile $roomFile `
        -ExpectViewers $ViewerCount -SampleMs $SampleMs
    [void]$processes.Add(@{ Name = "$Label-publisher"; Process = $publisher })
    Wait-RoomFile -Path $roomFile | Out-Null
    for ($index = 1; $index -le $ViewerCount; $index++) {
        if ($ViewerDelayMs -gt 0 -and $index -gt 1) { Start-Sleep -Milliseconds $ViewerDelayMs }
        $viewer = Start-ZdsClient -Role 'viewer' -Name "$Label-viewer$index" -Tag $Tag `
            -ProfileName $ProfileName -CodecName $CodecName -Duration $Duration -RoomFile $roomFile `
            -ExpectTag $Tag -ExpectWidth $width -ExpectHeight $height `
            -StallMs $(if ($index -eq 2) { $SlowViewerMs } else { 0 }) -SampleMs $SampleMs `
            -StallAtMs $(if ($index -eq 2) { $SlowViewerAtMs } else { 0 })
        [void]$processes.Add(@{ Name = "$Label-viewer$index"; Process = $viewer })
    }
    Wait-Clients -Processes $processes -TimeoutSeconds ($Duration / 1000 + 120)
    Remove-Item $roomFile -ErrorAction SilentlyContinue
}

function Invoke-Switch {
    param([string]$Label)
    Write-Host "=== scenario $Label (switch profile + source while sharing) ==="
    $roomFile = Join-Path $OutDir "$Label.room.tmp"
    Remove-Item $roomFile -ErrorAction SilentlyContinue
    $processes = [System.Collections.ArrayList]@()
    $publisher = Start-ZdsClient -Role 'publisher' -Name "$Label-publisher" -Tag 'A' `
        -ProfileName '720p30' -CodecName 'bgra-tiles' -Duration 38000 -RoomFile $roomFile `
        -ExpectViewers 2 -SwitchAt $SwitchAtMs -SwitchProfile '1080p30' -SwitchTagName 'C'
    [void]$processes.Add(@{ Name = "$Label-publisher"; Process = $publisher })
    Wait-RoomFile -Path $roomFile | Out-Null
    $viewer1 = Start-ZdsClient -Role 'viewer' -Name "$Label-viewer1" -Tag 'A' `
        -ProfileName '720p30' -CodecName 'bgra-tiles' -Duration 16000 -RoomFile $roomFile `
        -ExpectTag 'A' -ExpectWidth 1280 -ExpectHeight 720 -ExpectEndTag 'C' `
        -ExpectEndWidth 1920 -ExpectEndHeight 1080 -AfterSwitchMs 10000
    [void]$processes.Add(@{ Name = "$Label-viewer1"; Process = $viewer1 })
    Start-Sleep -Milliseconds 6000
    $viewer2 = Start-ZdsClient -Role 'viewer' -Name "$Label-viewer2" -Tag 'A' `
        -ProfileName '720p30' -CodecName 'bgra-tiles' -Duration 12000 -RoomFile $roomFile `
        -ExpectTag 'A' -ExpectWidth 1280 -ExpectHeight 720 -ExpectEndTag 'C' `
        -ExpectEndWidth 1920 -ExpectEndHeight 1080 -AfterSwitchMs 10000
    [void]$processes.Add(@{ Name = "$Label-viewer2"; Process = $viewer2 })
    Wait-Clients -Processes $processes -TimeoutSeconds 240
    Remove-Item $roomFile -ErrorAction SilentlyContinue
}

function Invoke-TwoRooms {
    Write-Host "=== scenario two-rooms (room A + isolated room B) ==="
    $roomA = Join-Path $OutDir 'roomA.room.tmp'
    $roomB = Join-Path $OutDir 'roomB.room.tmp'
    Remove-Item $roomA, $roomB -ErrorAction SilentlyContinue
    $processes = [System.Collections.ArrayList]@()
    $pubA = Start-ZdsClient -Role 'publisher' -Name 'roomA-publisher' -Tag 'A' `
        -ProfileName '720p30' -CodecName 'h264' -Duration 20000 -RoomFile $roomA -ExpectViewers 1
    [void]$processes.Add(@{ Name = 'roomA-publisher'; Process = $pubA })
    $pubB = Start-ZdsClient -Role 'publisher' -Name 'roomB-publisher' -Tag 'B' `
        -ProfileName '720p30' -CodecName 'h264' -Duration 20000 -RoomFile $roomB -ExpectViewers 1
    [void]$processes.Add(@{ Name = 'roomB-publisher'; Process = $pubB })
    Wait-RoomFile -Path $roomA | Out-Null
    Wait-RoomFile -Path $roomB | Out-Null
    $viewA = Start-ZdsClient -Role 'viewer' -Name 'roomA-viewer' -Tag 'A' -ProfileName '720p30' `
        -CodecName 'h264' -Duration 14000 -RoomFile $roomA -ExpectTag 'A' `
        -ExpectWidth 1280 -ExpectHeight 720
    [void]$processes.Add(@{ Name = 'roomA-viewer'; Process = $viewA })
    $viewB = Start-ZdsClient -Role 'viewer' -Name 'roomB-viewer' -Tag 'B' -ProfileName '720p30' `
        -CodecName 'h264' -Duration 14000 -RoomFile $roomB -ExpectTag 'B' `
        -ExpectWidth 1280 -ExpectHeight 720
    [void]$processes.Add(@{ Name = 'roomB-viewer'; Process = $viewB })
    Wait-Clients -Processes $processes -TimeoutSeconds 240
    Remove-Item $roomA, $roomB -ErrorAction SilentlyContinue
}

switch ($Scenario) {
    'single' { Invoke-Single -Label "single-$Codec-$Profile" -ProfileName $Profile -CodecName $Codec -Tag 'A' -ViewerCount 2 -ViewerDelayMs $LateJoinMs -Duration $DurationMs }
    'switch' { Invoke-Switch -Label 'switch' }
    'tworoom' { Invoke-TwoRooms }
    'matrix' {
        Invoke-Single -Label 'tiles-720p' -ProfileName '720p30' -CodecName 'bgra-tiles' -Tag 'A' -ViewerCount 2 -Duration $DurationMs
        Invoke-Single -Label 'tiles-1080p' -ProfileName '1080p30' -CodecName 'bgra-tiles' -Tag 'A' -ViewerCount 2 -Duration $DurationMs
        Invoke-Single -Label 'h264-720p' -ProfileName '720p30' -CodecName 'h264' -Tag 'A' -ViewerCount 2 -ViewerDelayMs $LateJoinMs -Duration $DurationMs
        Invoke-Single -Label 'h264-1080p' -ProfileName '1080p30' -CodecName 'h264' -Tag 'A' -ViewerCount 2 -Duration $DurationMs
        Invoke-Switch -Label 'switch'
        Invoke-TwoRooms
    }
}

Start-Sleep -Milliseconds 300
if ($script:failures.Count -gt 0) {
    Write-Host "FAILURES:"
    $script:failures | ForEach-Object { Write-Host " - $_" }
    exit 1
}
Write-Host "ALL SCENARIOS PASSED"
exit 0

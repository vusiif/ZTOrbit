param(
    [Parameter(Mandatory=$true)][string]$QtRoot,
    [string]$Exe='build/cmake-build-tests-host/Release/desktop_share_network_e2e.exe',
    [string]$OutDir='build/followup-stage4/no-media',
    [int]$Port=18140
)
$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$out=(Resolve-Path $OutDir).Path
$room=Join-Path $out 'room.tmp'
$env:PATH="$(Resolve-Path $QtRoot)/bin;$env:PATH"
$env:QML2_IMPORT_PATH="$(Resolve-Path $QtRoot)/qml"
$env:ZDS_E2E_ROLE='viewer'
$env:ZDS_E2E_SERVER="127.0.0.1:$Port"
$env:ZDS_E2E_ROOMCODE_FILE=$room
$env:ZDS_E2E_OUT=Join-Path $out 'viewer.json'
$env:ZDS_E2E_SCREENSHOT=''
$env:ZDS_E2E_EXPECT_NO_MEDIA='1'
$env:ZDS_E2E_STALL_MS='0'
$helper=Join-Path $PSScriptRoot 'silent_publisher.py'
$pub=Start-Process python -ArgumentList @($helper,'--port',"$Port",'--room-file',$room,'--seconds','60') -PassThru -WindowStyle Hidden -RedirectStandardOutput "$out/publisher.out.log" -RedirectStandardError "$out/publisher.err.log"
try {
    $viewer=Start-Process (Resolve-Path $Exe).Path -ArgumentList @('-platform','windows') -PassThru -WindowStyle Hidden -RedirectStandardOutput "$out/viewer.out.log" -RedirectStandardError "$out/viewer.err.log"
    $null=$viewer.Handle
    if(!$viewer.WaitForExit(90000)){throw 'no-media test did not complete'}
    $viewer.Refresh()
    $result=Get-Content "$out/viewer.json" -Raw | ConvertFrom-Json
    if(!$result.ok -or $viewer.ExitCode -ne 0){throw "no-media test failed: $($result.error)"}
    Write-Host "PASS no-media: connected=$($result.stats.connected) decoded=$($result.stats.decodedFrames) elapsed=$($result.timeoutElapsedMs)ms"
} finally {
    if(!$pub.HasExited){Stop-Process -Id $pub.Id}
    Remove-Item -LiteralPath $room -ErrorAction SilentlyContinue
    Remove-Item Env:ZDS_E2E_EXPECT_NO_MEDIA -ErrorAction SilentlyContinue
}

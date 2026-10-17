param(
    [Parameter(Mandatory=$true)][string]$QtRoot,
    [Parameter(Mandatory=$true)][string]$TestExecutable,
    [string[]]$TestArguments=@(),
    [string[]]$SupportBin=@(),
    [string]$HostRuntime='',
    [int]$TimeoutSeconds=180
)
$ErrorActionPreference='Stop'
$kit=(Resolve-Path -LiteralPath $QtRoot).Path
$exe=(Resolve-Path -LiteralPath $TestExecutable).Path
$platform=Join-Path $kit 'plugins/platforms/qwindows.dll'
if(!(Test-Path -LiteralPath $platform)) {throw "Qt Windows platform plugin is missing: $platform"}
if($HostRuntime) {
    $runtime=(Resolve-Path -LiteralPath $HostRuntime).Path
    if((Get-FileHash -LiteralPath (Join-Path $runtime 'Qt6Core.dll')).Hash -ne (Get-FileHash -LiteralPath (Join-Path $kit 'bin/Qt6Core.dll')).Hash) {throw 'HostRuntime Qt Core does not match QtRoot'}
    $destination=Split-Path $exe
    foreach($file in (Get-ChildItem -LiteralPath $runtime -File -Filter '*.dll')) {
        if($file.Name -match '^(Qt6Multimedia|avcodec-|avformat-|avutil-|avfilter-|avdevice-|swresample-|swscale-)') {throw 'HostRuntime contains media implementation libraries'}
        $target=Join-Path $destination $file.Name
        if(!(Test-Path -LiteralPath $target) -or (Get-FileHash -LiteralPath $target).Hash -ne (Get-FileHash -LiteralPath $file.FullName).Hash) {Copy-Item -LiteralPath $file.FullName -Destination $target -Force}
    }
}
$localCore=Join-Path (Split-Path $exe) 'Qt6Core.dll'
if((Test-Path -LiteralPath $localCore) -and
   (Get-FileHash -LiteralPath $localCore).Hash -ne (Get-FileHash -LiteralPath (Join-Path $kit 'bin/Qt6Core.dll')).Hash) {
    throw 'Test directory contains a different Qt Core; use a clean build directory'
}
$saved=@{};foreach($name in @('PATH','QT_PLUGIN_PATH','QT_QPA_PLATFORM_PLUGIN_PATH','QML_IMPORT_PATH','QML2_IMPORT_PATH','QT_QUICK_BACKEND','QTEST_FUNCTION_TIMEOUT')) {$saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
$logStem=[IO.Path]::GetFileNameWithoutExtension($exe)+'.launch-'+[guid]::NewGuid().ToString('N')
$stdout=Join-Path (Split-Path $exe) ($logStem+'.stdout.log')
$stderr=Join-Path (Split-Path $exe) ($logStem+'.stderr.log')
try {
    $paths=@()
    foreach($directory in $SupportBin) {
        $support=(Resolve-Path -LiteralPath $directory).Path
        $supportCore=Join-Path $support 'Qt6Core.dll'
        if((Test-Path -LiteralPath $supportCore) -and (Get-FileHash -LiteralPath $supportCore).Hash -ne (Get-FileHash -LiteralPath (Join-Path $kit 'bin/Qt6Core.dll')).Hash) {throw 'SupportBin contains a different Qt Core; select the matching SDK/QtRoot'}
        $paths+=$support
    }
    $paths+=Join-Path $kit 'bin'
    $env:PATH=($paths -join ';')+';'+$saved.PATH
    $env:QT_PLUGIN_PATH=Join-Path $kit 'plugins'
    $env:QT_QPA_PLATFORM_PLUGIN_PATH=Join-Path $kit 'plugins/platforms'
    $env:QML_IMPORT_PATH=Join-Path $kit 'qml'
    $env:QML2_IMPORT_PATH=Join-Path $kit 'qml'
    $env:QT_QUICK_BACKEND='software'
    $env:QTEST_FUNCTION_TIMEOUT=[string]($TimeoutSeconds*1000)
    # Do not allow loader/OS fault dialogs to hold an unattended test indefinitely.
    if(!('ZTMediaTestErrorMode' -as [type])) {Add-Type 'using System.Runtime.InteropServices; public static class ZTMediaTestErrorMode { [DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint value); }'}
    $oldMode=[ZTMediaTestErrorMode]::SetErrorMode(0x8003)
    try {
        $arguments=@()
        foreach($argument in $TestArguments) {
            if($argument.Contains('"')) {throw 'Test argument contains an unsupported quote'}
            $arguments+='"'+$argument+'"'
        }
        $parameters=@{FilePath=$exe;PassThru=$true;WindowStyle='Hidden';RedirectStandardOutput=$stdout;RedirectStandardError=$stderr}
        if($arguments.Count) {$parameters.ArgumentList=$arguments}
        $process=Start-Process @parameters
        if(!$process.WaitForExit($TimeoutSeconds*1000)) {$process.Kill();$process.WaitForExit();throw "Test timed out and was terminated: $exe"}
        $process.Refresh()
        Get-Content -LiteralPath $stdout -ErrorAction SilentlyContinue
        Get-Content -LiteralPath $stderr -ErrorAction SilentlyContinue
        if($process.ExitCode -ne 0) {throw "Test failed with exit code $($process.ExitCode): $exe"}
    } finally {[void][ZTMediaTestErrorMode]::SetErrorMode($oldMode)}
} finally {foreach($name in $saved.Keys) {[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')}}

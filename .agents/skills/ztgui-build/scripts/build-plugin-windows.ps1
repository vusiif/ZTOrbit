param(
    [Parameter(Mandatory=$true)][string]$PluginDirectory,
    [Parameter(Mandatory=$true)][string]$Target,
    [Parameter(Mandatory=$true)][string]$QtRoot,
    [Parameter(Mandatory=$true)][string]$ApiSdk,
    [string]$ProjectDirectory='.',
    [string]$BuildDirectory='',
    [string]$OutputDirectory='build/dist/plugins',
    [string[]]$AdditionalSdk=@(),
    [string[]]$CMakeArguments=@()
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'PackageTools.ps1')
$project=(Resolve-Path -LiteralPath $ProjectDirectory).Path
$plugin=(Resolve-Path -LiteralPath $(if ([IO.Path]::IsPathRooted($PluginDirectory)) {$PluginDirectory} else {Join-Path $project $PluginDirectory})).Path
$kit=(Resolve-Path -LiteralPath $QtRoot).Path
$sdk=(Resolve-Path -LiteralPath $ApiSdk).Path
$metadataPath=Join-Path $sdk 'sdk-tools.json'
if (!(Test-Path -LiteralPath $metadataPath)) {throw 'SDK lacks v2 packaging metadata; update SDK using build-windows.ps1 first'}
$metadata=Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
if ($metadata.packageFormats -notcontains 2) {throw 'SDK does not support v2 packages; update SDK first'}
foreach ($file in $metadata.files) {
    $path=Join-Path $sdk $file.path
    if (!(Test-Path -LiteralPath $path) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.sha256) {throw "SDK input is missing or modified: $path; update SDK first"}
}
# A locally maintained SDK must match this checkout's current public/tool sources.
foreach ($file in $metadata.sources) {
    $path=Join-Path $project $file.path
    if ((Test-Path -LiteralPath $path) -and (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.sha256) {throw "SDK is stale for $($file.path); update SDK first"}
}
if ($Target -notmatch '^[A-Za-z0-9_-]+$') {throw 'Invalid target name'}
if (!$BuildDirectory) {$BuildDirectory="build/cmake-build-plugin-$Target-windows-x64"}
$build=Resolve-PackageDirectory $project $BuildDirectory
$output=Resolve-PackageDirectory $project $OutputDirectory
if ($output -eq $sdk -or $output.StartsWith($sdk+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {throw 'Plugin output must be separate from SDK'}
$cache=Join-Path $build 'CMakeCache.txt'
if (Test-Path -LiteralPath $cache) {
    $entry=Get-Content -LiteralPath $cache | Where-Object {$_ -like 'CMAKE_HOME_DIRECTORY:INTERNAL=*'} | Select-Object -First 1
    if ($entry -and $entry.Split('=',2)[1].Replace('/','\') -ne $plugin.Replace('/','\')) {throw 'BuildDirectory belongs to another project; select a fresh build directory'}
}
$prefix=@($sdk,$kit)
$sdkDirectories=@()
foreach($additional in $AdditionalSdk) {
    $additionalRoot=(Resolve-Path -LiteralPath $additional).Path
    $prefix+=$additionalRoot
    foreach($package in (Get-ChildItem -LiteralPath (Join-Path $additionalRoot 'lib/cmake') -Directory)) {
        if(Test-Path -LiteralPath (Join-Path $package.FullName ($package.Name+'Config.cmake'))) {
            $sdkDirectories+=('-D'+$package.Name+'_DIR='+$package.FullName.Replace('\','/'))
        }
    }
}
Invoke-Checked cmake (@('-S',$plugin,'-B',$build,'-G','Visual Studio 17 2022','-A','x64',('-DCMAKE_PREFIX_PATH='+($prefix -join ';')),"-DZTGUIPluginApi_DIR=$sdk/lib/cmake/ZTGUIPluginApi")+$sdkDirectories+$CMakeArguments)
Invoke-Checked cmake @('--build',$build,'--config','Release','--target',$Target,'--parallel')
Invoke-Checked cmake @('--build',$build,'--config','Release','--target',($Target+'_package'),'--parallel')
$package=Join-Path $build "Release/$Target.ztplugin"
if (!(Test-Path -LiteralPath $package)) {throw "Expected package missing: $package"}
$packageInfo=(Invoke-Checked (Join-Path $sdk 'bin/ztplugin_pack.exe') @('--inspect',$package)) -join "`n" | ConvertFrom-Json
$pluginVersion=[string]$packageInfo.version
if ($pluginVersion -notmatch '^[0-9]+(?:\.[0-9]+)*(?:-[A-Za-z0-9.-]+)?(?:\+[A-Za-z0-9.-]+)?$') {throw 'Plugin version cannot be used in the package filename'}
$packageName="$Target-v$pluginVersion.ztplugin"
$null=New-Item -ItemType Directory -Path $output -Force
$temporary=Join-Path $output ('.'+$Target+'.publish-'+[guid]::NewGuid().ToString('N')+'.ztplugin')
$licenseDestination=Join-Path $output "$Target-licenses"
$licenseStaging=New-PackageStaging $licenseDestination
try {
    Copy-Item -LiteralPath $package -Destination $temporary
    Invoke-Checked (Join-Path $sdk 'bin/ztplugin_pack.exe') @('--extract-licenses',$temporary,'--output',$licenseStaging)
    $destination=Join-Path $output $packageName
    if (Test-Path -LiteralPath $destination) {
        $item=Get-Item -LiteralPath $destination -Force
        if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {throw 'Package destination must be an ordinary file'}
    }
    Publish-PackageDirectory $licenseStaging $licenseDestination
    $licenseStaging=$null
    if (Test-Path -LiteralPath $destination) {[IO.File]::Replace($temporary,$destination,[NullString]::Value)}
    else {[IO.File]::Move($temporary,$destination)}
} finally {
    if (Test-Path -LiteralPath $temporary) {Remove-Item -LiteralPath $temporary -Force}
    if ($licenseStaging) {Write-Warning "Unpublished license staging preserved at $licenseStaging"}
}
Write-Host "Created $(Join-Path $output $packageName)"

param(
    [Parameter(Mandatory=$true)][string]$QtRoot,
    [Parameter(Mandatory=$true)][string]$ApiSdk,
    [string]$ProjectDirectory='.',
    [string]$OutputDirectory='build/dist/plugins',
    [string]$OpusSource='',
    [string]$FfmpegRoot='',
    [string]$ExtensionBundle='',
    [string]$BuildDirectory='build/cmake-build-media-v2-worker'
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'PackageTools.ps1')
$project=(Resolve-Path -LiteralPath $ProjectDirectory).Path
$kit=(Resolve-Path -LiteralPath $QtRoot).Path
$hostSdk=(Resolve-Path -LiteralPath $ApiSdk).Path
if(!$FfmpegRoot) {
    & (Join-Path $PSScriptRoot 'build-ffmpeg-windows.ps1') -ProjectDirectory $project
    if(!$?) {throw 'FFmpeg development SDK build failed'}
    $FfmpegRoot=Join-Path $project 'build/cmake-build-media-dependencies/installed'
}
$ffmpeg=(Resolve-Path -LiteralPath $FfmpegRoot).Path
foreach($package in @(@{source='api';build='build/cmake-build-media-v2-api';output='build/dist/Z2-media-api-v2'},@{source='engine-sdk';build='build/cmake-build-media-v2-engine-sdk';output='build/dist/Z2-media-engine-sdk-v1'})) {
    $source=Join-Path $project ('src/plugins/media/client/'+$package.source)
    $build=Join-Path $project $package.build
    $output=Resolve-PackageDirectory $project $package.output
    $statePath=Join-Path $build 'package-state.json'
    $example=if($package.source -eq 'api') {'preview'} else {'flip'}
    $inputs=(Get-DirectoryFingerprint $source)+':'+(Get-DirectoryFingerprint (Join-Path $project ('src/plugins/media/client/examples/'+$example)))+':'+(Get-DirectoryFingerprint $hostSdk)+':'+(Get-FileHash -LiteralPath (Join-Path $kit 'bin/Qt6Core.dll')).Hash+':'+(Get-FileHash -LiteralPath $PSCommandPath).Hash
    if($package.source -eq 'engine-sdk') {$inputs+=':'+(Get-DirectoryFingerprint $ffmpeg)}
    $state=$null
    if(Test-Path -LiteralPath $statePath) {try {$state=Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json} catch {$state=$null}}
    $current=if(Test-Path -LiteralPath $output) {Get-DirectoryFingerprint $output} else {''}
    if(!$state -or $state.inputs -ne $inputs -or $state.sdk -ne $current) {
        Invoke-Checked cmake @('-S',$source,'-B',$build,'-G','Visual Studio 17 2022','-A','x64',"-DCMAKE_PREFIX_PATH=$hostSdk;$kit","-DZTOrbitPluginApi_DIR=$hostSdk/lib/cmake/ZTOrbitPluginApi","-DMEDIA_FFMPEG_ROOT=$ffmpeg")
        Invoke-Checked cmake @('--build',$build,'--config','Release','--parallel')
        $stage=New-PackageStaging $output
        Invoke-Checked cmake @('--install',$build,'--config','Release','--prefix',$stage)
        Publish-PackageDirectory $stage $output
        Write-PackageArchive $output
    } elseif(!(Test-Path -LiteralPath ($output+'.zip')) -or (Get-FileHash -LiteralPath ($output+'.zip')).Hash -ne $state.archive) {
        Write-PackageArchive $output
    } else {Write-Host "Reusing verified SDK $output"}
    @{inputs=$inputs;sdk=(Get-DirectoryFingerprint $output);archive=(Get-FileHash -LiteralPath ($output+'.zip')).Hash} | ConvertTo-Json | Set-Content -LiteralPath $statePath -Encoding utf8

}
$extensionArgument=if($ExtensionBundle) {(Resolve-Path -LiteralPath $ExtensionBundle).Path.Replace('\','/')} else {''}
$extra=@("-DMEDIA_FFMPEG_ROOT=$ffmpeg","-DMEDIA_EXTENSION_BUNDLE=$extensionArgument")
if($OpusSource) {$extra+=('-DFETCHCONTENT_SOURCE_DIR_OPUS='+(Resolve-Path -LiteralPath $OpusSource).Path.Replace('\','/'))}
$apiOutput=Join-Path $project 'build/dist/Z2-media-api-v2'
& (Join-Path $PSScriptRoot 'build-plugin-windows.ps1') -PluginDirectory 'src/plugins/media/client' -Target zto_media_worker -BuildDirectory $BuildDirectory -QtRoot $kit -ApiSdk $hostSdk -AdditionalSdk $apiOutput -CMakeArguments $extra -ProjectDirectory $project -OutputDirectory $OutputDirectory

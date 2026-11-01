param(
    [Parameter(Mandatory=$true)][string]$QtRoot,
    [string]$ProjectDirectory = '.',
    [string]$SodiumRoot = '',
    [string]$OutputDirectory = 'build/dist/Z2-windows-x64',
    [string]$PluginOutputDirectory = 'build/dist/plugins',
    [switch]$RebuildApi,
    [switch]$BuildVoicePlugin,
    [switch]$BuildMediaPlugin,
    [switch]$IncludeOpenGLSoftware,
    [switch]$KeepAllQmlStyles
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PackageTools.ps1')
$projectRoot = (Resolve-Path -LiteralPath $ProjectDirectory).Path
if (!(Test-Path -LiteralPath (Join-Path $projectRoot 'src/client/main/CMakeLists.txt')) -or
    !(Test-Path -LiteralPath (Join-Path $projectRoot 'src/client/api/CMakeLists.txt'))) { throw 'ProjectDirectory must be the ZTOrbit repository root' }
$qtSdk = (Resolve-Path -LiteralPath $QtRoot).Path
$deployQt = Join-Path $qtSdk 'bin/windeployqt.exe'
if (!(Test-Path -LiteralPath $deployQt)) { throw 'QtRoot must contain a shared Qt SDK with windeployqt' }
$outputPath = Resolve-PackageDirectory $projectRoot $OutputDirectory
$pluginOutputPath = Resolve-PackageDirectory $projectRoot $PluginOutputDirectory
if ($pluginOutputPath -eq $outputPath -or
    $pluginOutputPath.StartsWith($outputPath + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
    $outputPath.StartsWith($pluginOutputPath + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'PluginOutputDirectory must be separate from the application directory'
}
$stagingPath = $null
$apiStaging = $null
$sodiumConfigure = @()
if ($SodiumRoot) { $sodiumConfigure = @('-DZTO_SODIUM_ROOT=' + (Resolve-Path -LiteralPath $SodiumRoot).Path.Replace('\','/')) }
# A source move invalidates CMake's recorded source directory. Keep the old
# cache as a backup and configure a fresh one at the normal build path.
function Protect-SourceBuildCache([string]$BuildPath, [string]$SourcePath) {
    $cachePath = Join-Path $projectRoot "$BuildPath/CMakeCache.txt"
    if (!(Test-Path -LiteralPath $cachePath)) { return }
    $homeEntry = Get-Content -LiteralPath $cachePath | Where-Object { $_ -like 'CMAKE_HOME_DIRECTORY:INTERNAL=*' } | Select-Object -First 1
    $expectedSource = [IO.Path]::GetFullPath((Join-Path $projectRoot $SourcePath)).Replace('\','/')
    if ($homeEntry -and $homeEntry.Split('=',2)[1].TrimEnd('/') -ne $expectedSource.TrimEnd('/')) {
        $resolvedBuild = [IO.Path]::GetFullPath((Join-Path $projectRoot $BuildPath))
        if (!$resolvedBuild.StartsWith($projectRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Build cache outside project root' }
        Move-Item -LiteralPath $resolvedBuild -Destination ($resolvedBuild + '.previous-' + [guid]::NewGuid())
    }
}
Push-Location -LiteralPath $projectRoot
try {
    Protect-SourceBuildCache 'build/cmake-build-release' '.'
    # Build this target only, even if this build tree also has src/tests/plugins configured.
    Invoke-Checked cmake (@('-S', '.', '-B', 'build/cmake-build-release', '-G', 'Visual Studio 17 2022', '-A', 'x64', '-DZTO_BUILD_TESTS=OFF', "-DCMAKE_PREFIX_PATH=$qtSdk") + $sodiumConfigure)
    Invoke-Checked cmake @('--build', 'build/cmake-build-release', '--config', 'Release', '--target', 'zto', '--parallel')

    # The API is a header-only SDK. Configuration, clean installation and ZIP creation
    # are necessary only when its inputs or installed contents changed.
    $versionText=(Get-Content -LiteralPath (Join-Path $projectRoot 'VERSION') -Raw).Trim()
    $versionMatch=[regex]::Match($versionText,'^0\.([0-9]+)\.[0-9]+(-[0-9A-Za-z.-]+)?$')
    if(!$versionMatch.Success){throw 'VERSION must be 0.<core>.<patch>[-qualifier]'}
    $apiVersion="0.$($versionMatch.Groups[1].Value)$($versionMatch.Groups[2].Value)"
    $apiSdk = Resolve-PackageDirectory $projectRoot "build/dist/Z2-api-v$apiVersion"
    $apiFingerprint = $apiVersion + ':' + (Get-DirectoryFingerprint (Join-Path $projectRoot 'src/client/api')) + ':' + (Get-DirectoryFingerprint (Join-Path $projectRoot 'src/shared/include')) + ':' + (Get-DirectoryFingerprint (Join-Path $projectRoot 'src/shared/package')) + ':' + (Get-FileHash -LiteralPath (Join-Path $projectRoot 'src/shared/CMakeLists.txt') -Algorithm SHA256).Hash
    foreach ($configuration in @('Release','Debug')) {
        $contract=Get-Content -LiteralPath (Join-Path $projectRoot "build/cmake-build-release/src/client/api/host-runtime-contract/$configuration.json") -Raw | ConvertFrom-Json
        $apiFingerprint+=':'+$contract.qtBuildId
    }
    $apiStatePath = Join-Path $projectRoot 'build/cmake-build-api/package-state.json'
    $apiState = $null
    if (Test-Path -LiteralPath $apiStatePath) {
        try { $apiState = Get-Content -LiteralPath $apiStatePath -Raw | ConvertFrom-Json } catch { $apiState = $null }
    }
    $sdkFingerprint = if (Test-Path -LiteralPath $apiSdk) { Get-DirectoryFingerprint $apiSdk } else { '' }
    $apiChanged = $RebuildApi -or !$apiState -or $apiState.inputs -ne $apiFingerprint -or $apiState.sdk -ne $sdkFingerprint
    if ($apiChanged) {
        Protect-SourceBuildCache 'build/cmake-build-api' 'src/client/api'
        Invoke-Checked cmake (@('-S', 'src/client/api', '-B', 'build/cmake-build-api', '-G', 'Visual Studio 17 2022', '-A', 'x64', "-DCMAKE_PREFIX_PATH=$qtSdk") + $sodiumConfigure)
        Invoke-Checked cmake @('--build', 'build/cmake-build-api', '--config', 'Release', '--target', 'ztplugin_pack', '--parallel')
        $apiStaging = New-PackageStaging $apiSdk
        Invoke-Checked cmake @('--install', 'build/cmake-build-api', '--config', 'Release', '--prefix', $apiStaging)
        Publish-PackageDirectory $apiStaging $apiSdk
        $apiStaging = $null
        $sdkFingerprint = Get-DirectoryFingerprint $apiSdk
    } else { Write-Host 'API inputs and installed SDK unchanged; reusing existing SDK' }
    $apiArchive = $apiSdk + '.zip'
    $archiveChanged = $apiChanged -or !(Test-Path -LiteralPath $apiArchive)
    if (!$archiveChanged) { $archiveChanged = $apiState.archive -ne (Get-FileHash -LiteralPath $apiArchive -Algorithm SHA256).Hash }
    if ($archiveChanged) { Write-PackageArchive $apiSdk }
    else { Write-Host "API ZIP unchanged; reusing $apiArchive" }
    @{inputs=$apiFingerprint; sdk=$sdkFingerprint; archive=(Get-FileHash -LiteralPath $apiArchive -Algorithm SHA256).Hash} |
        ConvertTo-Json | Set-Content -LiteralPath $apiStatePath -Encoding UTF8

    # Only an explicit request builds/publishes the voice plugin.
    if ($BuildMediaPlugin) {
        & (Join-Path $PSScriptRoot 'build-media-windows.ps1') -QtRoot $qtSdk -ApiSdk $apiSdk -ProjectDirectory $projectRoot -OutputDirectory $pluginOutputPath
    }
    if ($BuildVoicePlugin) {
        $mediaApiSdk=Join-Path $projectRoot 'build/dist/Z2-media-api-v2'
        if(!(Test-Path -LiteralPath $mediaApiSdk)) {throw 'Install the media API SDK first, or specify -BuildMediaPlugin together with -BuildVoicePlugin'}
        & (Join-Path $PSScriptRoot 'build-plugin-windows.ps1') -PluginDirectory 'src/plugins/voice-chat/client' -Target zto_voice_chat -QtRoot $qtSdk -ApiSdk $apiSdk -AdditionalSdk $mediaApiSdk -ProjectDirectory $projectRoot -OutputDirectory $pluginOutputPath
    } else { Write-Host 'Plugin compilation skipped (not requested)' }

    $stagingPath = New-PackageStaging $outputPath
    Copy-Item -LiteralPath 'build/cmake-build-release/Release/libsodium.dll' -Destination $stagingPath
    New-Item -ItemType Directory -Path (Join-Path $stagingPath 'licenses') -Force | Out-Null
    Copy-Item -LiteralPath 'src/shared/package/libsodium-LICENSE.txt' -Destination (Join-Path $stagingPath 'licenses')
    Copy-Item -LiteralPath 'build/cmake-build-release/Release/Z2.exe' -Destination $stagingPath
    $deployArguments = @('--release', '--translations', 'en,zh_CN,zh_TW', '--skip-plugin-types', 'qmltooling', '--qmldir', (Join-Path $projectRoot 'src/client/main/qml'), '--dir', $stagingPath, (Join-Path $stagingPath 'Z2.exe'))
    # Qt Quick uses D3D11 by default on Windows. Its CPU scene graph backend
    # does not need the separate Mesa OpenGL software renderer.
    if (!$IncludeOpenGLSoftware) { $deployArguments += '--no-opengl-sw' }
    Invoke-Checked $deployQt $deployArguments
    $forbidden=Get-ChildItem -LiteralPath $stagingPath -File -Recurse | Where-Object {
        $_.Name -match '^(Qt6Multimedia|avcodec-|avformat-|avutil-|swresample-|swscale-)' -or
        $_.Extension -eq '.z2plugin' -or $_.Directory.Name -eq 'multimedia'
    }
    if ($forbidden) { throw 'Application deployment contains plugin media dependencies' }
    if (!$KeepAllQmlStyles) {
        # Generic Controls remains available to plugins and selects the host's
        # Basic style. Trim only known unused styles in the owned staging tree.
        $stagingRoot = [IO.Path]::GetFullPath($stagingPath).TrimEnd('\','/')
        foreach ($style in @('Fusion','Imagine','Material','Universal','FluentWinUI3','Windows','macOS','iOS')) {
            foreach ($relative in @("qml/QtQuick/Controls/$style", "qml/QtQuick/Dialogs/quickimpl/qml/+$style",
                                     "Qt6QuickControls2$style.dll", "Qt6QuickControls2${style}StyleImpl.dll")) {
                $stylePath = [IO.Path]::GetFullPath((Join-Path $stagingRoot $relative))
                if (!$stylePath.StartsWith($stagingRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Style cleanup escaped staging directory' }
                if (Test-Path -LiteralPath $stylePath) {
                    if ((Get-Item -LiteralPath $stylePath -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Style cleanup refuses linked paths' }
                    Remove-Item -LiteralPath $stylePath -Recurse -Force
                }
            }
        }
    }
    $vsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vsInstall = (& $vsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
    if (!$vsInstall) { throw 'Cannot locate MSVC runtime for deployment' }
    $redistVersion = Get-ChildItem -LiteralPath (Join-Path $vsInstall 'VC/Redist/MSVC') -Directory |
        Where-Object Name -Match '^[0-9]+\.[0-9]+' | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    $crtDirectory = Get-ChildItem -LiteralPath (Join-Path $redistVersion.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT' | Select-Object -First 1
    if (!$crtDirectory) { throw 'Cannot locate x64 release MSVC CRT' }
    Get-ChildItem -LiteralPath $crtDirectory.FullName -Filter '*.dll' | Copy-Item -Destination $stagingPath
    Publish-PackageDirectory $stagingPath $outputPath
    $stagingPath = $null
    Write-PackageArchive $outputPath
} catch {
    foreach ($incomplete in @($stagingPath, $apiStaging)) {
        if ($incomplete -and (Test-Path -LiteralPath $incomplete)) { Write-Warning "Incomplete staging preserved at $incomplete" }
    }
    throw
} finally { Pop-Location }

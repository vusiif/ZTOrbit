param(
    [Parameter(Mandatory=$true)][string]$QtRoot,
    [string]$ProjectDirectory = '.',
    [string]$OutputDirectory = 'dist/windows-x64',
    [string]$PluginOutputDirectory = 'dist/plugins',
    [switch]$RebuildApi,
    [switch]$BuildVoicePlugin
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PackageTools.ps1')
$projectRoot = (Resolve-Path -LiteralPath $ProjectDirectory).Path
if (!(Test-Path -LiteralPath (Join-Path $projectRoot 'src/client/main/CMakeLists.txt')) -or
    !(Test-Path -LiteralPath (Join-Path $projectRoot 'src/client/api/CMakeLists.txt'))) { throw 'ProjectDirectory must be the ZTGUI repository root' }
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
    # Build this target only, even if this build tree also has src/tests/plugins configured.
    Invoke-Checked cmake @('-S', '.', '-B', 'cmake-build-release', '-G', 'Visual Studio 17 2022', '-A', 'x64', "-DCMAKE_PREFIX_PATH=$qtSdk")
    Invoke-Checked cmake @('--build', 'cmake-build-release', '--config', 'Release', '--target', 'zerotier_gui', '--parallel')

    # The API is a header-only SDK. Configuration, clean installation and ZIP creation
    # are necessary only when its inputs or installed contents changed.
    $apiVersion = ((Get-Content VERSION -Raw).Trim().Split('.')[0..1] -join '.')
    $apiSdk = Resolve-PackageDirectory $projectRoot "dist/ZTGUI-api-v$apiVersion"
    $apiFingerprint = $apiVersion + ':' + (Get-DirectoryFingerprint (Join-Path $projectRoot 'src/client/api')) + ':' + (Get-DirectoryFingerprint (Join-Path $projectRoot 'src/shared/include'))
    $apiStatePath = Join-Path $projectRoot 'cmake-build-api/package-state.json'
    $apiState = $null
    if (Test-Path -LiteralPath $apiStatePath) {
        try { $apiState = Get-Content -LiteralPath $apiStatePath -Raw | ConvertFrom-Json } catch { $apiState = $null }
    }
    $sdkFingerprint = if (Test-Path -LiteralPath $apiSdk) { Get-DirectoryFingerprint $apiSdk } else { '' }
    $apiChanged = $RebuildApi -or !$apiState -or $apiState.inputs -ne $apiFingerprint -or $apiState.sdk -ne $sdkFingerprint
    if ($apiChanged) {
        Protect-SourceBuildCache 'cmake-build-api' 'src/client/api'
        Invoke-Checked cmake @('-S', 'src/client/api', '-B', 'cmake-build-api', '-G', 'Visual Studio 17 2022', '-A', 'x64', "-DCMAKE_PREFIX_PATH=$qtSdk")
        $apiStaging = New-PackageStaging $apiSdk
        Invoke-Checked cmake @('--install', 'cmake-build-api', '--prefix', $apiStaging)
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
    $voiceLibrary = Join-Path $pluginOutputPath 'ztgui_voice_chat.dll'
    if ($BuildVoicePlugin) {
        Protect-SourceBuildCache 'cmake-build-voice-client' 'src/client/plugins/voice-chat/client-plugin'
        Invoke-Checked cmake @('-S', 'src/client/plugins/voice-chat/client-plugin', '-B', 'cmake-build-voice-client', '-G', 'Visual Studio 17 2022', '-A', 'x64', "-DCMAKE_PREFIX_PATH=$apiSdk;$qtSdk")
        Invoke-Checked cmake @('--build', 'cmake-build-voice-client', '--config', 'Release', '--target', 'ztgui_voice_chat', '--parallel')
        New-Item -ItemType Directory -Path $pluginOutputPath -Force | Out-Null
        Copy-Item -LiteralPath 'cmake-build-voice-client/Release/ztgui_voice_chat.dll' -Destination $voiceLibrary
        $opusSource = Join-Path $projectRoot 'cmake-build-voice-client/_deps/opus-src'
        $override = Get-Content -LiteralPath 'cmake-build-voice-client/CMakeCache.txt' |
            Where-Object { $_ -match '^FETCHCONTENT_SOURCE_DIR_OPUS:[^=]+=' } | Select-Object -First 1
        if ($override -and $override.Split('=', 2)[1]) { $opusSource = $override.Split('=', 2)[1] }
        Copy-Item -LiteralPath (Join-Path $opusSource 'COPYING') -Destination (Join-Path $pluginOutputPath 'ztgui_voice_chat-LICENSE.txt')
        Write-Host "Created $voiceLibrary"
    } else { Write-Host 'Plugin compilation skipped (not requested)' }

    $stagingPath = New-PackageStaging $outputPath
    Copy-Item -LiteralPath 'cmake-build-release/Release/ZTGUI.exe' -Destination $stagingPath
    $deployArguments = @('--release', '--dir', $stagingPath, (Join-Path $stagingPath 'ZTGUI.exe'))
    # An existing DLL can supply dependency information without being rebuilt or modified.
    if (Test-Path -LiteralPath $voiceLibrary) { $deployArguments += $voiceLibrary }
    Invoke-Checked $deployQt $deployArguments
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

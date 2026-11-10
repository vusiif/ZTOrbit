param(
    [string]$ProjectDirectory = '.',
    [string]$ApplicationDirectory = 'build/dist/Z2-windows-x64',
    [string]$OutputDirectory = 'build/dist',
    [string]$InnoSetupCompiler = ''
)
# Wraps the verified application directory from build-windows.ps1 into an Inno
# Setup installer: build/dist/Z2-setup-<VERSION>-windows-x64.exe.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PackageTools.ps1')
$projectRoot = (Resolve-Path -LiteralPath $ProjectDirectory).Path
$applicationPath = Resolve-PackageDirectory $projectRoot $ApplicationDirectory
$outputPath = Resolve-PackageDirectory $projectRoot $OutputDirectory
foreach ($required in @('Z2.exe', 'z2_updater.exe', 'libsodium.dll')) {
    if (!(Test-Path -LiteralPath (Join-Path $applicationPath $required))) { throw "$required missing; run build-windows.ps1 first" }
}
$version = (Get-Content -LiteralPath (Join-Path $projectRoot 'VERSION') -Raw).Trim()
$match = [regex]::Match($version, '^([0-9]+\.[0-9]+\.[0-9]+)(-[0-9A-Za-z.-]+)?$')
if (!$match.Success) { throw 'VERSION must be <major>.<minor>.<patch>[-qualifier]' }
# The packaged Z2.exe must be the build of this VERSION.
$exeVersion = (Get-Item -LiteralPath (Join-Path $applicationPath 'Z2.exe')).VersionInfo.ProductVersion
if ($exeVersion -ne $version) { throw "Z2.exe reports $exeVersion but VERSION is $version; rebuild the application" }
if (!$InnoSetupCompiler) {
    $InnoSetupCompiler = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6/ISCC.exe'),
        (Join-Path $env:ProgramFiles 'Inno Setup 6/ISCC.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs/Inno Setup 6/ISCC.exe')
    ) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (!$InnoSetupCompiler -or !(Test-Path -LiteralPath $InnoSetupCompiler)) { throw 'Inno Setup 6 (ISCC.exe) not found; pass -InnoSetupCompiler' }
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
$installer = Join-Path $outputPath "Z2-setup-$version-windows-x64.exe"
Invoke-Checked $InnoSetupCompiler @('/Qp',
    "/DAppVersion=$version", "/DFileVersion=$($match.Groups[1].Value)",
    "/DSourceDir=$applicationPath", "/DOutputDir=$outputPath",
    ('/DIconFile=' + (Join-Path $projectRoot 'src/client/assets/app-icon.ico')),
    (Join-Path $PSScriptRoot 'z2-setup.iss'))
if (!(Test-Path -LiteralPath $installer)) { throw "Installer was not produced: $installer" }
$item = Get-Item -LiteralPath $installer
Write-Host ("Installer: {0} ({1:N1} MB, SHA-256 {2})" -f $item.FullName, ($item.Length / 1MB), (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash)

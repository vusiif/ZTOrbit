param(
    [Parameter(Mandatory=$true)][string]$QtRoot,
    [string]$SodiumRoot = '',
    [string]$OutputDirectory = 'build/dist/windows-x64',
    [string]$PluginOutputDirectory = 'build/dist/plugins',
    [switch]$RebuildApi,
    [switch]$BuildVoicePlugin,
    [switch]$BuildMediaPlugin,
    [switch]$IncludeOpenGLSoftware,
    [switch]$KeepAllQmlStyles
)
# Compatibility entry point. Mirrors the build skill's Windows switches;
# build/packaging implementation lives with the build skill.
$projectRoot = Split-Path $PSScriptRoot -Parent
& (Join-Path $projectRoot '.agents/skills/ztgui-build/scripts/build-windows.ps1') @PSBoundParameters -ProjectDirectory $projectRoot

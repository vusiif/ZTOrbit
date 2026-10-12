param(
    [Parameter(Mandatory=$true)][string]$QtRoot,
    [string]$OutputDirectory = 'dist/windows-x64',
    [string]$PluginOutputDirectory = 'dist/plugins',
    [switch]$RebuildApi,
    [switch]$BuildVoicePlugin
)
# Compatibility entry point. Build/packaging implementation lives with the build skill.
$projectRoot = Split-Path $PSScriptRoot -Parent
& (Join-Path $projectRoot '.agent/skills/ztgui-build/scripts/build-windows.ps1') @PSBoundParameters -ProjectDirectory $projectRoot

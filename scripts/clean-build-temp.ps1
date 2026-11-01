<#
.SYNOPSIS
Removes disposable build backups and clears the repository .tmp directory.

.DESCRIPTION
Compatibility entry point for the build skill's cleanup implementation. It
deletes directories named like package-replacement backups (*.previous-*) and
empties .tmp while keeping requested exclusions. The .tmp directory itself is
preserved. Defaults to the repository containing this script.

.PARAMETER ProjectDirectory
ZTOrbit repository root. Defaults to the repository containing this script.

.PARAMETER Exclude
Paths below .tmp to keep, relative to .tmp. A leading .tmp/ prefix is
accepted. A folder keeps its whole subtree. * and ? wildcards are supported.

.EXAMPLE
./scripts/clean-build-temp.ps1 -WhatIf

.EXAMPLE
./scripts/clean-build-temp.ps1 -Exclude relay-deps,screenshots/keep-*.png
#>
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param(
    [string]$ProjectDirectory = '.',
    [string[]]$Exclude = @()
)
# Compatibility entry point. Cleanup implementation lives with the build skill.
$projectRoot = Split-Path $PSScriptRoot -Parent
$arguments = @{
    ProjectDirectory = $ProjectDirectory
    Exclude = $Exclude
}
if (!$PSBoundParameters.ContainsKey('ProjectDirectory')) { $arguments['ProjectDirectory'] = $projectRoot }
if ($WhatIfPreference) { $arguments['WhatIf'] = $true }
& (Join-Path $projectRoot '.agents/skills/zto-build/scripts/clean-build-temp.ps1') @arguments

<#
.SYNOPSIS
Cleans disposable ZTGUI build leftovers.

.DESCRIPTION
Removes directories whose names look like package-replacement backups
(*.previous-*), then empties the repository .tmp directory while keeping
requested exclusions. The .tmp directory itself is preserved. Symlinks and
other reparse points are neither followed nor removed.

.PARAMETER ProjectDirectory
ZTGUI repository root. Defaults to the current directory.

.PARAMETER Exclude
Paths below .tmp to keep, relative to .tmp. A folder keeps its whole subtree.
A leading .tmp/ prefix is accepted. * and ? wildcards are supported and may
span path separators. Absolute paths inside .tmp are also accepted but must
not contain wildcards.

.EXAMPLE
./scripts/clean-build-temp.ps1

.EXAMPLE
./scripts/clean-build-temp.ps1 -Exclude relay-deps,screenshots/keep-*.png

.EXAMPLE
./scripts/clean-build-temp.ps1 -WhatIf
#>
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param(
    [string]$ProjectDirectory = '.',
    [string[]]$Exclude = @()
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PackageTools.ps1')

$projectRoot = (Resolve-Path -LiteralPath $ProjectDirectory).Path
if (!(Test-Path -LiteralPath (Join-Path $projectRoot 'src/client/main/CMakeLists.txt')) -or
    !(Test-Path -LiteralPath (Join-Path $projectRoot 'src/client/api/CMakeLists.txt'))) {
    throw 'ProjectDirectory must be the ZTGUI repository root'
}
$tempPath = Resolve-PackageDirectory $projectRoot '.tmp'
$pathSeparator = [IO.Path]::DirectorySeparatorChar
if (!$tempPath.StartsWith($projectRoot + $pathSeparator, [StringComparison]::OrdinalIgnoreCase)) {
    throw '.tmp must be inside the project root'
}
$tempRelative = $tempPath.Substring($projectRoot.Length + 1).Replace('\', '/')

$script:removedBackups = 0
$script:removedFiles = 0
$script:removedDirectories = 0
$script:kept = 0
$script:failures = New-Object 'System.Collections.Generic.List[string]'

function Add-CleanupFailure([string]$Path, [string]$Message) {
    $script:failures.Add("$Path`: $Message")
}

function Test-LinkedItem($Item) {
    return (($Item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
}

function Remove-CleanupItem {
    param(
        [Parameter(Mandatory = $true)]$Cmdlet,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Description,
        [Parameter(Mandatory = $true)][ValidateSet('backup', 'file', 'directory')][string]$Kind,
        [switch]$Recurse
    )
    if (!$Cmdlet.ShouldProcess($Path, "Remove $Description")) { return }
    try {
        if ($Recurse) { Remove-Item -LiteralPath $Path -Recurse -Force }
        else { Remove-Item -LiteralPath $Path -Force }
        if ($Kind -eq 'backup') {
            $script:removedBackups++
            Write-Host "Removed backup: $Path"
        } elseif ($Kind -eq 'file') {
            $script:removedFiles++
            Write-Verbose "Removed file: $Path"
        } else {
            $script:removedDirectories++
            Write-Verbose "Removed empty directory: $Path"
        }
    } catch {
        Add-CleanupFailure $Path $_.Exception.Message
    }
}

function Test-TreeHasLink {
    param([Parameter(Mandatory = $true)][string]$Path)
    $pending = New-Object 'System.Collections.Generic.Stack[string]'
    $pending.Push($Path)
    while ($pending.Count -gt 0) {
        $current = $pending.Pop()
        $children = @(Get-ChildItem -LiteralPath $current -Force -ErrorAction Stop)
        foreach ($child in $children) {
            if (Test-LinkedItem $child) { return $true }
            if ($child -is [IO.DirectoryInfo]) { $pending.Push($child.FullName) }
        }
    }
    return $false
}

function Get-PreviousBackups {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [string]$SkipPath = ''
    )
    $backups = New-Object 'System.Collections.Generic.List[string]'
    $pending = New-Object 'System.Collections.Generic.Stack[string]'
    $pending.Push($Root)
    while ($pending.Count -gt 0) {
        $current = $pending.Pop()
        try {
            $children = @(Get-ChildItem -LiteralPath $current -Directory -Force -ErrorAction Stop)
        } catch {
            Add-CleanupFailure $current $_.Exception.Message
            continue
        }
        foreach ($child in $children) {
            if ($child.Name -eq '.git' -or $child.Name -eq 'node_modules') { continue }
            if ($SkipPath -and ($child.FullName.Equals($SkipPath, [StringComparison]::OrdinalIgnoreCase) -or
                $child.FullName.StartsWith($SkipPath + $pathSeparator, [StringComparison]::OrdinalIgnoreCase))) { continue }
            if ($child.Name -like '*.previous-*') {
                if (Test-LinkedItem $child) {
                    Write-Warning "Skipping linked backup directory: $($child.FullName)"
                    $script:kept++
                } else {
                    $backups.Add($child.FullName)
                }
                continue
            }
            if (!(Test-LinkedItem $child)) { $pending.Push($child.FullName) }
        }
    }
    return $backups.ToArray()
}

function ConvertTo-TempExcludePattern {
    param(
        [Parameter(Mandatory = $true)][string]$Pattern,
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$TempRelative
    )
    if ([string]::IsNullOrWhiteSpace($Pattern)) { throw 'Exclude entries must not be empty' }
    $value = $Pattern.Trim()
    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd([char[]]'\/')
    if ([IO.Path]::IsPathRooted($value)) {
        try {
            $absolute = [IO.Path]::GetFullPath($value).TrimEnd([char[]]'\/')
        } catch {
            throw "Absolute exclude paths must not contain wildcards; use a path relative to ${rootPath}: $value"
        }
        if (!$absolute.StartsWith($rootPath + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Excluded path must be inside ${rootPath}: $value"
        }
        $value = $absolute.Substring($rootPath.Length)
    }
    $value = $value.Replace('\', '/').Trim([char[]]'/')
    while ($value.StartsWith('./')) { $value = $value.Substring(2).TrimStart([char[]]'/') }
    # Tab completion from the repository root yields paths such as .tmp/cache.
    # Treat the temporary directory prefix as optional so both spellings work.
    if ($value.Equals($TempRelative, [StringComparison]::OrdinalIgnoreCase) -or
        $value.StartsWith($TempRelative + '/', [StringComparison]::OrdinalIgnoreCase)) {
        $value = $value.Substring($TempRelative.Length).TrimStart([char[]]'/')
    }
    if (!$value) { throw 'Exclude entries must name a file or folder below the temporary directory' }
    foreach ($segment in $value.Split('/')) {
        if (!$segment -or $segment -eq '.' -or $segment -eq '..') {
            throw "Exclude entries must be relative paths without empty or dot segments: $Pattern"
        }
    }
    return $value
}

function Test-ExcludedPath {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [string[]]$Patterns = @()
    )
    if (!$Patterns -or $Patterns.Count -eq 0) { return $false }
    $current = $RelativePath.Replace('\', '/').Trim([char[]]'/')
    while ($current) {
        foreach ($pattern in $Patterns) {
            if ($current -like $pattern) { return $true }
        }
        $index = $current.LastIndexOf('/')
        if ($index -lt 0) { break }
        $current = $current.Substring(0, $index)
    }
    return $false
}

function Clear-TempDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [string[]]$Patterns = @(),
        [Parameter(Mandatory = $true)]$Cmdlet
    )
    $files = New-Object 'System.Collections.Generic.List[string]'
    $directories = New-Object 'System.Collections.Generic.List[string]'
    $pending = New-Object 'System.Collections.Generic.Stack[string]'
    $pending.Push($Root)
    while ($pending.Count -gt 0) {
        $current = $pending.Pop()
        try {
            $children = @(Get-ChildItem -LiteralPath $current -Force -ErrorAction Stop)
        } catch {
            Add-CleanupFailure $current $_.Exception.Message
            continue
        }
        foreach ($child in $children) {
            $relative = $child.FullName.Substring($Root.Length + 1).Replace('\', '/')
            if (Test-LinkedItem $child) {
                Write-Warning "Skipping linked path: $($child.FullName)"
                $script:kept++
                continue
            }
            if (Test-ExcludedPath $relative $Patterns) {
                Write-Host "Kept excluded: $relative"
                $script:kept++
                continue
            }
            if ($child -is [IO.DirectoryInfo]) {
                $directories.Add($child.FullName)
                $pending.Push($child.FullName)
            } else {
                $files.Add($child.FullName)
            }
        }
    }
    foreach ($file in $files) {
        Remove-CleanupItem -Cmdlet $Cmdlet -Path $file -Description 'temporary file' -Kind file
    }
    for ($index = $directories.Count - 1; $index -ge 0; $index--) {
        $directory = $directories[$index]
        try {
            $empty = (@(Get-ChildItem -LiteralPath $directory -Force -ErrorAction Stop).Count -eq 0)
        } catch {
            Add-CleanupFailure $directory $_.Exception.Message
            continue
        }
        if ($empty) {
            Remove-CleanupItem -Cmdlet $Cmdlet -Path $directory -Description 'empty temporary directory' -Kind directory
        }
    }
}

$tempExclusions = @()
foreach ($entry in $Exclude) {
    $tempExclusions += ConvertTo-TempExcludePattern -Pattern $entry -Root $tempPath -TempRelative $tempRelative
}
$tempExclusions = @($tempExclusions | Sort-Object -Unique)

Write-Host "Cleaning build leftovers in $projectRoot"
if ($WhatIfPreference) { Write-Host 'Preview only (-WhatIf): no files will be removed' }
if ($tempExclusions.Count -gt 0) {
    Write-Host ('Keeping temporary paths: ' + ($tempExclusions -join ', '))
}

$backups = @(Get-PreviousBackups -Root $projectRoot -SkipPath $tempPath | Sort-Object)
if ($backups.Count -eq 0) {
    Write-Host 'No *.previous-* build backups found'
} else {
    foreach ($backup in $backups) {
        try {
            if (Test-TreeHasLink -Path $backup) {
                Write-Warning "Keeping backup with linked content: $backup"
                $script:kept++
                continue
            }
        } catch {
            Add-CleanupFailure $backup $_.Exception.Message
            continue
        }
        Remove-CleanupItem -Cmdlet $PSCmdlet -Path $backup -Description 'build backup' -Kind backup -Recurse
    }
}

if (Test-Path -LiteralPath $tempPath) {
    Write-Host "Clearing temporary directory: $tempPath"
    Clear-TempDirectory -Root $tempPath -Patterns $tempExclusions -Cmdlet $PSCmdlet
} else {
    Write-Host "Temporary directory not present: $tempPath"
}

Write-Host ("Summary: removed {0} backup(s), {1} file(s), {2} empty folder(s); kept {3} excluded or linked path(s)." -f `
    $script:removedBackups, $script:removedFiles, $script:removedDirectories, $script:kept)
if ($script:failures.Count -gt 0) {
    throw ("Cleanup failed for {0} path(s):`n{1}" -f $script:failures.Count, ($script:failures -join "`n"))
}

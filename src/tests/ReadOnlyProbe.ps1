param(
    [Parameter(Mandatory = $true)][string]$Probe
)

$ErrorActionPreference = 'Stop'
if (!(Test-Path -LiteralPath $Probe -PathType Leaf)) { throw "Probe executable not found: $Probe" }

$root = Join-Path ([IO.Path]::GetTempPath()) ('zto-readonly-' + [guid]::NewGuid().ToString('N'))
$shared = Join-Path $root 'application'
$personal = Join-Path $root 'user-data'
New-Item -ItemType Directory -Path $shared -Force | Out-Null

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
$elevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$rights = [Security.AccessControl.FileSystemRights]'CreateFiles,WriteData,AppendData'
$rule = New-Object Security.AccessControl.FileSystemAccessRule(
    $identity.Name, $rights, [Security.AccessControl.InheritanceFlags]'ContainerInherit,ObjectInherit',
    [Security.AccessControl.PropagationFlags]::None, [Security.AccessControl.AccessControlType]::Deny)
$acl = Get-Acl -LiteralPath $shared
$denyApplied = $false
try {
    $acl.AddAccessRule($rule)
    Set-Acl -LiteralPath $shared -AclObject $acl
    $denyApplied = $true

    $output = & $Probe --shared $shared --personal $personal 2>&1
    $exitCode = $LASTEXITCODE
    $json = ($output | Out-String).Trim()
    Write-Output ("elevated={0}" -f $elevated)
    Write-Output $json
    if ($exitCode -ne 0) { throw "Read-only probe exited with $exitCode" }
    $result = $json | ConvertFrom-Json
    if ($result.mode -ne 'personal') { throw "Expected personal fallback, got '$($result.mode)'" }
    if (!$result.prepared) { throw "Personal root preparation failed: $($result.error)" }
    $resolvedPersonal = [IO.Path]::GetFullPath($personal).TrimEnd('\')
    $resolvedActive = [IO.Path]::GetFullPath([string]$result.activeRoot).TrimEnd('\')
    if (!($resolvedActive.StartsWith($resolvedPersonal + '\', [StringComparison]::OrdinalIgnoreCase))) {
        throw "Active root '$resolvedActive' is not under the personal base '$resolvedPersonal'"
    }
    if (Test-Path -LiteralPath (Join-Path $shared 'settings.ini')) {
        throw 'The read-only shared root received settings.ini'
    }
    if ($elevated) {
        Write-Output 'Note: this run used an elevated account; record elevation when citing the result.'
    }
    Write-Output 'Windows native read-only fallback passed'
} finally {
    if ($denyApplied) {
        $restored = Get-Acl -LiteralPath $shared
        $restored.RemoveAccessRule($rule) | Out-Null
        Set-Acl -LiteralPath $shared -AclObject $restored
    }
    $resolved = [IO.Path]::GetFullPath($root)
    $parent = [IO.Path]::GetDirectoryName($resolved)
    if (![IO.Path]::GetFileName($resolved).StartsWith('zto-readonly-') -or
        $parent -ne [IO.Path]::GetTempPath().TrimEnd('\', '/')) {
        throw 'Unsafe test cleanup'
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

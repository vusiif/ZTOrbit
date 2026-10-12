function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed: $LASTEXITCODE" }
}

function Resolve-PackageDirectory([string]$ProjectRoot, [string]$Directory) {
    $requested = if ([IO.Path]::IsPathRooted($Directory)) { $Directory } else { Join-Path $ProjectRoot $Directory }
    $path = [IO.Path]::GetFullPath($requested).TrimEnd([char[]]'\/')
    $projectPath = [IO.Path]::GetFullPath($ProjectRoot).TrimEnd([char[]]'\/')
    if (!$path -or $path -eq [IO.Path]::GetPathRoot($requested).TrimEnd([char[]]'\/') -or
        $path -eq $projectPath -or $projectPath.StartsWith($path + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Package directory must not be a drive root, project root, or its ancestor'
    }
    if (Test-Path -LiteralPath $path) {
        $item = Get-Item -LiteralPath $path -Force
        if (!$item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Package directory must be a regular directory'
        }
    }
    return $path
}

function New-PackageStaging([string]$Destination) {
    $parent = Split-Path $Destination -Parent
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    $parent = (Resolve-Path -LiteralPath $parent).Path
    $staging = Join-Path $parent ('.' + (Split-Path $Destination -Leaf) + '.staging-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $staging | Out-Null
    return $staging
}

function Publish-PackageDirectory([string]$Staging, [string]$Destination) {
    $parent = (Resolve-Path -LiteralPath (Split-Path $Staging -Parent)).Path
    $name = Split-Path $Destination -Leaf
    $backup = Join-Path $parent ($name + '.previous-' + [guid]::NewGuid().ToString('N'))
    foreach ($path in @($Staging, $Destination, $backup)) {
        if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($path)) -ne $parent) {
            throw 'Package replacement path is outside the selected output parent'
        }
    }
    $hasBackup = $false
    if (Test-Path -LiteralPath $Destination) {
        $item = Get-Item -LiteralPath $Destination -Force
        if (!$item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Package destination changed during deployment'
        }
        Rename-Item -LiteralPath $Destination -NewName (Split-Path $backup -Leaf)
        $hasBackup = $true
    }
    try { Rename-Item -LiteralPath $Staging -NewName $name }
    catch {
        if ($hasBackup -and !(Test-Path -LiteralPath $Destination)) {
            Rename-Item -LiteralPath $backup -NewName $name
        }
        throw
    }
    Write-Host "Created $Destination"
    if ($hasBackup) { Write-Host "Previous output preserved at $backup" }
}

function Get-DirectoryFingerprint([string]$Directory) {
    $root = (Resolve-Path -LiteralPath $Directory).Path
    $records = @(Get-ChildItem -LiteralPath $root -File -Recurse -Force | Sort-Object FullName | ForEach-Object {
        $_.FullName.Substring($root.Length + 1).Replace('\', '/') + ':' + (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    })
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($records -join "`n"))).Replace('-', '') }
    finally { $hasher.Dispose() }
}

function Write-PackageArchive([string]$Directory) {
    $archive = $Directory + '.zip'
    if (Test-Path -LiteralPath $archive) {
        $item = Get-Item -LiteralPath $archive -Force
        if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Archive destination must be a regular file'
        }
    }
    $temporary = Join-Path (Split-Path $Directory -Parent) ('.' + (Split-Path $Directory -Leaf) + '.archive-' + [guid]::NewGuid().ToString('N') + '.zip')
    try {
        Compress-Archive -LiteralPath $Directory -DestinationPath $temporary -CompressionLevel Optimal
        Move-Item -LiteralPath $temporary -Destination $archive -Force
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
    }
    Write-Host "Created $archive"
}

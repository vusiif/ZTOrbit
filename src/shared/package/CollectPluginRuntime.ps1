param([Parameter(Mandatory=$true)][string]$Request,[switch]$Cleanup)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'RuntimeTools.ps1')
$r=Get-Content -LiteralPath $Request -Raw | ConvertFrom-Json
if ($Cleanup) {
    $manifest=Get-Content -LiteralPath ($r.output+'.bundle.json') -Raw | ConvertFrom-Json
    $parent=[IO.Path]::GetFullPath([IO.Path]::GetDirectoryName($r.output))
    $roots=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($file in $manifest.files) {
        $source=[IO.Path]::GetFullPath($file.source)
        if (!$source.StartsWith($parent+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {throw 'Bundle cleanup escaped output parent'}
        $leaf=$source.Substring($parent.Length+1).Split([IO.Path]::DirectorySeparatorChar)[0]
        $prefix=[IO.Path]::GetFileName($r.output)+'.bundle-'
        if (!$leaf.StartsWith($prefix) -or $leaf.Substring($prefix.Length) -notmatch '^[a-f0-9]{32}$') {throw 'Invalid owned bundle directory'}
        $null=$roots.Add((Join-Path $parent $leaf))
    }
    foreach ($root in $roots) {
        if ((Get-Item -LiteralPath $root -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {throw 'Refusing linked bundle cleanup'}
        Remove-Item -LiteralPath $root -Recurse -Force
    }
    return
}
$contract=Get-Content -LiteralPath $r.contract -Raw | ConvertFrom-Json
if ($contract.debug -ne ($r.configuration -eq 'Debug')) {throw 'SDK runtime contract configuration mismatch'}
$base=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($name in $contract.baseLibraries) {$null=$base.Add($name)}
$stage=Join-Path ([IO.Path]::GetDirectoryName($r.output)) ('.runtime-stage-'+[guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory -Path $stage
try {
    $files=[Collections.Generic.List[object]]::new()
    $files.Add(@{path='plugin/plugin.dll';role='entry';source=$r.entry})
    $private=[Collections.Generic.Dictionary[string,object]]::new([StringComparer]::OrdinalIgnoreCase)
    $explicit=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    if ($r.modules.Count) {
        # Recompute this Kit's contract using the SDK's host module set, before deployment patches any DLL.
        $verify=Join-Path $stage 'kit-contract.json'
        & (Join-Path $PSScriptRoot 'GenerateHostContract.ps1') -QtBin (Join-Path $r.qtRoot 'bin') -Modules ($contract.modules -join ',') -Output $verify -QtVersion $contract.qtVersion -Architecture $contract.architecture -CompilerAbi $contract.compilerAbi -Configuration $r.configuration -HostKind $contract.host
        $actual=Get-Content -LiteralPath $verify -Raw | ConvertFrom-Json
        if ($actual.qtBuildId -ne $contract.qtBuildId) {throw 'Qt Kit does not match SDK host runtime contract; update SDK/Kit'}
        $deploy=Join-Path $r.qtRoot 'bin/windeployqt.exe'
        $arguments=@('--dir',$stage,'--libdir',(Join-Path $stage 'bin'),'--plugindir',(Join-Path $stage 'qt-plugins'),'--no-translations','--no-quick-import','--no-compiler-runtime','--no-opengl-sw','--no-system-d3d-compiler','--no-system-dxc-compiler','--no-patchqt','--skip-plugin-types','qmltooling')
        $arguments+=if ($r.configuration -eq 'Debug') {'--debug'} else {'--release'}
        foreach ($module in $r.modules) {$arguments+='--'+$module.ToLowerInvariant()}
        $arguments+=$r.entry
        & $deploy @arguments
        if ($LASTEXITCODE -ne 0) {throw "windeployqt failed: $LASTEXITCODE"}
        # Include dynamically loaded backends belonging to requested modules only.
        $types=@($r.pluginTypes)
        foreach ($file in (Get-ChildItem -LiteralPath $stage -Recurse -File -Filter '*.dll')) {
            $relative=$file.FullName.Substring($stage.Length+1).Replace('\','/')
            if ($base.Contains($file.Name) -or (Test-SystemLibrary $file.Name)) {continue}
            if ($relative.StartsWith('qt-plugins/')) {
                if ($types -notcontains $relative.Split('/')[1]) {continue}
                $item=@{path='runtime/'+$relative;role='qt-plugin';source=$file.FullName}
            } elseif ($relative.StartsWith('bin/')) {
                $item=@{path='runtime/bin/'+$file.Name;role='runtime-library';source=$file.FullName}
            } else {continue}
            if ($private.ContainsKey($file.Name)) {throw "Duplicate runtime DLL: $($file.Name)"}
            $private.Add($file.Name,$item)
        }
    }
    foreach ($mapping in $r.runtime) {
        $parts=$mapping.Split('=',2)
        if ($parts.Count -ne 2) {throw 'Expected package/path=source runtime mapping'}
        Assert-RegularPath $parts[1]
        $leaf=[IO.Path]::GetFileName($parts[0])
        if ($base.Contains($leaf) -or (Test-SystemLibrary $leaf) -or $leaf -ieq 'libsodium.dll') {throw "Host/system library must not be bundled: $leaf"}
        if ($private.ContainsKey($leaf)) {throw "Duplicate runtime DLL: $leaf"}
        $private.Add($leaf,@{path=$parts[0];role=$(if ($parts[0].StartsWith('runtime/qt-plugins/')) {'qt-plugin'} else {'runtime-library'});source=$parts[1]})
        $null=$explicit.Add($leaf)
    }
    # Any new transitive Qt library copied by windeployqt but needed by retained backends is retained.
    $queue=[Collections.Generic.Queue[object]]::new()
    $queue.Enqueue(@{source=$r.entry})
    foreach ($name in $private.Keys) {
        if ($private[$name].role -eq 'qt-plugin' -or $explicit.Contains($name)) {$queue.Enqueue($private[$name])}
    }
    foreach ($module in $r.modules) {
        $suffix=$(if ($r.configuration -eq 'Debug') {'d'} else {''})
        $name="Qt6$module$suffix.dll"
        if ($private.ContainsKey($name)) {$queue.Enqueue($private[$name])}
    }
    $visited=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $required=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    while ($queue.Count) {
        $item=$queue.Dequeue()
        if (!$visited.Add($item.source)) {continue}
        foreach ($name in (Get-PeImports $item.source)) {
            if ($base.Contains($name)) {$null=$required.Add($name); continue}
            if (Test-SystemLibrary $name) {continue}
            if ($private.ContainsKey($name)) {$queue.Enqueue($private[$name]); continue}
            throw "Missing transitive import $name required by $($item.source); declare RUNTIME_FILES explicitly"
        }
    }
    foreach ($item in ($private.Values | Sort-Object {$_.path})) {if ($visited.Contains($item.source)) {$files.Add($item)}}
    $licenseMappings=@($r.licenses)
    if ($private.Count) {
        # Only accept licenses supplied with the actual Kit; never substitute a random system FFmpeg license.
        $licenseRoot=Join-Path $r.qtRoot 'licenses'
        if (!(Test-Path -LiteralPath $licenseRoot)) {throw "Actual Qt/FFmpeg Kit licenses missing: $licenseRoot; install verified license files into the Kit"}
        foreach ($file in (Get-ChildItem -LiteralPath $licenseRoot -File -Recurse)) {
            $licenseMappings+=('licenses/Qt/'+$file.FullName.Substring($licenseRoot.Length+1).Replace('\','/')+'='+$file.FullName)
        }
        if (!($licenseMappings | Where-Object {$_ -match '(?i)LGPL.*3|LGPL-3'})) {throw 'Qt LGPLv3 license missing'}
        if (($private.Keys | Where-Object {$_ -match '^(avcodec|avformat|avutil|swresample|swscale)'}) -and
            !($licenseMappings | Where-Object {$_ -match '(?i)ffmpeg'})) {throw 'Actual FFmpeg build license/attribution missing'}
        $ffmpeg=@($files | Where-Object {$_.path -match '/(avcodec|avformat|avutil|swresample|swscale)'})
        if ($ffmpeg.Count) {
            $provenance=Join-Path $licenseRoot 'FFmpeg/actual-build.json'
            if (!(Test-Path -LiteralPath $provenance)) {throw 'FFmpeg actual-build.json with binary hashes, version, configuration and license is required'}
            $record=Get-Content -LiteralPath $provenance -Raw | ConvertFrom-Json
            if (!$record.av_version_info -or !$record.avutil_configuration -or !$record.avutil_license) {throw 'Incomplete FFmpeg build provenance'}
            foreach ($item in $ffmpeg) {
                $name=[IO.Path]::GetFileName($item.source)
                if ($record.binaries.$name -ne (Get-RuntimeHash $item.source)) {throw "FFmpeg license provenance does not match binary $name"}
            }
        }
    }
    foreach ($mapping in $licenseMappings) {
        $parts=$mapping.Split('=',2)
        if ($parts.Count -ne 2) {throw 'Expected package/path=source license mapping'}
        Assert-RegularPath $parts[1]
        $files.Add(@{path=$parts[0];role='license';source=$parts[1]})
    }
    $runtime=@{}
    if ($private.Count) {$runtime=@{qtBuildId=$contract.qtBuildId}}
    $manifest=@{files=@($files.ToArray());runtime=$runtime;requiresHost=@($required | Sort-Object)}
    # Publish collected files into a new durable bundle directory before handing them to the tool.
    $bundle=$r.output+'.bundle-'+[guid]::NewGuid().ToString('N')
    $null=New-Item -ItemType Directory -Path $bundle
    foreach ($item in $manifest.files) {
        $dest=Join-Path $bundle $item.path
        $null=New-Item -ItemType Directory -Path (Split-Path $dest -Parent) -Force
        Copy-Item -LiteralPath $item.source -Destination $dest
        $item.source=$dest
    }
    $manifest | ConvertTo-Json -Depth 15 | Set-Content -LiteralPath ($r.output+'.bundle.json') -Encoding UTF8
    $licenseOutput=Join-Path ([IO.Path]::GetDirectoryName($r.output)) (([IO.Path]::GetFileNameWithoutExtension($r.output))+'-licenses')
    $null=New-Item -ItemType Directory -Path $licenseOutput -Force
    foreach ($item in $manifest.files) {if ($item.role -eq 'license') {
        $dest=Join-Path $licenseOutput $item.path.Substring(9)
        $null=New-Item -ItemType Directory -Path (Split-Path $dest -Parent) -Force
        Copy-Item -LiteralPath $item.source -Destination $dest -Force
    }}
} finally {
    $resolved=[IO.Path]::GetFullPath($stage)
    if ($resolved -ne $stage -or !([IO.Path]::GetFileName($resolved).StartsWith('.runtime-stage-'))) {throw 'Unsafe staging cleanup'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

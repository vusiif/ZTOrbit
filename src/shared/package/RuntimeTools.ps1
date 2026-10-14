function Get-RuntimeHash([string]$Path) {
    $stream=[IO.File]::OpenRead($Path); $hash=[Security.Cryptography.SHA256]::Create()
    try {return [BitConverter]::ToString($hash.ComputeHash($stream)).Replace('-','').ToLowerInvariant()}
    finally {$stream.Dispose(); $hash.Dispose()}
}
function Get-PeImports([string]$Path) {
    $data=[IO.File]::ReadAllBytes($Path)
    function U16([int]$at) { if ($at -lt 0 -or $at+2 -gt $data.Length) {throw "Invalid PE: $Path"}; [BitConverter]::ToUInt16($data,$at) }
    function U32([int]$at) { if ($at -lt 0 -or $at+4 -gt $data.Length) {throw "Invalid PE: $Path"}; [BitConverter]::ToUInt32($data,$at) }
    if ((U16 0) -ne 0x5a4d) {throw "Invalid PE: $Path"}
    $pe=U32 60
    if ((U32 $pe) -ne 0x4550) {throw "Invalid PE: $Path"}
    $count=U16 ($pe+6); $opt=$pe+24; $table=$opt+(U16 ($pe+20)); $magic=U16 $opt
    if ($magic -ne 0x20b -and $magic -ne 0x10b) {throw "Invalid PE: $Path"}
    $dirs=$opt+$(if ($magic -eq 0x20b) {112} else {96})
    function Rva([uint32]$value) {
        for ($i=0;$i -lt $count;$i++) {
            $s=$table+$i*40; $va=U32 ($s+12); $len=U32 ($s+16); $raw=U32 ($s+20)
            if ($value -ge $va -and [long]$value-$va -lt $len) {return [int]([long]$raw+$value-$va)}
        }
        throw "Invalid PE RVA: $Path"
    }
    $imports=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($directory in @(1,13)) {
        $rva=U32 ($dirs+$directory*8); $length=U32 ($dirs+$directory*8+4)
        if (!$rva) {continue}
        $at=Rva $rva; $stride=$(if ($directory -eq 1) {20} else {32}); $terminated=$false
        for ($used=0;$used+$stride -le $length;$used+=$stride) {
            $at=(Rva $rva)+$used
            $name=U32 ($at+$(if ($directory -eq 1) {12} else {4}))
            if (!$name) {$terminated=$true; break}
            if ($directory -eq 13 -and !((U32 $at) -band 1)) {throw "Unsupported delay-import VA: $Path"}
            $start=Rva $name; $end=$start
            while ($end -lt $data.Length -and $end-$start -lt 260 -and $data[$end]) {$end++}
            if ($end -eq $data.Length -or $end-$start -ge 260) {throw "Invalid PE import: $Path"}
            $null=$imports.Add([Text.Encoding]::ASCII.GetString($data,$start,$end-$start).ToLowerInvariant())
        }
        if (!$terminated) {throw "Invalid PE imports: $Path"}
    }
    return @($imports)
}
function Test-SystemLibrary([string]$Name) {
    return $Name.StartsWith('api-ms-win-') -or $Name.StartsWith('ext-ms-win-') -or
        (Test-Path -LiteralPath (Join-Path ([Environment]::SystemDirectory) $Name)) -or
        $Name -match '^(vcruntime[0-9_]*|msvcp[0-9_]*|concrt[0-9_]*|ucrtbase)\.dll$'
}
function Assert-RegularPath([string]$Path) {
    $node=Get-Item -LiteralPath $Path -Force
    if ($node.PSIsContainer) {throw "Expected ordinary file: $Path"}
    while ($node) {
        if ($node.Attributes -band [IO.FileAttributes]::ReparsePoint) {throw "Linked input path: $Path"}
        $parent=Split-Path $node.FullName -Parent
        if (!$parent -or $parent -eq $node.FullName) {break}
        $node=Get-Item -LiteralPath $parent -Force
    }
}

param([string]$QtBin,[string]$Modules,[string]$Output,[string]$QtVersion,[string]$Architecture,[string]$CompilerAbi,[string]$Configuration,[string]$HostKind)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'RuntimeTools.ps1')
$names=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$queue=[Collections.Generic.Queue[string]]::new()
$suffix=$(if ($Configuration -eq 'Debug') {'d'} else {''})
foreach ($module in $Modules.Split(',')) {$queue.Enqueue("Qt6$module$suffix.dll")}
while ($queue.Count) {
    $name=$queue.Dequeue()
    if (!$names.Add($name)) {continue}
    $path=Join-Path $QtBin $name
    Assert-RegularPath $path
    foreach ($dep in (Get-PeImports $path)) {if ($dep -match '^qt6.*\.dll$') {$queue.Enqueue($dep)}}
}
$records=@($names | Sort-Object | ForEach-Object {$_.ToLowerInvariant()+':'+(Get-RuntimeHash (Join-Path $QtBin $_))})
$sha=[Security.Cryptography.SHA256]::Create()
try {$id=[BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($records -join [char]10))).Replace('-','').ToLowerInvariant()} finally {$sha.Dispose()}
@{formatVersion=1;host=$HostKind;platform='windows';architecture=$Architecture;compilerAbi=$CompilerAbi;debug=($Configuration -eq 'Debug');qtVersion=$QtVersion;modules=@($Modules.Split(','));baseLibraries=@($names | Sort-Object | ForEach-Object {$_.ToLowerInvariant()});qtBuildId=$id} |
    ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $Output -Encoding UTF8

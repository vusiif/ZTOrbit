param([string]$ProjectDirectory='.',[string]$BuildDirectory='build/cmake-build-media-dependencies',[int]$Jobs=8)
$ErrorActionPreference='Stop'
$project=(Resolve-Path -LiteralPath $ProjectDirectory).Path
$build=[IO.Path]::GetFullPath((Join-Path $project $BuildDirectory))
if(!$build.StartsWith($project+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {throw 'Dependency build must be inside workspace'}
$null=New-Item -ItemType Directory -Path $build -Force
$sources=@(
 @{name='ffmpeg';file='ffmpeg.tar.xz';url='https://ffmpeg.org/releases/ffmpeg-7.1.5.tar.xz';hash='DE668509CAF9E35E3CD162473441FDB29538C6D96ED080292B3CF9E6FC5D558F'},
 @{name='openh264';file='openh264.tar.gz';url='https://github.com/cisco/openh264/archive/refs/tags/v2.6.0.tar.gz';hash='558544AD358283A7AB2930D69A9CEDDF913F4A51EE9BF1BFB9E377322AF81A69'},
 @{name='make';file='make.tar.zst';url='https://repo.msys2.org/msys/x86_64/make-4.4.1-3-x86_64.pkg.tar.zst';hash='AF0BDBA17F06FE037F0194069ADAA31A8FE45F1A11381501896AEA1FAE37BD5D'},
 @{name='pkgconf';file='pkgconf.tar.zst';url='https://repo.msys2.org/msys/x86_64/pkgconf-2.5.1-1-x86_64.pkg.tar.zst';hash='38EFD4928AC0CB06E9BD558BAA533FC923C0592115BF9BD44970250F3AE2CB7E'},
 @{name='nasm';file='nasm.zip';url='https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/win64/nasm-2.16.03-win64.zip';hash='3EE4782247BCB874378D02F7EAB4E294A84D3D15F3F6EE2DE2F47A46AA7226E6'}
)
$tools=Join-Path $build 'tools'; $null=New-Item -ItemType Directory -Path $tools -Force
foreach($s in $sources) {
 $archive=Join-Path $build $s.file
 if(!(Test-Path -LiteralPath $archive)) {Invoke-WebRequest $s.url -OutFile $archive -TimeoutSec 300}
 if((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $s.hash) {throw "Source hash mismatch: $($s.name)"}
 if($s.name -eq 'nasm') {if(!(Test-Path "$tools/nasm-2.16.03/nasm.exe")) {Expand-Archive -LiteralPath $archive -DestinationPath $tools}}
 elseif($s.name -in @('make','pkgconf')) {if(!(Test-Path "$tools/usr/bin/$($s.name).exe")) {& tar -xf $archive -C $tools; if($LASTEXITCODE) {throw 'Tool extraction failed'}}}
 elseif(!(Test-Path (Join-Path $build $(if($s.name -eq 'ffmpeg') {'ffmpeg-7.1.5'} else {'openh264-2.6.0'})))) {& tar -xf $archive -C $build; if($LASTEXITCODE) {throw 'Source extraction failed'}}
}
$gitRoot=Split-Path (Split-Path (Get-Command git).Source)
$bash=Join-Path $gitRoot 'usr/bin/bash.exe'; if(!(Test-Path $bash)) {throw 'Git for Windows bash is required'}
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=(& $vswhere -latest -property installationPath).Trim()
$dev=Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
& cmd /d /s /c "`"$dev`" -arch=x64 -host_arch=x64 >nul && set" | ForEach-Object {if($_ -match '^([^=]+)=(.*)$') {[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
if($LASTEXITCODE) {throw 'Cannot initialize MSVC'}
$env:PATH="$tools/usr/bin;$tools/nasm-2.16.03;$gitRoot/usr/bin;"+$env:PATH
function Posix([string]$p) { return ((& (Join-Path $gitRoot 'usr/bin/cygpath.exe') -u $p) -join '').Trim() }
$posixBuild=Posix $build
$prefix=Join-Path $build 'installed'; $posixPrefix=Posix $prefix
$env:PKG_CONFIG_PATH="$posixPrefix/lib/pkgconfig"
$env:VSLANG='1033'
$windowsPrefix=$prefix.Replace('\','/')
$script=@"
set -eu
cd '$posixBuild/openh264-2.6.0'
make OS=msvc ARCH=x86_64 BUILDTYPE=Release USE_ASM=Yes CFLAGS_OPT='-O2 -MD -FS' PREFIX='$posixPrefix' -j$Jobs install-shared > '$posixBuild/openh264-build.log' 2>&1
sed -i 's|^prefix=.*|prefix=$windowsPrefix|;s/-lopenh264 /-lopenh264_dll /' '$posixPrefix/lib/pkgconfig/openh264.pc'
cd '$posixBuild/ffmpeg-7.1.5'
if test ! -f ffbuild/config.mak; then
 ./configure --prefix='$posixPrefix' --toolchain=msvc --arch=x86_64 --enable-shared --disable-static --disable-programs --disable-doc --disable-debug --disable-autodetect --enable-network --disable-vulkan --enable-libopenh264 --pkg-config=pkgconf > '$posixBuild/ffmpeg-configure.log' 2>&1
fi
# rc.exe does not accept the localized UTF-8 compiler banner in generated config.h.
sed -i 's/^#define CC_IDENT .*/#define CC_IDENT "MSVC"/' config.h
make -j$Jobs > '$posixBuild/ffmpeg-build.log' 2>&1
make install >> '$posixBuild/ffmpeg-build.log' 2>&1
"@
$scriptPath=Join-Path $build 'build.sh'; [IO.File]::WriteAllText($scriptPath,$script.Replace("`r`n","`n"),[Text.UTF8Encoding]::new($false))
& $bash (Posix $scriptPath)
if($LASTEXITCODE) {throw "Media dependency build failed; see $build/*-build.log and ffmpeg-configure.log"}
$sources | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $prefix 'sources.json') -Encoding utf8
Copy-Item -LiteralPath "$build/ffmpeg-7.1.5/COPYING.LGPLv2.1","$build/ffmpeg-7.1.5/LICENSE.md","$build/openh264-2.6.0/LICENSE" -Destination $prefix
Copy-Item -Path "$prefix/bin/*.lib" -Destination "$prefix/lib/" -Force
& python (Join-Path $PSScriptRoot 'record-ffmpeg.py') $prefix
if($LASTEXITCODE) {throw 'Cannot record controlled FFmpeg build provenance'}
Write-Host "Media dependency SDK: $prefix"

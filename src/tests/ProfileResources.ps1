param(
    [Parameter(Mandatory=$true)][string]$RuntimeDirectory,
    [Parameter(Mandatory=$true)][string]$FakeCli,
    [Parameter(Mandatory=$true)][string]$OutputJson,
    [string]$VoicePlugin,
    [int]$Repeats = 2,
    [switch]$LowMemory
)
$ErrorActionPreference = 'Stop'
$taskRuntime = (Resolve-Path -LiteralPath $RuntimeDirectory).Path
Copy-Item -LiteralPath $FakeCli -Destination (Join-Path $taskRuntime 'zerotier-cli.exe') -Force
if (!(Test-Path -LiteralPath (Join-Path $taskRuntime 'Z2.exe'))) { throw 'RuntimeDirectory must contain Z2.exe' }
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class ZTOrbitResourceWindows {
    public delegate bool Visitor(IntPtr window, IntPtr state);
    [DllImport("user32.dll")] public static extern bool EnumWindows(Visitor callback, IntPtr state);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr window, StringBuilder text, int size);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam, uint flags, uint timeout, out UIntPtr result);
    public static IntPtr Find(int pid) {
        IntPtr found=IntPtr.Zero;
        EnumWindows((window,state)=>{
            uint owner; GetWindowThreadProcessId(window,out owner);
            var title=new StringBuilder(128); GetWindowText(window,title,title.Capacity);
            if (owner==pid && title.ToString()=="Z2") { found=window; return false; }
            return true;
        },IntPtr.Zero);
        return found;
    }
}
'@
$taskResults = [System.Collections.Generic.List[object]]::new()
$taskCases = @(@{Name='empty';Mode='empty';Voice=$false},@{Name='compact';Mode='compact-lists';Voice=$false},@{Name='many';Mode='';Voice=$false})
if ($VoicePlugin) { $taskCases += @{Name='compact_voice';Mode='compact-lists';Voice=$true} }
foreach ($taskCase in $taskCases) {
    $taskPluginDir = Join-Path $taskRuntime 'plugins'
    # The harness owns only this copied test DLL; never alter the real plugin directory.
    # Historical 0.3.2 flow: current hosts scan only *.z2plugin and voice audio runs
    # in the media worker. To reproduce current voice usage, package the .z2plugin
    # and media worker instead of this bare DLL copy.
    $taskPluginCopy = Join-Path $taskPluginDir 'z2_voice_chat.dll'
    if ($taskCase.Voice) {
        New-Item -ItemType Directory -Path $taskPluginDir -Force | Out-Null
        Copy-Item -LiteralPath $VoicePlugin -Destination $taskPluginCopy -Force
    } elseif (Test-Path -LiteralPath $taskPluginCopy) { Remove-Item -LiteralPath $taskPluginCopy }
    for ($taskRepeat=0; $taskRepeat -lt $Repeats; $taskRepeat++) {
        $taskInfo = [System.Diagnostics.ProcessStartInfo]::new()
        $taskInfo.FileName = Join-Path $taskRuntime 'Z2.exe'
        if ($LowMemory) { $taskInfo.Arguments = '--disable-gpu-acceleration' }
        $taskInfo.WorkingDirectory = $taskRuntime
        $taskInfo.UseShellExecute = $false
        $taskInfo.CreateNoWindow = $true
        $taskInfo.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
        $taskInfo.RedirectStandardError = $true
        $taskInfo.RedirectStandardOutput = $true
        # Read-only UI profiling: the adjacent simulated CLI handles all queries.
        $taskInfo.Environment['__COMPAT_LAYER']='RunAsInvoker'
        $taskInfo.Environment['ZT_TEST_MODE']=$taskCase.Mode
        $taskQueryLog=Join-Path $taskRuntime "profile-$($taskCase.Name)-$taskRepeat.queries.txt"
        if (Test-Path -LiteralPath $taskQueryLog) { Remove-Item -LiteralPath $taskQueryLog }
        $taskInfo.Environment['ZT_TEST_QUERY_LOG']=$taskQueryLog
        $taskInfo.Environment['QSG_INFO']='1'
        $taskInfo.Environment['PATH']=$taskRuntime+';'+(Join-Path $env:WINDIR 'System32')+';'+$env:WINDIR
        foreach ($taskName in @('QML_IMPORT_PATH','QML2_IMPORT_PATH','QT_PLUGIN_PATH','QT_QPA_PLATFORM_PLUGIN_PATH','QT_QUICK_BACKEND','QSG_RHI_BACKEND')) { $taskInfo.Environment.Remove($taskName) | Out-Null }
        $taskProcess=[System.Diagnostics.Process]::Start($taskInfo)
        $taskErrorTask=$taskProcess.StandardError.ReadToEndAsync()
        $taskOutputTask=$taskProcess.StandardOutput.ReadToEndAsync()
        try {
            $taskWindow=[IntPtr]::Zero
            for ($taskTry=0; $taskTry -lt 30 -and !$taskProcess.HasExited; $taskTry++) {
                $taskWindow=[ZTOrbitResourceWindows]::Find($taskProcess.Id)
                if ($taskWindow -ne [IntPtr]::Zero) { break }
                [void]$taskProcess.WaitForExit(100)
            }
            if ($taskProcess.HasExited -or $taskWindow -eq [IntPtr]::Zero) { throw 'Profile process did not create its own window' }
            # Expose without taking focus so the normal D3D scene graph renders.
            [void][ZTOrbitResourceWindows]::ShowWindow($taskWindow,4)
            if ($taskProcess.WaitForExit(6000)) { throw 'Profile process exited during startup' }
            if (!(Test-Path -LiteralPath $taskQueryLog) -or
                !(Select-String -LiteralPath $taskQueryLog -SimpleMatch 'listnetworks') -or
                !(Select-String -LiteralPath $taskQueryLog -SimpleMatch 'listpeers')) {
                throw 'Simulated queries were not verified; do not report this sample'
            }
            foreach ($taskStage in @('visible','tray')) {
                if ($taskStage -eq 'tray') {
                    $taskMessageResult=[UIntPtr]::Zero
                    [void][ZTOrbitResourceWindows]::SendMessageTimeout($taskWindow,0x112,[UIntPtr]0xf060,[IntPtr]1,2,2000,[ref]$taskMessageResult)
                    if ($taskProcess.WaitForExit(1500)) { throw 'Caption close exited instead of hiding' }
                }
                $taskProcess.Refresh()
                $taskResults.Add([pscustomobject]@{Case=$taskCase.Name;Repeat=$taskRepeat;Stage=$taskStage;LowMemory=[bool]$LowMemory;WorkingSet=$taskProcess.WorkingSet64;PrivateCommit=$taskProcess.PrivateMemorySize64;PeakWorkingSet=$taskProcess.PeakWorkingSet64;Modules=@($taskProcess.Modules | ForEach-Object ModuleName)})
            }
        } finally {
            if (!$taskProcess.HasExited -and $taskWindow -ne [IntPtr]::Zero) {
                $taskCloseResult=[UIntPtr]::Zero
                [void][ZTOrbitResourceWindows]::SendMessageTimeout($taskWindow,0x10,[UIntPtr]::Zero,[IntPtr]::Zero,2,2000,[ref]$taskCloseResult)
            }
            if (!$taskProcess.WaitForExit(3000)) { $taskProcess.Kill(); $taskProcess.WaitForExit() }
            $taskErrorTask.GetAwaiter().GetResult() | Set-Content -LiteralPath (Join-Path $taskRuntime "profile-$($taskCase.Name)-$taskRepeat.stderr.txt") -Encoding utf8
            $taskProcess.Dispose()
        }
        Write-Host "Profiled $($taskCase.Name) #$taskRepeat"
    }
}
$taskResults | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $OutputJson -Encoding utf8
$taskResults | Select-Object Case,Repeat,Stage,@{Name='WorkingSetMiB';Expression={[math]::Round($_.WorkingSet/1MB,2)}},@{Name='PrivateCommitMiB';Expression={[math]::Round($_.PrivateCommit/1MB,2)}} | Format-Table

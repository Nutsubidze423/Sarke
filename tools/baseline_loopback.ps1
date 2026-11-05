# SPDX-License-Identifier: Apache-2.0
#
# Loopback video baseline (roadmap P2.1, docs/PERF.md): the real kvm_host and kvm_client on 127.0.0.1,
# one content window at a time, kvm_client's --stats for each. Both sides run on this one PC, so the
# numbers include both PCs' CPU work on one CPU.
#   -Mode desk    desk mode: the window is moved onto the virtual display. -Codec jpeg: a JPEG per
#                 window (P2.1); h264: one H.264 stream of the whole virtual display (P2.7).
#   -Mode viewer  the window viewer: the window is sent the way Ctrl+Alt+Shift+W sends it (the
#                 "share <hwnd>" command on kvm_host's stdin, so an installed Sarke holding the hotkey
#                 doesn't matter), as -Codec jpeg or h264 (kvm_host --codec, roadmap P2.4c).
#
#   powershell -ExecutionPolicy Bypass -File tools\baseline_loopback.ps1 [-Seconds 30] [-Mode desk|viewer] [-Codec jpeg|h264]
#
# Needs: a build (cmake --build --preset x64-release), the Virtual Display Driver, Microsoft Edge, and
# hands off the PC for about 3 minutes: it opens Notepad and two Edge windows (a throwaway profile)
# and moves them onto the virtual display. Prints a docs/PERF.md row per content; the CSVs and logs
# stay in %TEMP%\sarke_baseline.
param([int]$Seconds = 30, [ValidateSet('desk', 'viewer')][string]$Mode = 'desk',
      [ValidateSet('jpeg', 'h264')][string]$Codec = 'jpeg')
$ErrorActionPreference = 'Continue'  # native programs log to stderr; each step is checked
$repo = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $repo 'build\x64-release'
$work = Join-Path $env:TEMP 'sarke_baseline'
# Programs a failed earlier run left behind (only this script's: their command line names $work).
function Stop-Ours {
  Get-CimInstance Win32_Process -Filter "Name='kvm_host.exe' OR Name='kvm_client.exe' OR Name='msedge.exe'" |
    Where-Object { $_.CommandLine -like "*$work*" } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}
Stop-Ours
Start-Sleep -Seconds 1
Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
if (Test-Path $work) { throw "can't clear $work" }
New-Item -ItemType Directory -Force $work | Out-Null

Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct MONITORINFOEX { public int cbSize; public RECT rcMonitor; public RECT rcWork; public uint dwFlags; [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string szDevice; }
  public delegate bool MonitorEnumProc(IntPtr h, IntPtr hdc, ref RECT r, IntPtr d);
  [DllImport("user32.dll")] public static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, MonitorEnumProc f, IntPtr d);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool GetMonitorInfoW(IntPtr h, ref MONITORINFOEX mi);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  public static List<string> Monitors() {
    var list = new List<string>();
    EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (IntPtr h, IntPtr hdc, ref RECT r, IntPtr d) => {
      var mi = new MONITORINFOEX(); mi.cbSize = Marshal.SizeOf(mi); GetMonitorInfoW(h, ref mi);
      list.Add(mi.szDevice + "|" + mi.rcMonitor.L + "|" + mi.rcMonitor.T + "|" + mi.rcMonitor.R + "|" + mi.rcMonitor.B + "|" + mi.dwFlags);
      return true; }, IntPtr.Zero);
    return list;
  }
  public static IntPtr WindowWithTitle(string part) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { if (!IsWindowVisible(h)) return true; var sb = new StringBuilder(512); GetWindowTextW(h, sb, 512);
      if (sb.ToString().Contains(part)) { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
}
"@
[W]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

function Free-Port { $l = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0); $l.Start(); $p = $l.LocalEndpoint.Port; $l.Stop(); $p }
function Device-Id($exe, $dir) {
  # --print-id prints the 64-hex device ID on a line of its own (stdout); the log goes to stderr.
  $out = & $exe --data-dir $dir --print-id 2>$null
  foreach ($line in $out) { $t = "$line".Trim(); if ($t -match '^[0-9a-f]{64}$') { return $t } }
  throw "no device id from $exe"
}
function Wait-Log($file, $text, $timeout = 30) {
  $end = (Get-Date).AddSeconds($timeout)
  while ((Get-Date) -lt $end) { if ((Test-Path $file) -and ((Get-Content $file -Raw) -match [regex]::Escape($text))) { return $true }; Start-Sleep -Milliseconds 300 }
  return $false
}

$c = Free-Port; $v = Free-Port; $f = Free-Port
$desk = if ($Mode -eq 'desk') { 'true' } else { 'false' }
$ports = "`"control_port`": $c, `"video_port`": $v, `"file_port`": $f, `"desk`": $desk"
Set-Content -Encoding ascii "$work\host.json" "{$ports}"
Set-Content -Encoding ascii "$work\client.json" "{$ports, `"host_ip`": `"127.0.0.1`"}"
$hostId = Device-Id "$bin\host_a\kvm_host.exe" "$work\host-data"
$clientId = Device-Id "$bin\client_b\kvm_client.exe" "$work\client-data"

# The content: Notepad with a long text, and two pages in Edge (a throwaway profile, app window).
Copy-Item (Join-Path $repo 'THIRD_PARTY_NOTICES.md') "$work\document.txt"
$scroll = @'
<!doctype html><title>baseline-scroll</title><body style="font:16px Segoe UI;margin:24px">
<script>
for (let i = 0; i < 400; i++) document.write('<p>Paragraph ' + i + ': ' + 'Sarke streams this page while it scrolls. '.repeat(6) + '</p>');
let y = 0; (function step() { y += 3; if (y > document.body.scrollHeight - innerHeight) y = 0; scrollTo(0, y); requestAnimationFrame(step); })();
</script>
'@
$motion = @'
<!doctype html><title>baseline-motion</title><body style="margin:0;background:#000">
<canvas id=c width=1280 height=720 style="width:100vw;height:100vh"></canvas>
<script>
const c = document.getElementById('c').getContext('2d'); let t = 0;
(function frame() { t += 1;
  for (let i = 0; i < 40; i++) { c.fillStyle = 'hsl(' + ((t * 3 + i * 9) % 360) + ',80%,50%)';
    c.fillRect((Math.sin(t / 30 + i) * 0.5 + 0.5) * 1180, (Math.cos(t / 40 + i * 1.3) * 0.5 + 0.5) * 620, 100, 100); }
  c.fillStyle = 'rgba(0,0,0,0.08)'; c.fillRect(0, 0, 1280, 720); requestAnimationFrame(frame); })();
</script>
'@
Set-Content -Encoding utf8 "$work\scroll.html" $scroll
Set-Content -Encoding utf8 "$work\motion.html" $motion
$edge = 'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'

$contents = @(
  @{ name = 'static document (Notepad)'; title = 'document.txt'; start = { Start-Process notepad.exe "$work\document.txt" -PassThru } },
  @{ name = 'browser scrolling (Edge)'; title = 'baseline-scroll'; start = { Start-Process $edge "--user-data-dir=$work\edge --no-first-run --app=file:///$($work -replace '\\','/')/scroll.html --window-size=1280,800" -PassThru } },
  @{ name = 'animation, 40 moving boxes (Edge canvas)'; title = 'baseline-motion'; start = { Start-Process $edge "--user-data-dir=$work\edge --no-first-run --app=file:///$($work -replace '\\','/')/motion.html --window-size=1280,760" -PassThru } }
)

# kvm_host detaches a virtual display left over from a host that was killed: start one, let it, end it.
function Detach-Leftover {
  $p = Start-Process "$bin\host_a\kvm_host.exe" -ArgumentList "--config `"$work\host.json`" --no-kvm --no-clipboard --no-discovery --data-dir `"$work\host-data`" --log `"$work\detach.log`"" -PassThru -WindowStyle Hidden
  Wait-Log "$work\detach.log" 'listening on ports' | Out-Null
  Start-Sleep -Seconds 3
  Stop-Process -Id $p.Id -ErrorAction SilentlyContinue
  Start-Sleep -Seconds 1
}
Detach-Leftover
# Whatever fails below, nothing of this run stays behind.
trap { "FAILED: $_"; Stop-Ours; Detach-Leftover; exit 1 }
# The virtual display is the monitor that isn't among these.
$real = @([W]::Monitors() | ForEach-Object { $_.Split('|')[0] })
"monitors without the virtual display: $($real -join ', ')"

$index = 0
foreach ($content in $contents) {
  $index++
  $tag = "run$index"
  # stdin is a pipe: kvm_host reads launcher commands from it ("share <hwnd>" below).
  $psi = [System.Diagnostics.ProcessStartInfo]::new("$bin\host_a\kvm_host.exe", "--config `"$work\host.json`" --no-kvm --no-clipboard --no-discovery --codec $Codec --data-dir `"$work\host-data`" --pair-with $clientId --log `"$work\$tag-host.log`"")
  $psi.UseShellExecute = $false; $psi.RedirectStandardInput = $true; $psi.CreateNoWindow = $true
  $hostP = [System.Diagnostics.Process]::Start($psi)
  if (-not (Wait-Log "$work\$tag-host.log" 'listening on ports')) { throw "host didn't start" }
  $clientP = Start-Process "$bin\client_b\kvm_client.exe" -ArgumentList "--config `"$work\client.json`" --host 127.0.0.1 --no-kvm --no-clipboard --desk-no-vdd --data-dir `"$work\client-data`" --pair-with $hostId --stats `"$work\$tag.csv`" --log `"$work\$tag-client.log`"" -PassThru -WindowStyle Hidden
  if (-not (Wait-Log "$work\$tag-client.log" 'file channel connected')) { throw "client didn't connect" }

  if ($Mode -eq 'desk') {
    Start-Sleep -Seconds 4  # the host attaches the virtual display
    $vdd = [W]::Monitors() | Where-Object { $real -notcontains $_.Split('|')[0] } | Select-Object -First 1
    if (-not $vdd) { throw "no virtual display attached: $([W]::Monitors() -join '; ')" }
    $m = $vdd.Split('|'); $vx = [int]$m[1]; $vy = [int]$m[2]
    "[$tag] $($content.name): virtual display $vdd"
  } else {
    $vx = 0; $vy = 0
    "[$tag] $($content.name): window viewer, $Codec"
  }

  $app = & $content.start
  $hwnd = [IntPtr]::Zero
  for ($i = 0; $i -lt 40 -and $hwnd -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 250; $hwnd = [W]::WindowWithTitle($content.title) }
  if ($hwnd -eq [IntPtr]::Zero) { throw "no window titled $($content.title)" }
  [W]::SetWindowPos($hwnd, [IntPtr]::Zero, $vx + 40, $vy + 40, 1280, 800, 0x0040) | Out-Null  # SWP_SHOWWINDOW
  if ($Mode -eq 'viewer') {
    Start-Sleep -Seconds 1
    $hostP.StandardInput.WriteLine("share $($hwnd.ToInt64().ToString('x'))"); $hostP.StandardInput.Flush()
    if (-not (Wait-Log "$work\$tag-host.log" 'is on the other PC now' 10)) { throw "the window wasn't sent" }
  }
  Start-Sleep -Seconds $Seconds

  # Back to the real monitor, then end everything (Edge and Notepad are this run's own windows).
  if ($Mode -eq 'viewer') {
    $hostP.StandardInput.WriteLine("share $($hwnd.ToInt64().ToString('x'))"); $hostP.StandardInput.Flush()  # the window comes home
  } else {
    [W]::SetWindowPos($hwnd, [IntPtr]::Zero, 40, 40, 1280, 800, 0x0040) | Out-Null
  }
  Start-Sleep -Seconds 2
  Stop-Process -Id $clientP.Id -ErrorAction SilentlyContinue
  Stop-Process -Id $hostP.Id -ErrorAction SilentlyContinue
  if ($content.title -eq 'document.txt') { Stop-Process -Id $app.Id -ErrorAction SilentlyContinue }
  else { Get-CimInstance Win32_Process -Filter "Name='msedge.exe'" | Where-Object { $_.CommandLine -like "*$work\edge*" } | ForEach-Object { Stop-Process -Id $_.ProcessId -ErrorAction SilentlyContinue } }
  Start-Sleep -Seconds 3
  & "$bin\tools\video_stats_report.exe" "$work\$tag.csv" $content.name
}
# Leave no virtual display attached.
Detach-Leftover
"monitors at the end: $(([W]::Monitors() | ForEach-Object { $_.Split('|')[0] }) -join ', ')"

# Co-op on one PC: starts the relay and two games side by side, both moved by one gamepad (or one each with -TwoPads,
# below). Both play copies of the 1.4.3 save (build\prof-play-1 and -2), so nothing is saved back
# to the real profile. The copies are kept from one start to the next, with what was saved in them; -Fresh makes them
# again from the real profile. (Made fresh each start, a session's saves were lost at the next one, a play session,
# 2026-11-06.) Close either window (or this console) to end the session.
#   powershell -ExecutionPolicy Bypass -File tools\coop\play_two.ps1 [-Fresh] [-TwoPads] [-ShareWorld]   (or double-click "Play co-op (two pads).vbs")
# The other player is shown as a PlayerPuppet, TotK Online's stand-in (its pack made by tools/coop/make_puppet_pack.py).
# Horses stay off. -ShareWorld adds shared world progress (TOTK_COOP_SHARE_WORLD=1), not yet for play: on two saves
# each game's own load was taken as changes and sent to the other (2026-11-07).
param([string]$Name1 = "Player 1", [string]$Name2 = "Player 2", [string]$Size = "1904x1071", [switch]$Fresh, [switch]$TwoPads,
      [switch]$ShareWorld)
# One gamepad (the first) moves both games; Minus with the right stick click, or F9, switches to only the game in
# focus and back (input.c, pad_focus_gate). -TwoPads: game 1 reads the first gamepad, game 2 the second, as before.
$ErrorActionPreference = 'Stop'
# Programs started with no console window at all (CREATE_NO_WINDOW): hiding one after the fact does not work when
# Windows Terminal hosts the consoles. Their environment is this script's (set below for each game).
function Start-NoConsole([string]$exe, [string]$arguments, [string]$dir) {
    $si = New-Object System.Diagnostics.ProcessStartInfo $exe, $arguments
    $si.UseShellExecute = $false; $si.CreateNoWindow = $true; $si.WorkingDirectory = $dir
    [System.Diagnostics.Process]::Start($si)
}
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root
Get-Process totk -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 2

# The second game runs from build\v143play: links to build\v143's files, a logs folder of its own. (Not build\v143b,
# which the test scripts use too: a game left there that cannot be stopped holds its files, and the links could not
# be made again; 2026-11-07.)
$a = Join-Path $root 'build\v143'; $b = Join-Path $root 'build\v143play'
if (-not (Test-Path $b)) { New-Item -ItemType Directory $b | Out-Null }
foreach ($f in Get-ChildItem $a -Force) {
    $t = Join-Path $b $f.Name
    if ($f.Name -in 'logs', 'obj') { continue }
    if (Test-Path $t) { if ($f.PSIsContainer) { continue }; Remove-Item -Force $t }
    if ($f.PSIsContainer) { New-Item -ItemType Junction $t -Target $f.FullName | Out-Null }
    else { New-Item -ItemType HardLink $t -Target $f.FullName | Out-Null }
}

$relay = Start-NoConsole (Join-Path $root '.venv\Scripts\python.exe') 'server\coop_relay.py' $root
$games = @()
foreach ($g in @(@{dir = $a; n = 1; name = $Name1; pad = '0'}, @{dir = $b; n = 2; name = $Name2; pad = $(if ($TwoPads) { '1' } else { '0' })})) {
    $p = Join-Path $root "build\prof-play-$($g.n)"
    if ($Fresh -and (Test-Path $p)) { Remove-Item -Recurse -Force $p }
    if (-not (Test-Path $p)) { Copy-Item -Recurse (Join-Path $root 'build\profile-v143') $p }
    $env:TOTK_PROFILE_DIR = $p
    $env:TOTK_PAD_INDEX = $g.pad
    $env:TOTK_ASSUME_FOCUS = '1'        # both games keep running and reading their pad without the focus
    $env:TOTK_FULLSCREEN = '0'; $env:TOTK_WINDOW_SIZE = $Size
    $env:TOTK_COOP_RELAY = '127.0.0.1'; $env:TOTK_COOP_NAME = $g.name
    $env:TOTK_MOUSE_SENS = '0'          # no mouse camera, no cursor capture: the mouse moves between the windows
    $env:TOTK_COOP_HIDE_CONSOLE = '1'
    Remove-Item Env:TOTK_INPUT_SCRIPT, Env:TOTK_INPUT -ErrorAction SilentlyContinue
    if ($ShareWorld) { $env:TOTK_COOP_SHARE_WORLD = '1' } else { Remove-Item Env:TOTK_COOP_SHARE_WORLD -ErrorAction SilentlyContinue }
    $games += Start-NoConsole (Join-Path $g.dir 'totk.exe') '' $g.dir
    Start-Sleep 3
}
# Side by side on the primary monitor: game 1 on the left half, game 2 on the right (each window keeps its size).
Add-Type -AssemblyName System.Windows.Forms
Add-Type -Namespace W -Name U -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetWindowPos(System.IntPtr h, System.IntPtr after, int x, int y, int cx, int cy, uint flags); [DllImport("user32.dll")] public static extern System.IntPtr SetThreadDpiAwarenessContext(System.IntPtr c);'
[void][W.U]::SetThreadDpiAwarenessContext([IntPtr]-4)   # physical pixels, as the games' windows (per-monitor aware)
$wa = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
for ($i = 0; $i -lt $games.Count; $i++) {
    $g = $games[$i]
    for ($t = 0; $t -lt 100 -and $g.MainWindowHandle -eq [IntPtr]::Zero -and -not $g.HasExited; $t++) { Start-Sleep -Milliseconds 200; $g.Refresh() }
    if ($g.MainWindowHandle -ne [IntPtr]::Zero) {   # SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
        [void][W.U]::SetWindowPos($g.MainWindowHandle, [IntPtr]::Zero, $wa.X + $i * [int]($wa.Width / 2), $wa.Y, 0, 0, 0x15)
    }
}
if ($TwoPads) { "Session running: game 1 = first gamepad, game 2 = second gamepad. Load your save in both windows." }
else { "Session running: one gamepad moves both games. Minus + right stick click (or F9): only the game in focus, and back." }
while (-not ($games | Where-Object { $_.HasExited })) { Start-Sleep 1 }
$games | Where-Object { -not $_.HasExited } | Stop-Process -Force
Stop-Process -Id $relay.Id -Force -ErrorAction SilentlyContinue
"Session ended."
# The sync audit of the session: what the two games showed differently (tools/coop/sync_audit.py), kept in
# build/audit_report.txt and shown here.
$report = Join-Path $root 'build/audit_report.txt'
& (Join-Path $root '.venv/Scripts/python.exe') (Join-Path $PSScriptRoot 'sync_audit.py') (Join-Path $a 'logs/audit.jsonl') (Join-Path $b 'logs/audit.jsonl') |
    Tee-Object -FilePath $report
"(kept in $report)"

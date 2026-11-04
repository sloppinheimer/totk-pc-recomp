# Co-op on one PC, one game per gamepad: starts the relay and two games side by side; game 1 reads the first connected
# gamepad, game 2 the second. Both play copies of the 1.4.3 save (build\prof-play-1 and -2, made fresh each start), so
# nothing is saved back to the real profile. Close either window (or this console) to end the session.
#   powershell -ExecutionPolicy Bypass -File tools\coop\play_two.ps1   (or double-click "Play co-op (two pads).vbs")
param([string]$Name1 = "Player 1", [string]$Name2 = "Player 2", [string]$Size = "1920x1080")
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

# The second game runs from build\v143b: links to build\v143's files, a logs folder of its own.
$a = Join-Path $root 'build\v143'; $b = Join-Path $root 'build\v143b'
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
foreach ($g in @(@{dir = $a; n = 1; name = $Name1; pad = '0'}, @{dir = $b; n = 2; name = $Name2; pad = '1'})) {
    $p = Join-Path $root "build\prof-play-$($g.n)"
    if (Test-Path $p) { Remove-Item -Recurse -Force $p }
    Copy-Item -Recurse (Join-Path $root 'build\profile-v143') $p
    $env:TOTK_PROFILE_DIR = $p
    $env:TOTK_PAD_INDEX = $g.pad
    $env:TOTK_ASSUME_FOCUS = '1'        # both games keep running and reading their pad without the focus
    $env:TOTK_FULLSCREEN = '0'; $env:TOTK_WINDOW_SIZE = $Size
    $env:TOTK_COOP_RELAY = '127.0.0.1'; $env:TOTK_COOP_NAME = $g.name
    $env:TOTK_COOP_P2 = 'off'           # the second pad belongs to the other game, not to a local player 2
    $env:TOTK_MOUSE_SENS = '0'          # no mouse camera, no cursor capture: the mouse moves between the windows
    $env:TOTK_COOP_HIDE_CONSOLE = '1'
    Remove-Item Env:TOTK_INPUT_SCRIPT, Env:TOTK_INPUT, Env:TOTK_COOP_AUTOSPAWN -ErrorAction SilentlyContinue
    $games += Start-NoConsole (Join-Path $g.dir 'totk.exe') '' $g.dir
    Start-Sleep 3
}
"Session running: game 1 = first gamepad, game 2 = second gamepad. Load your save in both windows."
while (-not ($games | Where-Object { $_.HasExited })) { Start-Sleep 1 }
$games | Where-Object { -not $_.HasExited } | Stop-Process -Force
Stop-Process -Id $relay.Id -Force -ErrorAction SilentlyContinue
"Session ended."

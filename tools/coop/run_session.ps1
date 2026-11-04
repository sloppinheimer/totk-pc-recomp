# The menu's Host / Join / Leave without the menu: game A hosts (a relay inside it) -HostAt seconds into the world,
# game B joins it at 127.0.0.1 -JoinAt seconds in and leaves -LeaveAt seconds in (0: stays); -HostLeaveAt: A leaves. No external relay, no
# TOTK_COOP_RELAY. Both play the first save of copies of the 1.4.3 profile, no real input.
#   powershell -ExecutionPolicy Bypass -File tools\coop\run_session.ps1 [-Seconds 90]
param([int]$Seconds = 90, [int]$HostAt = 5, [int]$JoinAt = 10, [int]$LeaveAt = 40,
      [string]$ScriptA = "tools\coop\idle.txt", [string]$ScriptB = "tools\coop\idle.txt", [string]$MenuTestB = "",
      [string]$ShotsDir = "", [int]$HostLeaveAt = 0)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root
Get-Process totk -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 2
$a = Join-Path $root 'build\v143'; $b = Join-Path $root 'build\v143b'
if (-not (Test-Path $b)) { New-Item -ItemType Directory $b | Out-Null }
foreach ($f in Get-ChildItem $a -Force) {
    $t = Join-Path $b $f.Name
    if ($f.Name -in 'logs', 'obj') { continue }
    if (Test-Path $t) { if ($f.PSIsContainer) { continue }; Remove-Item -Force $t }
    if ($f.PSIsContainer) { New-Item -ItemType Junction $t -Target $f.FullName | Out-Null }
    else { New-Item -ItemType HardLink $t -Target $f.FullName | Out-Null }
}
. (Join-Path $PSScriptRoot 'quiet.ps1')
Remove-Item Env:TOTK_COOP_RELAY, Env:TOTK_COOP_TEST_HOST, Env:TOTK_COOP_TEST_JOIN, Env:TOTK_COOP_TEST_LEAVE, Env:TOTK_MENU_TEST, Env:TOTK_MENU_TEST_DIR -ErrorAction SilentlyContinue
$games = @()
foreach ($g in @(@{dir = $a; prof = 'build\prof-coop-a'; script = $ScriptA; name = 'A'; input = $null},
                 @{dir = $b; prof = 'build\prof-coop-b'; script = $ScriptB; name = 'B'; input = 'none'})) {
    $p = Join-Path $root $g.prof
    if (Test-Path $p) { Remove-Item -Recurse -Force $p }
    Copy-Item -Recurse (Join-Path $root 'build\profile-v143') $p
    $env:TOTK_PROFILE_DIR = $p
    $env:TOTK_INPUT_SCRIPT = (Resolve-Path $g.script).Path
    $env:TOTK_FULLSCREEN = '0'; $env:TOTK_WINDOW_SIZE = '960x540'
    $env:TOTK_COOP_NAME = "Player $($g.name)"; $env:TOTK_COOP_P2 = 'off'
    if ($g.input) { $env:TOTK_INPUT = $g.input } else { Remove-Item Env:TOTK_INPUT -ErrorAction SilentlyContinue }
    if ($g.name -eq 'A') { $env:TOTK_COOP_TEST_HOST = "$HostAt"; if ($HostLeaveAt) { $env:TOTK_COOP_TEST_LEAVE = "$HostLeaveAt" } else { Remove-Item Env:TOTK_COOP_TEST_LEAVE -ErrorAction SilentlyContinue } }
    else {
        Remove-Item Env:TOTK_COOP_TEST_HOST -ErrorAction SilentlyContinue
        $env:TOTK_COOP_TEST_JOIN = "${JoinAt}:127.0.0.1"
        if ($LeaveAt) { $env:TOTK_COOP_TEST_LEAVE = "$LeaveAt" } else { Remove-Item Env:TOTK_COOP_TEST_LEAVE -ErrorAction SilentlyContinue }
        if ($MenuTestB) { $env:TOTK_MENU_TEST = $MenuTestB; $env:TOTK_MENU_TEST_DIR = $ShotsDir }
    }
    $games += Start-Quiet (Join-Path $g.dir 'totk.exe') '' $g.dir
    Start-Sleep 3
}
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline -and ($games | Where-Object { -not $_.HasExited })) { Start-Sleep 1 }
$games | Where-Object { -not $_.HasExited } | Stop-Process -Force
Start-Sleep 1
foreach ($d in $a, $b) {
    "=== $d"
    Select-String -Path (Join-Path $d 'logs\totk.log') -Pattern 'coop (net|relay)|coop: (test|slot \d: (Player|its)|remote player|player slot)|FATAL' | Select-Object -ExpandProperty Line
}

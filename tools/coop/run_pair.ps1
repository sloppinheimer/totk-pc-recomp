# Two games on this PC in one co-op session through a local relay (server/coop_relay.py): game A plays -ScriptA, game B
# plays -ScriptB with no real input. Each runs on a copy of the 1.4.3 profile and logs to its own folder
# (build\v143 and build\v143b, the second made of links to the first's files). Prints both games' co-op lines.
#   powershell -ExecutionPolicy Bypass -File tools\coop\run_pair.ps1 [-Seconds 90]
#   -FakeGearA "Weapon_Sword_070,Weapon_Bow_001,Weapon_Shield_001": game A reports these items (see coop_players.c)
#   -ShotsA / -ShotsB w20-30:2 -ShotsDir <dir>: that game saves screenshots (TOTK_SHOTS syntax, boot.c) in <dir>\A or \B
#   -FakeDyeA ",,,,3,3": game A reports these armor dyes (slot order; empty keeps the real one)
#   -QuitBAfter 60: game B is closed 60 s after it starts (a player leaving)
#   -SpawnA / -SpawnB "Enemy_Bokoblin_Junior@20": that game spawns the actor beside its Link at that world time
#   -ShotsSmall: the screenshots box-filtered to 480x270 (else full size)
#   -Profile build\prof-play-1: both games play copies of that profile instead of the 1.4.3 one (it is not written)
#   -BDir build\v143c: game B runs from that folder instead of build\v143b (a stuck game may hold its totk.exe)
#   -FakeAtA 25: game A's fakes (-FakeGearA, -FakeDyeA, -FakeHandA) apply only from 25 s into its world
#   -MarkShotsA / -MarkShotsB 30-60:2: that game saves the swapchain image, the overlay's map markers included, at those
#    world seconds as <ShotsDir>\A-overlay\mark-<s>.png (TOTK_COOP_TEST_MARKSHOTS; -ShotsA/-ShotsB save the picture
#    before the overlay); -MarkSelf: both games also mark their own Link and log the map transform every 2 s
#   -Port 41950: the relay on that UDP port (41900 may be taken by a game hosting on this PC)
#   -EnvA / -EnvB "TOTK_COOP_TEST_TRAVEL=40;TOTK_COOP_TEST_NOTICESHOT=1": these settings for that game only
param([int]$Seconds = 90, [string]$ScriptA = "tools\coop\walk.txt", [string]$ScriptB = "tools\coop\idle.txt", [string]$FakeGearA = "", [string]$FakeDyeA = "", [string]$FakeHandA = "",
      [string]$ShotsA = "", [string]$ShotsB = "", [string]$ShotsDir = "", [int]$QuitBAfter = 0, [string]$FakeAtA = "", [string]$SpawnA = "", [string]$SpawnB = "", [string]$Profile = "build\profile-v143", [switch]$ShotsSmall, [string]$BDir = "build\v143b",
      [string]$MarkShotsA = "", [string]$MarkShotsB = "", [switch]$MarkSelf, [string]$EnvA = "", [string]$EnvB = "", [int]$Port = 41900)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root
# Only this checkout's games (from its build folder): a game of another checkout may be someone playing.
Get-Process totk -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith((Join-Path $root 'build\'), [StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force
Start-Sleep 2

# build\v143b: hard links to build\v143's files, junctions to its folders, a logs folder of its own.
$a = Join-Path $root 'build\v143'; $b = Join-Path $root $BDir
if (-not (Test-Path $b)) { New-Item -ItemType Directory $b | Out-Null }
foreach ($f in Get-ChildItem $a -Force) {
    $t = Join-Path $b $f.Name
    if ($f.Name -in 'logs', 'obj') { continue }
    if (Test-Path $t) { if ($f.PSIsContainer) { continue }; Remove-Item -Force $t }
    if ($f.PSIsContainer) { New-Item -ItemType Junction $t -Target $f.FullName | Out-Null }
    else { New-Item -ItemType HardLink $t -Target $f.FullName | Out-Null }
}

. (Join-Path $PSScriptRoot 'quiet.ps1')   # no consoles, no mouse capture, the windows stay behind
$relay = Start-Quiet (Join-Path $root '.venv\Scripts\pythonw.exe') "server\coop_relay.py --port $Port --log build\relay.log" $root
$games = @()
foreach ($g in @(@{dir = $a; prof = 'build\prof-coop-a'; script = $ScriptA; name = 'A'; input = $null},
                 @{dir = $b; prof = 'build\prof-coop-b'; script = $ScriptB; name = 'B'; input = 'none'})) {
    $p = Join-Path $root $g.prof
    if (Test-Path $p) { Remove-Item -Recurse -Force $p }
    Copy-Item -Recurse (Join-Path $root $Profile) $p
    $env:TOTK_PROFILE_DIR = $p
    $env:TOTK_INPUT_SCRIPT = (Resolve-Path $g.script).Path
    $env:TOTK_ASSUME_FOCUS = '1'; $env:TOTK_FULLSCREEN = '0'; $env:TOTK_WINDOW_SIZE = '960x540'
    $env:TOTK_COOP_RELAY = "127.0.0.1:$Port"; $env:TOTK_COOP_NAME = $g.name; $env:TOTK_COOP_P2 = 'off'
    if ($g.input) { $env:TOTK_INPUT = $g.input } else { Remove-Item Env:TOTK_INPUT -ErrorAction SilentlyContinue }
    if ($g.name -eq 'A' -and $FakeGearA) { $env:TOTK_COOP_FAKEGEAR = $FakeGearA } else { Remove-Item Env:TOTK_COOP_FAKEGEAR -ErrorAction SilentlyContinue }
    if ($g.name -eq 'A' -and $FakeDyeA) { $env:TOTK_COOP_FAKEDYE = $FakeDyeA } else { Remove-Item Env:TOTK_COOP_FAKEDYE -ErrorAction SilentlyContinue }
    if ($g.name -eq 'A' -and $FakeAtA) { $env:TOTK_COOP_FAKE_AT = $FakeAtA } else { Remove-Item Env:TOTK_COOP_FAKE_AT -ErrorAction SilentlyContinue }
    if ($g.name -eq 'A' -and $FakeHandA) { $env:TOTK_COOP_FAKEHAND = $FakeHandA } else { Remove-Item Env:TOTK_COOP_FAKEHAND -ErrorAction SilentlyContinue }
    $spawn = if ($g.name -eq 'A') { $SpawnA } else { $SpawnB }
    if ($spawn) { $env:TOTK_COOP_TEST_SPAWN = $spawn } elseif ($SpawnA -or $SpawnB) { Remove-Item Env:TOTK_COOP_TEST_SPAWN -ErrorAction SilentlyContinue }
    $shots = if ($g.name -eq 'A') { $ShotsA } else { $ShotsB }
    if ($shots) { $sd = Join-Path (Join-Path $root $ShotsDir) $g.name; New-Item -ItemType Directory -Force $sd | Out-Null; $env:TOTK_SHOTS = $shots; $env:TOTK_SHOTS_DIR = $sd; if ($ShotsSmall) { Remove-Item Env:TOTK_SHOTS_FULL -ErrorAction SilentlyContinue } else { $env:TOTK_SHOTS_FULL = '1' } }
    else { Remove-Item Env:TOTK_SHOTS, Env:TOTK_SHOTS_DIR, Env:TOTK_SHOTS_FULL -ErrorAction SilentlyContinue }
    $mark = if ($g.name -eq 'A') { $MarkShotsA } else { $MarkShotsB }
    if ($mark) { $md = Join-Path (Join-Path $root $ShotsDir) "$($g.name)-overlay"; New-Item -ItemType Directory -Force $md | Out-Null; $env:TOTK_COOP_TEST_MARKSHOTS = $mark; $env:TOTK_MENU_TEST_DIR = $md }
    else { Remove-Item Env:TOTK_COOP_TEST_MARKSHOTS, Env:TOTK_MENU_TEST_DIR -ErrorAction SilentlyContinue }
    if ($MarkSelf) { $env:TOTK_COOP_TEST_MARKSELF = '1' } else { Remove-Item Env:TOTK_COOP_TEST_MARKSELF -ErrorAction SilentlyContinue }
    foreach ($kv in (($EnvA + ';' + $EnvB) -split ';' | Where-Object { $_ -match '=' })) { Remove-Item "Env:$(($kv -split '=', 2)[0])" -ErrorAction SilentlyContinue }
    $mine = if ($g.name -eq 'A') { $EnvA } else { $EnvB }
    foreach ($kv in ($mine -split ';' | Where-Object { $_ -match '=' })) { $k, $v = $kv -split '=', 2; Set-Item "Env:$k" $v }
    $games += Start-Quiet (Join-Path $g.dir 'totk.exe') '' $g.dir
    Start-Sleep 3
}
$deadline = (Get-Date).AddSeconds($Seconds)
$bStart = Get-Date
while ((Get-Date) -lt $deadline -and ($games | Where-Object { -not $_.HasExited })) {
    Start-Sleep 1
    if ($QuitBAfter -and -not $games[1].HasExited -and ((Get-Date) - $bStart).TotalSeconds -ge $QuitBAfter) {
        "game B closed"; Stop-Process -Id $games[1].Id -Force
    }
}
$games | Where-Object { -not $_.HasExited } | Stop-Process -Force
Stop-Process -Id $relay.Id -Force -ErrorAction SilentlyContinue
Start-Sleep 1
foreach ($d in $a, $b) {
    "=== $d"
    Select-String -Path (Join-Path $d 'logs\totk.log') -Pattern 'coop net|coop: (slot|player slot|remote|markers|notice|test: notice|test: travel)|FATAL' | Select-Object -ExpandProperty Line
}
"=== relay"; Get-Content (Join-Path $root 'build\relay.log')

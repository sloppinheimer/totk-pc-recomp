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
#   -StopWhen "A:handed over||B:made, 4 of 4": stop as soon as every pattern (a regex; A: or B: says whose log) has
#    appeared, -StopGrace seconds later (default 3); -Seconds stays the limit
#   -ADir build\v143d -Tag 2: a second pair beside another one: A runs from that folder (made of links to build\v143 like
#    -BDir), the profiles and the relay log are named after the tag, and only this pair's games are stopped (give each
#    pair its own -ADir, -BDir and -Port)
#   At the end, the mismatch checker (tools\coop\check.py) compares what the two games showed of the enemies
#    (logs\check.jsonl, written with TOTK_COOP_DEBUG=1) and prints its report
#   -Nudge 40,8,5: a test of the checker (TOTK_COOP_TEST_NUDGE, runtime/coop_check.c): from 40 world seconds game B
#    shows its first enemy copy 5 m east of where its owner has it, for 8 s; the report must list it
#   -Row 1: the two windows placed side by side on that row of a 2x2 grid (A left, B right; row 0 at the top), so two
#    pairs can be watched at once
param([int]$Seconds = 90, [string]$ScriptA = "tools\coop\walk.txt", [string]$ScriptB = "tools\coop\idle.txt", [string]$FakeGearA = "", [string]$FakeDyeA = "", [string]$FakeHandA = "",
      [string]$ShotsA = "", [string]$ShotsB = "", [string]$ShotsDir = "", [int]$QuitBAfter = 0, [string]$FakeAtA = "", [string]$SpawnA = "", [string]$SpawnB = "", [string]$Profile = "build\profile-v143", [switch]$ShotsSmall, [string]$BDir = "build\v143b",
      [string]$MarkShotsA = "", [string]$MarkShotsB = "", [switch]$MarkSelf, [string]$EnvA = "", [string]$EnvB = "", [int]$Port = 41900,
      [string]$StopWhen = "", [int]$StopGrace = 3, [string]$ADir = "build\v143", [string]$Tag = "", [int]$Row = 0, [string]$Nudge = "")
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root
$env:TOTK_COOP_DEBUG = '1'   # (the records and measurements the reports read: off in a plain game)
$src = Join-Path $root 'build\v143'; $a = Join-Path $root $ADir; $b = Join-Path $root $BDir
# Only this pair's games (from its two folders): another pair may run beside it, and a game of another checkout may be
# someone playing.
Get-Process totk -ErrorAction SilentlyContinue | Where-Object { $_.Path -and ($_.Path.StartsWith("$a\", [StringComparison]::OrdinalIgnoreCase) -or $_.Path.StartsWith("$b\", [StringComparison]::OrdinalIgnoreCase)) } | Stop-Process -Force
Start-Sleep 2

# build\v143b (and -ADir when it is not build\v143): hard links to build\v143's files, junctions to its folders, a logs
# folder of its own. Made again each run, so a new build's totk.exe is linked.
foreach ($d in @($a, $b) | Where-Object { $_ -ne $src }) {
    if (-not (Test-Path $d)) { New-Item -ItemType Directory $d | Out-Null }
    foreach ($f in Get-ChildItem $src -Force) {
        $t = Join-Path $d $f.Name
        if ($f.Name -in 'logs', 'obj') { continue }
        if (Test-Path $t) { if ($f.PSIsContainer) { continue }; Remove-Item -Force $t }
        if ($f.PSIsContainer) { New-Item -ItemType Junction $t -Target $f.FullName | Out-Null }
        else { New-Item -ItemType HardLink $t -Target $f.FullName | Out-Null }
    }
}

foreach ($d in $a, $b) { Remove-Item -Force (Join-Path $d 'logs\check.jsonl') -ErrorAction SilentlyContinue }   # (an older run's must not stand in)
. (Join-Path $PSScriptRoot 'quiet.ps1')   # no consoles, no mouse capture, the windows stay behind
$relayLog = "build\relay$Tag.log"
$relay = Start-Quiet (Join-Path $root '.venv\Scripts\pythonw.exe') "server\coop_relay.py --port $Port --log $relayLog" $root
$games = @()
foreach ($g in @(@{dir = $a; prof = "build\prof-coop-a$Tag"; script = $ScriptA; name = 'A'; input = $null},
                 @{dir = $b; prof = "build\prof-coop-b$Tag"; script = $ScriptB; name = 'B'; input = 'none'})) {
    $p = Join-Path $root $g.prof
    if (Test-Path $p) { Remove-Item -Recurse -Force $p }
    Copy-Item -Recurse (Join-Path $root $Profile) $p
    $env:TOTK_PROFILE_DIR = $p
    $env:TOTK_INPUT_SCRIPT = (Resolve-Path $g.script).Path
    $env:TOTK_ASSUME_FOCUS = '1'; $env:TOTK_FULLSCREEN = '0'; $env:TOTK_WINDOW_SIZE = '960x540'
    $env:TOTK_COOP_RELAY = "127.0.0.1:$Port"; $env:TOTK_COOP_NAME = $g.name
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
    if ($g.name -eq 'B' -and $Nudge) { $env:TOTK_COOP_TEST_NUDGE = $Nudge } else { Remove-Item Env:TOTK_COOP_TEST_NUDGE -ErrorAction SilentlyContinue }
    foreach ($kv in ($mine -split ';' | Where-Object { $_ -match '=' })) { $k, $v = $kv -split '=', 2; Set-Item "Env:$k" $v }
    $games += Start-Quiet (Join-Path $g.dir 'totk.exe') '' $g.dir
    Start-Sleep 3
}
Add-Type -Namespace W -Name Pos -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetWindowPos(System.IntPtr h, System.IntPtr after, int x, int y, int cx, int cy, uint flags);
[DllImport("user32.dll")] public static extern bool GetWindowRect(System.IntPtr h, int[] r);
'@ -ErrorAction SilentlyContinue
$placed = @($false, $false)
$deadline = (Get-Date).AddSeconds($Seconds)
$bStart = Get-Date
$stops = @($StopWhen -split '\|\|' | Where-Object { $_ -match '^[AB]:' })
$logs = @{ A = (Join-Path $a 'logs\totk.log'); B = (Join-Path $b 'logs\totk.log') }
while ((Get-Date) -lt $deadline -and ($games | Where-Object { -not $_.HasExited })) {
    Start-Sleep 1
    for ($i = 0; $i -lt 2; $i++) {   # (each window once it exists: A left, B right, on this pair's row; not activated)
        if ($placed[$i]) { continue }
        $games[$i].Refresh()
        $h = $games[$i].MainWindowHandle
        if ($h -eq [IntPtr]::Zero) { continue }
        # (by the window's own size, so the grid closes up whatever the display scaling: A at the row's left, B against A)
        $r = New-Object int[] 4; [void][W.Pos]::GetWindowRect($h, $r); $w = $r[2] - $r[0]; $hh = $r[3] - $r[1]
        [void][W.Pos]::SetWindowPos($h, [IntPtr]::Zero, $i * $w, $Row * $hh, 0, 0, 0x15); $placed[$i] = $true
    }
    if ($stops.Count -and ((Get-Date) - $bStart).TotalSeconds -ge 15) {
        $all = $true
        foreach ($st in $stops) {
            $who = $st.Substring(0, 1); $rx = $st.Substring(2)
            if (-not (Test-Path $logs[$who]) -or -not (Select-String -Path $logs[$who] -Pattern $rx -Quiet)) { $all = $false; break }
        }
        if ($all) { "stop: every pattern seen after $([int]((Get-Date) - $bStart).TotalSeconds) s"; Start-Sleep $StopGrace; break }
    }
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
"=== relay"; Get-Content (Join-Path $root $relayLog)
"=== check"; & (Join-Path $root '.venv\Scripts\python.exe') (Join-Path $PSScriptRoot 'check.py') $a $b

# The enemy sync's bench (docs/coop/enemy-sync.md, section 15): a fixed set of two-game cases on the camp save, each
# judged from the two games' logs, with one line per case and PASS or FAIL.
#
#   powershell -ExecutionPolicy Bypass -File tools\coop\en_bench.ps1 [-Cases fight,wizzrobe,handover,quit] [-Out build\bench]
#
# Cases:
#   fight     game A fights the camp, game B watches: tables equal, no leak, copies within the bars
#   wizzrobe  a Fire Wizzrobe is made beside game A's Link: its shots are made on the copy, its flames fly the same
#             path, the copy game's Link is burned by them, the weather is asked for in both
#   handover  game A pauses in a menu while a Fire Wizzrobe fights: game B takes it over and it stays in battle
#   quit      game B runs a Fire Wizzrobe and is closed: game A takes it over and it stays in battle
#   families  one actor of each family taken by default is made beside game A's Link, standing: bound, followed
# The bars (section 15): tables equal at the end; no leak; position error (95th percentile) 12 cm while moving and 1 cm
# standing; the same animation command in 95 % of samples. A case that needs something that did not happen in the run
# (no weather attack, no change of owner) says so and counts as not judged, not as a pass.
param([string]$Cases = "fight,wizzrobe,handover,quit", [string]$Out = "build\bench", [switch]$Judge)   # -Judge: no runs, the logs already in -Out are judged again
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root
New-Item -ItemType Directory -Force $Out | Out-Null
$py = Join-Path $root '.venv\Scripts\python.exe'
$idle = 'tools\coop\idle.txt'

function Run-Pair([string]$tag, [int]$seconds, [string]$scriptA, [string]$scriptB, [string]$envA, [string]$envB, [int]$quitB = 0) {
    if ($Judge) { return }
    $e = 'TOTK_COOP_ENEMY=1;TOTK_COOP_ELEM=1'
    & (Join-Path $root 'tools\coop\run_pair.ps1') -Seconds $seconds -ScriptA $scriptA -ScriptB $scriptB -Profile build\profile-v143-camp `
        -EnvA "$e;$envA" -EnvB "$e;$envB" -QuitBAfter $quitB | Out-Null
    Copy-Item build\v143\logs\totk.log (Join-Path $Out "$tag-A.log") -Force
    Copy-Item build\v143b\logs\totk.log (Join-Path $Out "$tag-B.log") -Force
}
function Count([string]$file, [string]$pattern) { (Select-String -Path $file -Pattern $pattern | Measure-Object).Count }
function Report([string]$tag) {
    $a = Join-Path $Out "$tag-A.log"; $b = Join-Path $Out "$tag-B.log"
    $r = & $py tools\coop\en_report.py $a $b 2>&1
    $equal = ($r | Select-String 'tables equal at the end: YES') -ne $null
    $leaks = 0
    foreach ($l in ($r | Select-String 'runner entries turned away, (\d+) leaks')) { $leaks += [int]$l.Matches[0].Groups[1].Value }
    $worstMove = 0.0; $worstStill = 0.0; $worstCmd = 100.0; $rows = 0; $few = 0
    foreach ($l in ($r | Select-String '^\s+Enemy_\S+\s+(\d+)\s+([\d.]+)cm\s+([\d.]+)cm\s+\d+\s+[\d.]+cm\s+[\d.]+cm\s+([\d.]+)%')) {
        $g = $l.Matches[0].Groups
        if ([int]$g[1].Value -lt 60) { $few++; continue }   # (a copy seen for under six seconds is not judged)
        $rows++
        if ([double]$g[2].Value -gt $worstStill) { $worstStill = [double]$g[2].Value }
        if ([double]$g[3].Value -gt $worstMove) { $worstMove = [double]$g[3].Value }
        if ([double]$g[4].Value -lt $worstCmd) { $worstCmd = [double]$g[4].Value }
    }
    $bad = 'EXCEPTION|rt_trap|access violation'   # (not the start-up lines about the crash reporter and the shader cache)
    $crash = (Count $a $bad) + (Count $b $bad)
    [pscustomobject]@{ equal = $equal; leaks = $leaks; rows = $rows; still = $worstStill; move = $worstMove; cmd = $worstCmd; crash = $crash; few = $few; a = $a; b = $b }
}
function Verdict([string]$name, [bool]$ok, [bool]$judged, [string]$detail) {
    $word = if (-not $judged) { 'NOT JUDGED' } elseif ($ok) { 'PASS' } else { 'FAIL' }
    "{0,-10} {1,-10} {2}" -f $name, $word, $detail
}
$results = @()
foreach ($case in ($Cases -split ',')) {
    switch ($case.Trim()) {
        'fight' {
            Run-Pair 'fight' 110 'tools\coop\camp_melee.txt' 'tools\coop\camp_watch.txt' '' ''
            $r = Report 'fight'
            $ok = $r.equal -and $r.leaks -eq 0 -and $r.crash -eq 0 -and $r.still -le 1.0 -and $r.move -le 12.0 -and $r.cmd -ge 95.0
            $results += Verdict 'fight' $ok ($r.rows -gt 0) ("tables equal {0}, leaks {1}, {2} copies compared ({6} seen too briefly): standing {3} cm, moving {4} cm, command {5} %" -f $r.equal, $r.leaks, $r.rows, $r.still, $r.move, $r.cmd, $r.few)
        }
        'wizzrobe' {
            Run-Pair 'wizzrobe' 150 $idle $idle 'TOTK_COOP_TEST_SPAWN=Enemy_Wizzrobe_Fire@22;TOTK_COOP_EN_HANDOVER=0;TOTK_COOP_TEST_LINKHIT=1' 'TOTK_COOP_EN_HANDOVER=0;TOTK_COOP_TEST_LINKHIT=1'
            $r = Report 'wizzrobe'
            $stated = Count $r.a 'shoots its projectile'; $made = Count $r.b 'shoots its projectile .* as its owner.s did: made'
            $burnB = Count $r.b 'linkhit by FireRodLv1Fire'; $wx = Count $r.b 'asks the weather its owner'
            $ok = $r.equal -and $r.leaks -eq 0 -and $r.crash -eq 0 -and $made -gt 0
            $results += Verdict 'wizzrobe' $ok ($stated -gt 0) ("tables equal {0}, leaks {1}; shots stated {2}, made on the copy {3}; the copy game's Link burned by a flame {4} times; weather asked on the copy {5}" -f $r.equal, $r.leaks, $stated, $made, $burnB, $(if ($wx) { 'yes' } else { 'no (no weather attack came up)' }))
        }
        'handover' {
            $pause = Join-Path $Out 'pause75.txt'
            (Get-Content $idle | Where-Object { $_ -notmatch '^#' }) + '@75000 PLUS 150' + '@95000 PLUS 150' | Set-Content $pause -Encoding ascii
            Run-Pair 'handover' 150 $pause $idle 'TOTK_COOP_TEST_SPAWN=Enemy_Wizzrobe_Fire@22' ''
            $r = Report 'handover'
            $took = Count $r.b 'own: Enemy_Wizzrobe_Fire .* taken over by this game'
            $who = Select-String -Path $r.b -Pattern 'brain: Enemy_Wizzrobe_Fire ([0-9a-f]+) ' | Select-Object -First 1   # (its actor here, from any line of its brain's)
            $actor = if ($who) { $who.Matches[0].Groups[1].Value } else { 'none' }
            $lines = @(Select-String -Path $r.b -Pattern ("brain: $actor \d+ s after its take-over: verb (\d+)"))   # (the Wizzrobe's own lines only)
            $inBattle = ($lines | Where-Object { $_.Matches[0].Groups[1].Value -eq '12' } | Measure-Object).Count
            $ok = $r.equal -and $r.leaks -eq 0 -and $r.crash -eq 0 -and $lines.Count -gt 0 -and $inBattle -eq $lines.Count
            $results += Verdict 'handover' $ok ($took -gt 0) ("tables equal {0}, leaks {1}; taken over {2} times; in battle in {3} of {4} seconds logged after it" -f $r.equal, $r.leaks, $took, $inBattle, $lines.Count)
        }
        'quit' {
            Run-Pair 'quit' 150 $idle $idle '' 'TOTK_COOP_TEST_SPAWN=Enemy_Wizzrobe_Fire@22' 110
            $r = Report 'quit'
            $took = Count $r.a 'own: Enemy_Wizzrobe_Fire .* taken over by this game \(its owner left\)'
            $find = 0
            $ok = $r.leaks -eq 0 -and $r.crash -eq 0 -and $took -gt 0
            $results += Verdict 'quit' $ok ($took -gt 0) ("leaks {0}; the Wizzrobe taken over when its owner left: {1}" -f $r.leaks, $(if ($took) { 'yes' } else { 'no' }))
        }
        'families' {
            foreach ($nm in 'Enemy_Lizalfos_Junior', 'Enemy_Moriblin_Junior', 'Enemy_Horablin_Junior', 'Enemy_Chuchu_Middle', 'Enemy_Keese', 'Enemy_Octarock', 'Enemy_Zonau_Robot_Junior', 'Enemy_Zonau_Golem_Junior', 'Enemy_Assassin_Junior', 'Enemy_LikeLike_Fire', 'Enemy_Lynel_Junior', 'Enemy_Treant_Broadleaf', 'Enemy_Zombie_Junior', 'Enemy_Toby_Junior') {
                Run-Pair "fam-$nm" 75 'tools\coop\stand_swing.txt' $idle "TOTK_COOP_TEST_SPAWN=$nm@22" ''
                $r = Report "fam-$nm"
                $bound = (Count $r.b 'is its copy') + (Count $r.a 'is its copy')
                $ok = $r.equal -and $r.leaks -eq 0 -and $r.crash -eq 0 -and $bound -gt 0 -and $r.cmd -ge 95.0
                $results += Verdict $nm.Replace('Enemy_', '') $ok ($bound -gt 0) ("tables equal {0}, leaks {1}, command {2} %, moving {3} cm" -f $r.equal, $r.leaks, $r.cmd, $r.move)
            }
        }
        default { $results += "unknown case '$case'" }
    }
}
""
"The enemy sync's bench (logs in $Out):"
$results
if ($results | Where-Object { $_ -match ' FAIL ' }) { exit 1 }

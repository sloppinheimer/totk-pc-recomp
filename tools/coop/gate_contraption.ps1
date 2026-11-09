# The contraption sync gate suite (docs/coop/contraption-sync-v3.md, P1 on): the two-game scenarios every phase gate
# runs, two pairs at a time, each stopped as soon as its pass lines are in the logs (run_pair.ps1 -StopWhen; its seconds
# stay the limit). Logs land in build\<Tag>_<scenario>_A.log and _B.log; every game logs a "coop: clean:" line every
# 10 s whose failure counts are 0 in a clean run.
#   powershell -ExecutionPolicy Bypass -File tools\coop\gate_contraption.ps1 [-Tag gate] [-Only tog,carry]
#   tog      B toggles A's Fanplane on and off (devices decided where it runs); the contraption channel under load too
#   partoff  A hits a fan off its Fanplane; B makes the 3-part shape
#   carry    B grabs A's Fanplane and carries it (the grab hand-over)
#   stick    B takes the steering stick of A's stick build (the stick hand-over); A's fans run on B's say
param([string]$Tag = "gate", [string[]]$Only = @())
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root
$on = 'TOTK_COOP_CONTRAPTION=1;TOTK_COOP_TEST_ABLOG=1'   # (ABLOG turns the ABHIT/ABSTICK test switches on; its watch is off)
$runs = @(
  @{ n = 'tog'; s = 120; a = 'tools\coop\autobuild_noy_3rd.txt'; b = 'tools\coop\idle_3rd.txt'
     ea = "$on;TOTK_COOP_TEST_CTRLOAD=25,3"; eb = "$on;TOTK_COOP_TEST_CPTOGGLE=46,54;TOTK_COOP_TEST_CTRLOAD=25,3"
     stop = 'B:its devices off where it runs, on here: toggled||B:ctr test: sent [6-9]\d control'; grace = 2 }
  @{ n = 'carry'; s = 125; a = 'tools\coop\autobuild_noy_3rd.txt'; b = 'tools\coop\autobuild_carry_b_3rd.txt'
     ea = $on; eb = "$on;TOTK_COOP_TEST_ABHIT=46;TOTK_COOP_TEST_ABHIT_B=46;TOTK_COOP_TEST_ABHIT_DIST=2.2;TOTK_COOP_TEST_ABHIT_FANX=1"
     stop = 'B:asked to run it||A:handed over'; grace = 10 }
  @{ n = 'partoff'; s = 120; a = 'tools\coop\autobuild_part_3rd.txt'; b = 'tools\coop\idle_3rd.txt'
     ea = "$on;TOTK_COOP_TEST_ABHIT=36"; eb = $on
     stop = 'B:made, 3 of 3'; grace = 3 }
  @{ n = 'stick'; s = 120; a = 'tools\coop\autobuild_stick_3rd.txt'; b = 'tools\coop\autobuild_stick_off_b_3rd.txt'
     ea = "$on;TOTK_COOP_TEST_ABSTICK=50,2"; eb = "$on;TOTK_COOP_TEST_ABSTICK=50,2"
     stop = 'A:handed over||A:fan on/10'; grace = 5 }
)
if ($Only.Count) { $runs = @($runs | Where-Object { $Only -contains $_.n }) }
$slots = @(@{ adir = 'build\v143'; bdir = 'build\v143c'; port = 41991; tag = '' }, @{ adir = 'build\v143d'; bdir = 'build\v143e'; port = 41992; tag = '2' })
$t0 = Get-Date
for ($i = 0; $i -lt $runs.Count; $i += 2) {
  $batch = @()
  for ($j = 0; $j -lt 2 -and $i + $j -lt $runs.Count; $j++) {
    $r = $runs[$i + $j]; $sl = $slots[$j]
    $argv = @('-ExecutionPolicy', 'Bypass', '-File', 'tools\coop\run_pair.ps1', '-Seconds', $r.s, '-ScriptA', $r.a, '-ScriptB', $r.b,
              '-ADir', $sl.adir, '-BDir', $sl.bdir, '-Port', $sl.port, '-EnvA', $r.ea, '-EnvB', $r.eb, '-StopWhen', $r.stop, '-StopGrace', $r.grace)
    if ($sl.tag) { $argv += @('-Tag', $sl.tag) }
    $argv = @($argv | ForEach-Object { if ("$_" -match '\s') { '"' + $_ + '"' } else { "$_" } })   # (Start-Process joins them with spaces)
    $batch += @{ r = $r; sl = $sl; p = (Start-Process powershell -ArgumentList $argv -WindowStyle Hidden -PassThru -RedirectStandardOutput "build\$($Tag)_$($r.n).out") }
    Start-Sleep 5   # (the two pairs' games start a little apart)
  }
  foreach ($x in $batch) {
    $x.p.WaitForExit()
    Copy-Item (Join-Path $x.sl.adir 'logs\totk.log') "build\$($Tag)_$($x.r.n)_A.log"
    Copy-Item (Join-Path $x.sl.bdir 'logs\totk.log') "build\$($Tag)_$($x.r.n)_B.log"
    $stopped = Select-String -Path "build\$($Tag)_$($x.r.n).out" -Pattern '^stop:' -Quiet
    "== $($x.r.n): $(if ($stopped) { 'pass lines seen' } else { 'ran to the limit (pass lines NOT seen)' }) at $([int]((Get-Date) - $t0).TotalSeconds) s"
  }
}
foreach ($r in $runs) {
  foreach ($g in 'A', 'B') {
    $l = Select-String -Path "build\$($Tag)_$($r.n)_$g.log" -Pattern 'coop: clean:' | Select-Object -Last 1
    $bad = Select-String -Path "build\$($Tag)_$($r.n)_$g.log" -Pattern 'coop: clean:.*(refused deaths [1-9]|not whole [1-9]|parts gone [1-9]|unreadable [1-9]|drops [1-9]|drops \d+/[1-9])' | Select-Object -First 1
    "$($r.n) $g : $(if ($bad) { 'NOT CLEAN: ' + $bad.Line.Substring(7) } elseif ($l) { 'clean' } else { 'no clean line' })"
  }
}
"suite: $([int]((Get-Date) - $t0).TotalSeconds) s"

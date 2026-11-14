# The entity sync's test suite: each scenario is one unattended session of two games on this PC (play_two.ps1 -Bench)
# and its report (ent_report.py); the reports are kept in build\suite\<name>.txt and a line per scenario is printed.
#   powershell -ExecutionPolicy Bypass -File tools\coop\ent_suite.ps1 [-Only glue-both,hand-over] [-Shots w20-45:2]
# Both games load the third save by themselves from the frozen saves, so every run starts from the same world. A
# scenario's script presses the buttons a player does (tools\coop\hand_*.txt); the scene (runtime\coop_ent_bench.c)
# places the Links, makes three fans, brings the picture's middle onto the fan to take and what is held next to its
# neighbour. A scenario passes when what it was meant to do happened ("expected") and every measured line passes.
#   drive       game 1 moves a fan by written velocities (no player does that): the following, the resting, a tip
#   hand        player 1 takes a fan with Ultrahand, carries it, holds it still in the air, lets it go
#   hand-over   player 2 takes a fan of player 1's: the hand-over of ownership, then as hand
#   glue        player 1 glues a fan to another
#   glue-cross  player 2 takes a fan of player 1's and glues it to another of player 1's: a glue across owners
#   glue-both   glue-cross, then player 1 glues the third fan onto that pair: a second glue by the other player
#   paused      hand-over while the owner has its inventory open: the take is asked of a paused game
#   autobuild   player 1 builds the first Autobuild schematic (a wing with three fans): previews, the confirm, a build of four
#   saved       each game starts with fans of its own that have no identity, as a save's: the host's two end in both games, the other's is gone
#   powered     autobuild, then game 1 switches the build's fans on: a build that moves by its own devices, and their state
param([string]$Only = "", [string]$Shots = "")
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root
$out = Join-Path $root 'build\suite'
New-Item -ItemType Directory -Force $out | Out-Null
$env:TOTK_COOP_FOLLOW_TRACE = '1'; $env:TOTK_COOP_CONTRAPTION = '1'; $env:TOTK_COOP_DEVICE_SYNC = '0'; $env:TOTK_COOP_GLUE = '0'; $env:TOTK_COOP_BONDS = '0'
$all = @(
    @{ name = 'drive';      bench = 'fans,drive';                                         seconds = 120; expect = '' },
    @{ name = 'hand';       bench = 'fans,aim1=0@22.7';                                   seconds = 88;  expect = 'hand1';               s1 = 'tools\coop\hand_a.txt' },
    @{ name = 'hand-over';  bench = 'fans,aim2=0@22.7';                                   seconds = 88;  expect = 'hand2';               s2 = 'tools\coop\hand_a.txt' },
    @{ name = 'glue';       bench = 'fans,aim1=0@22.7,join1=@25';                         seconds = 75;  expect = 'hand1,bonds=1';       s1 = 'tools\coop\hand_glue.txt' },
    @{ name = 'glue-cross'; bench = 'fans,aim2=0@22.7,join2=@25';                         seconds = 75;  expect = 'hand2,bonds=1';       s2 = 'tools\coop\hand_glue.txt' },
    @{ name = 'glue-both';  bench = 'fans,aim1=2@36.7,aim2=0@22.7,join2=@25,join1=@39.5'; seconds = 88;  expect = 'hand1,hand2,bonds=2'; s1 = 'tools\coop\hand_glue_late.txt'; s2 = 'tools\coop\hand_glue.txt' },
    @{ name = 'paused';     bench = 'fans,aim2=0@22.7';                                   seconds = 88;  expect = 'hand2';               s1 = 'tools\coop\hand_pause.txt'; s2 = 'tools\coop\hand_a.txt' },
    @{ name = 'autobuild';  bench = 'fans,nofans';                                        seconds = 75;  expect = 'ents=4,bonds=3';      s1 = 'tools\coop\hand_autobuild.txt' },
    @{ name = 'powered';    bench = 'fans,nofans,on=@38';                                 seconds = 85;  expect = 'ents=4,bonds=3,onseen'; s1 = 'tools\coop\hand_autobuild.txt' },
    @{ name = 'saved';      bench = 'fans,nofans,saved';                                  seconds = 60;  expect = 'ents=2' }
)
$want = if ($Only) { $Only -split ',' } else { $all | ForEach-Object { $_.name } }
$summary = @()
foreach ($s in $all | Where-Object { $want -contains $_.name }) {
    $a = @{ Bench = $s.bench; Seconds = $s.seconds; FrozenSaves = $true }
    if ($s.s1) { $a.Script1 = $s.s1 }
    if ($s.s2) { $a.Script2 = $s.s2 }
    if ($s.expect) { $a.Expect = $s.expect }
    if ($Shots) { $a.Shots = $Shots }
    & (Join-Path $PSScriptRoot 'play_two.ps1') @a | Out-Null
    $rep = Get-Content (Join-Path $root 'build\ent_report.txt')
    # what went wrong in either game, whatever the report says
    $trouble = foreach ($g in 'v143', 'v143play') {
        Select-String -Path (Join-Path $root "build\$g\logs\totk.log") -Pattern 'access violation|fatal|is full|ring lost [1-9]|still there after three' |
            Select-Object -Last 2 | ForEach-Object { "  TROUBLE $g`: " + $_.Line }
    }
    ($rep + $trouble) | Set-Content -Encoding utf8 (Join-Path $out "$($s.name).txt")
    foreach ($g in 'v143', 'v143play') { Copy-Item -Force (Join-Path $root "build\$g\logs\totk.log") (Join-Path $out "$($s.name)-$g.log") }
    $fails = @($rep | Where-Object { $_ -match '^\s+FAIL' })
    $line = "{0,-11} {1}" -f $s.name, $(if ($fails.Count -or $trouble) { "FAIL" } else { "pass" })
    $summary += $line
    $line
    $fails + $trouble
}
"-- summary"
$summary
$summary | Set-Content -Encoding utf8 (Join-Path $out 'summary.txt')

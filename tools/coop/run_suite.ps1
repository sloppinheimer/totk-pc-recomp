# The co-op pair tests in one go: each case runs tools\coop\run_pair.ps1 (two games through a local relay, scripted
# input, copies of the 1.4.3 profile) and passes when every pattern it expects is in the log it names and no game
# hit a FATAL. Prints one line per case and a total. About 2 minutes a case.
#   powershell -ExecutionPolicy Bypass -File tools\coop\run_suite.ps1 [-Only tour,fuse]
param([string]$Only = "")
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root
$A = 'build\v143\logs\totk.log'; $B = 'build\v143b\logs\totk.log'
# name, seconds, script A, script B, environment for both games, then what each log must have (A: / B: patterns)
$cases = @(
    @{n = 'tour'; s = 75; a = 'tour.txt'; b = 'idle_3rd.txt'; env = @{};
      want = @('A:joined as player', 'B:joined as player', 'A:Player \w+ in the world', 'B:Player \w+ in the world', 'B:slot 0 swings')},
    @{n = 'fuse'; s = 75; a = 'bow_fuse_3rd.txt'; b = 'idle_3rd.txt'; env = @{};
      want = @('A:my arrow 1 is fused with BombFruit', 'B:the copy of BombFruit \(2\) went off', 'B:the copy of BombFruit \(4\) went off')},
    @{n = 'equip'; s = 60; a = 'equip_draw_3rd.txt'; b = 'idle_3rd.txt'; env = @{};
      want = @("B:owner's gear slot 0 is Weapon_Sword_070")},
    @{n = 'pickup'; s = 50; a = 'idle_3rd.txt'; b = 'pickup_b_3rd.txt'; env = @{TOTK_COOP_TEST_PICKUP = '1'};
      want = @('B:was picked up here', 'A:picked up or destroyed my')}
)
$pass = 0; $fail = 0
foreach ($c in $cases) {
    if ($Only -and ($Only -split ',') -notcontains $c.n) { continue }
    foreach ($k in $c.env.Keys) { Set-Item "Env:$k" $c.env[$k] }
    powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'run_pair.ps1') -Seconds $c.s -ScriptA "tools\coop\$($c.a)" -ScriptB "tools\coop\$($c.b)" | Out-Null
    foreach ($k in $c.env.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
    $missing = @()
    foreach ($w in $c.want) {
        $log = if ($w.StartsWith('A:')) { $A } else { $B }
        if (-not (Select-String -Path $log -Pattern $w.Substring(2) -Quiet)) { $missing += $w }
    }
    $fatal = @(Select-String -Path $A, $B -Pattern 'FATAL' | ForEach-Object { "$(Split-Path -Leaf (Split-Path -Parent (Split-Path -Parent $_.Path))): $($_.Line)" })
    if (!$missing -and !$fatal) { $pass++; "PASS $($c.n)" }
    else { $fail++; "FAIL $($c.n)"; $missing | ForEach-Object { "  missing $_" }; $fatal | ForEach-Object { "  $_" } }
}
"$pass passed, $fail failed"

# The co-op pair tests in one go: each case runs tools\coop\run_pair.ps1 (two games through a local relay, scripted
# input, copies of the 1.4.3 profile) and passes when every pattern it expects is in the log it names, no game hit a
# FATAL, and the sync audit (tools\coop\sync_audit.py on both games' logs\audit.jsonl) finds nothing that does not
# match. Prints one line per case (the audit's report under a failed one) and a total. About 2 minutes a case.
#   powershell -ExecutionPolicy Bypass -File tools\coop\run_suite.ps1 [-Only tour,fuse] [-BDir build\v143c]
#   -BDir: game B's folder (run_pair.ps1 -BDir), for when a stuck game holds build\v143b's exe
param([string]$Only = "", [string]$BDir = "build\v143b")
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root
$A = 'build\v143\logs\totk.log'; $B = Join-Path $BDir 'logs\totk.log'
# name, seconds, script A, script B, environment for both games, then what each log must have (A: / B: patterns)
$cases = @(
    @{n = 'tour'; s = 80; a = 'tour.txt'; b = 'idle_3rd.txt'; env = @{};
      want = @('A:joined as player', 'B:joined as player', 'A:Player \w+ in the world', 'B:Player \w+ in the world', 'B:slot 0 swings')},
    @{n = 'fuse'; s = 80; a = 'bow_fuse_3rd.txt'; b = 'idle_3rd.txt'; env = @{};
      want = @('A:my arrow 1 is fused with BombFruit', 'B:the copy of BombFruit \(2\) went off', 'B:the copy of BombFruit \(4\) went off')},
    @{n = 'equip'; s = 65; a = 'equip_draw_3rd.txt'; b = 'idle_3rd.txt'; env = @{};
      want = @("B:owner's gear slot 0 is Weapon_Sword_070")},
    @{n = 'hands'; s = 75; a = 'equip_bow_3rd.txt'; b = 'idle_3rd.txt'; env = @{};
      want = @("B:owner's gear slot 0 is Weapon_")},
    @{n = 'pickup'; s = 55; a = 'idle_3rd.txt'; b = 'pickup_b_3rd.txt'; env = @{TOTK_COOP_TEST_PICKUP = '1'};
      want = @('B:was picked up here', 'A:picked up or destroyed my')}
)
$pass = 0; $fail = 0
foreach ($c in $cases) {
    if ($Only -and ($Only -split ',') -notcontains $c.n) { continue }
    foreach ($k in $c.env.Keys) { Set-Item "Env:$k" $c.env[$k] }
    # (the last case's logs and audits go first: a game that did not start left them, and they were judged again)
    Remove-Item $A, $B, 'build\v143\logs\audit.jsonl', (Join-Path $BDir 'logs\audit.jsonl') -ErrorAction SilentlyContinue
    powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'run_pair.ps1') -Seconds $c.s -ScriptA "tools\coop\$($c.a)" -ScriptB "tools\coop\$($c.b)" -BDir $BDir | Out-Null
    $pair_ok = $LASTEXITCODE -eq 0
    foreach ($k in $c.env.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
    $missing = @()
    foreach ($w in $c.want) {
        $log = if ($w.StartsWith('A:')) { $A } else { $B }
        if (-not (Select-String -Path $log -Pattern $w.Substring(2) -Quiet)) { $missing += $w }
    }
    # each case's logs and audit snapshots kept in build\suite\<case>\A and \B (the next case writes over the games')
    foreach ($g in @(@{d = 'v143'; k = 'A'}, @{d = (Split-Path -Leaf $BDir); k = 'B'})) {
        $keep = Join-Path $root "build\suite\$($c.n)\$($g.k)"
        New-Item -ItemType Directory -Force $keep | Out-Null
        Copy-Item "build\$($g.d)\logs\totk.log", "build\$($g.d)\logs\audit.jsonl" $keep -ErrorAction SilentlyContinue
    }
    $fatal = @(Select-String -Path $A, $B -Pattern 'FATAL' | ForEach-Object { "$(Split-Path -Leaf (Split-Path -Parent (Split-Path -Parent $_.Path))): $($_.Line)" })
    $audit = & (Join-Path $root '.venv\Scripts\python.exe') (Join-Path $PSScriptRoot 'sync_audit.py') 'build\v143\logs\audit.jsonl' (Join-Path $BDir 'logs\audit.jsonl')
    $audit_ok = $LASTEXITCODE -eq 0
    if ($pair_ok -and !$missing -and !$fatal -and $audit_ok) { $pass++; "PASS $($c.n)  ($($audit[0]))" }
    else {
        $fail++; "FAIL $($c.n)"; if (!$pair_ok) { "  run_pair.ps1 failed" }; $missing | ForEach-Object { "  missing $_" }; $fatal | ForEach-Object { "  $_" }
        if (!$audit_ok) { $audit | ForEach-Object { "  audit: $_" } }
    }
}
"$pass passed, $fail failed"

# Two games on this PC in one co-op session through a local relay (server/coop_relay.py): game A plays -ScriptA, game B
# plays -ScriptB with no real input. Each runs on a copy of the 1.4.3 profile and logs to its own folder
# (build\v143 and build\v143b, the second made of links to the first's files). Prints both games' co-op lines.
#   powershell -ExecutionPolicy Bypass -File tools\coop\run_pair.ps1 [-Seconds 90]
param([int]$Seconds = 90, [string]$ScriptA = "tools\coop\walk.txt", [string]$ScriptB = "tools\coop\idle.txt", [string]$FakeGearA = "")
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root
Get-Process totk -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 2

# build\v143b: hard links to build\v143's files, junctions to its folders, a logs folder of its own.
$a = Join-Path $root 'build\v143'; $b = Join-Path $root 'build\v143b'
if (-not (Test-Path $b)) { New-Item -ItemType Directory $b | Out-Null }
foreach ($f in Get-ChildItem $a -Force) {
    $t = Join-Path $b $f.Name
    if ($f.Name -in 'logs', 'obj') { continue }
    if (Test-Path $t) { if ($f.PSIsContainer) { continue }; Remove-Item -Force $t }
    if ($f.PSIsContainer) { New-Item -ItemType Junction $t -Target $f.FullName | Out-Null }
    else { New-Item -ItemType HardLink $t -Target $f.FullName | Out-Null }
}

$relay = Start-Process -PassThru -WindowStyle Hidden (Join-Path $root '.venv\Scripts\python.exe') `
    -ArgumentList "server\coop_relay.py" -RedirectStandardOutput (Join-Path $root 'build\relay.log')
$games = @()
foreach ($g in @(@{dir = $a; prof = 'build\prof-coop-a'; script = $ScriptA; name = 'A'; input = $null},
                 @{dir = $b; prof = 'build\prof-coop-b'; script = $ScriptB; name = 'B'; input = 'none'})) {
    $p = Join-Path $root $g.prof
    if (Test-Path $p) { Remove-Item -Recurse -Force $p }
    Copy-Item -Recurse (Join-Path $root 'build\profile-v143') $p
    $env:TOTK_PROFILE_DIR = $p
    $env:TOTK_INPUT_SCRIPT = (Resolve-Path $g.script).Path
    $env:TOTK_ASSUME_FOCUS = '1'; $env:TOTK_FULLSCREEN = '0'; $env:TOTK_WINDOW_SIZE = '960x540'
    $env:TOTK_COOP_RELAY = '127.0.0.1'; $env:TOTK_COOP_NAME = $g.name; $env:TOTK_COOP_P2 = 'off'
    if ($g.input) { $env:TOTK_INPUT = $g.input } else { Remove-Item Env:TOTK_INPUT -ErrorAction SilentlyContinue }
    if ($g.name -eq 'A' -and $FakeGearA) { $env:TOTK_COOP_FAKEGEAR = $FakeGearA } else { Remove-Item Env:TOTK_COOP_FAKEGEAR -ErrorAction SilentlyContinue }
    $games += Start-Process (Join-Path $g.dir 'totk.exe') -WorkingDirectory $g.dir -PassThru
    Start-Sleep 3
}
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline -and ($games | Where-Object { -not $_.HasExited })) { Start-Sleep 1 }
$games | Where-Object { -not $_.HasExited } | Stop-Process -Force
Stop-Process -Id $relay.Id -Force -ErrorAction SilentlyContinue
Start-Sleep 1
foreach ($d in $a, $b) {
    "=== $d"
    Select-String -Path (Join-Path $d 'logs\totk.log') -Pattern 'coop net|coop: (slot|player slot|remote)|FATAL' | Select-Object -ExpandProperty Line
}
"=== relay"; Get-Content (Join-Path $root 'build\relay.log')

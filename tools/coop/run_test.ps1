# Unattended co-op test run: a copy of the 1.4.3 profile (the real one is never written), scripted title-to-save input,
# Player#2 spawned automatically, a small window. Stops the game after -Seconds and prints the coop log lines.
#   powershell -ExecutionPolicy Bypass -File tools\coop\run_test.ps1 [-Seconds 90] [-Script tools\input\load_save.txt]
param([int]$Seconds = 90, [string]$Script = "tools\input\load_save.txt", [int]$AutoSpawn = 150, [string]$Watch = "0", [string]$Prof = "0", [string]$Blind = "0", [string]$P2 = "", [string]$CtrlDump = "0", [string]$CamAt = "")
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root
Get-Process totk -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 2
$profDir = Join-Path $root 'build\prof-coop'
if (Test-Path $profDir) { Remove-Item -Recurse -Force $profDir }
Copy-Item -Recurse (Join-Path $root 'build\profile-v143') $profDir
$env:TOTK_PROFILE_DIR = $profDir
$env:TOTK_INPUT_SCRIPT = (Resolve-Path $Script).Path
$env:TOTK_ASSUME_FOCUS = '1'
$env:TOTK_FULLSCREEN = '0'
$env:TOTK_WINDOW_SIZE = '1280x720'
$env:TOTK_COOP_AUTOSPAWN = "$AutoSpawn"
$env:TOTK_COOP_WATCH = $Watch
$env:TOTK_COOP_PROF = $Prof
$env:TOTK_COOP_BLIND = $Blind
$env:TOTK_COOP_CTRLDUMP = $CtrlDump
if ($CamAt) { $env:TOTK_COOP_CAMAT = $CamAt } else { Remove-Item Env:TOTK_COOP_CAMAT -ErrorAction SilentlyContinue }
if ($P2) { $env:TOTK_COOP_P2 = $P2 } else { Remove-Item Env:TOTK_COOP_P2 -ErrorAction SilentlyContinue }
. (Join-Path $PSScriptRoot 'quiet.ps1')   # no console, no mouse capture, the window stays behind
$p = Start-Quiet (Join-Path $root 'build\v143\totk.exe') '' (Join-Path $root 'build\v143')
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline -and -not $p.HasExited) { Start-Sleep 1 }
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep 1
Select-String -Path build\v143\logs\totk.log -Pattern 'coop:|FATAL' | Select-Object -ExpandProperty Line

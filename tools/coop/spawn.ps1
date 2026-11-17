# Makes an actor beside game 1's Link in a running co-op session (tools\coop\play_two.ps1 gives game 1 the file to
# watch, TOTK_COOP_SPAWN_FILE). The game makes it within a quarter of a second and writes
# "coop: spawn file: request <n>: <actor>" in build\v143\logs\totk.log; the other game gets its copy by the sync.
#   powershell -ExecutionPolicy Bypass -File tools\coop\spawn.ps1 Enemy_Chuchu_Middle
param([Parameter(Mandatory = $true)][string]$Actor)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$file = Join-Path $root 'build\spawn_cmd.txt'
$n = 0
if (Test-Path $file) { $t = (Get-Content $file -Raw); if ($t -match '^(\d+)') { $n = [int]$Matches[1] } }
$n++
[System.IO.File]::WriteAllText($file, "$n $Actor`n", (New-Object System.Text.ASCIIEncoding))
"asked for $Actor (request $n)"

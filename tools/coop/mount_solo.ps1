# The mount test in one game with no co-op peer (no relay, no Player#2): does a test-spawned horse take a mount at all,
# apart from the co-op sync? Same save, horse, place, puts and presses as run_suite.ps1's "mount" case: the horse is
# spawned at 26 s, this Link is put by its saddle at 30, 32 and 34 s (TOTK_COOP_TEST_MOUNT_PUT without a player id:
# alone this game has none), tools\coop\mount_horse_3rd.txt presses A after each and rides forward from 37 s.
# coop_world_tick runs coop_mounts_tick alone too (coop_world.c), so the TOTK_COOP_TEST_MOUNT lines and
# "coop: mounts: this Link rides" show here as in a pair. Prints the log's coop lines (run_test.ps1).
# -Horse: the actor spawned (any GameRomHorse* or Animal_Horse* from the RomFS index, all synced as horses). 75 s of
# wall time covers the boot and the presses to 35.6 s (60 s ended at 32 s world, 2026-11-07).
#   powershell -ExecutionPolicy Bypass -File tools\coop\mount_solo.ps1 [-Seconds 75] [-Horse GameRomHorse01]
param([int]$Seconds = 75, [string]$Horse = 'GameRomHorse01')
$ErrorActionPreference = 'Stop'
$env:TOTK_COOP_MOUNTS = '1'
$env:TOTK_COOP_TEST_MOUNT = '1'
$env:TOTK_COOP_TEST_MOUNT_PUT = '30'
$env:TOTK_COOP_TEST_SPAWN = "$Horse@26"
$env:TOTK_COOP_TEST_SPAWN_AT = '41.6,1511.0,1504.9'
try {
    & (Join-Path $PSScriptRoot 'run_test.ps1') -Seconds $Seconds -Script tools\coop\mount_horse_3rd.txt -AutoSpawn 0 |
        Select-String -Pattern 'mounts:|test: mount:|Horse|spawn ' | Select-Object -ExpandProperty Line
} finally {
    foreach ($k in 'TOTK_COOP_MOUNTS', 'TOTK_COOP_TEST_MOUNT', 'TOTK_COOP_TEST_MOUNT_PUT', 'TOTK_COOP_TEST_SPAWN', 'TOTK_COOP_TEST_SPAWN_AT') {
        Remove-Item "Env:$k" -ErrorAction SilentlyContinue
    }
}

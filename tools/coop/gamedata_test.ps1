# Shared world progress (runtime/coop_gamedata.c): two games in one session with TOTK_COOP_SHARE_WORLD=1, both on
# copies of the profile's first save, standing still (tools/coop/gamedata.txt). Game A, from 25 s of world time, sets a
# shared Bool64bitKey that reads 0 to 1 and the bool IsGet.Item_Fruit_A to the other value through its own game data
# queue (TOTK_COOP_TEST_GAMEDATA=A:25), and puts both back 10 s later. Prints both games' "coop: gamedata" lines.
#   powershell -ExecutionPolicy Bypass -File tools\coop\gamedata_test.ps1 [-Seconds 80]
#
# A passing run shows (K the key, T its reset type, X the bool's new value, n player ids; the order may vary):
#   both games: coop: gamedata: sharing world progress (manager ...): 130375 Bool64bitKey keys, 126838 shared by reset
#               type and 3537 the player's; ...
#               coop: gamedata: Bool64bitKey reset types here: 0x150 x100751, 0x58 x22312, 0x50 x3537, 0x5a x1846,
#               0xd0 x982, 0x52 x947 (in any order; the same counts as GameDataList: the in-memory field is the
#               reset type)
#   A:  coop: gamedata: test: key K (index ..., reset type T) set to 1 in this game (it read 0)
#       coop: gamedata: key K = 1 in this game (reset type T), shared
#       coop: gamedata: test: bool IsGet.Item_Fruit_A (b06a5a66) set to X in this game (it read !X)
#       coop: gamedata: bool b06a5a66 = X in this game, shared
#       coop: gamedata: test: key K set back to 0 in this game (it read 1)      <- A's own calc wrote it
#       coop: gamedata: key K = 0 in this game (reset type T), shared
#       coop: gamedata: test: bool b06a5a66 set back to !X in this game (it read X)
#       coop: gamedata: bool b06a5a66 = !X in this game, shared
#   B:  coop: gamedata: player n's key K = 1 applied here (it read 0)
#       coop: gamedata: player n's key K now reads 1 here (as sent)
#       coop: gamedata: player n's bool b06a5a66 = X applied here (it read !X)   (only if B's save had !X: same save, so yes)
#       coop: gamedata: player n's bool b06a5a66 now reads X here (as sent)
#       ... and the same two pairs for the values put back (= 0, = !X)
#   the 30 s stats line in B: "... received: ... applied ..." with applied >= 4 and "already so" >= 4 (the resends).
# Failing signs: no "sharing world progress" line (the calc hook did not run, or no other player was seen); "NOT as
# sent"; B's "unknown here" counting up (the two games' tables differ).
# Other "= ... in this game, shared" lines are the games' own changes while standing (time of day flags and the like).
param([int]$Seconds = 80)
$ErrorActionPreference = 'Stop'
$env:TOTK_COOP_SHARE_WORLD = '1'
$env:TOTK_COOP_TEST_GAMEDATA = 'A:25'
try {
    & (Join-Path $PSScriptRoot 'run_pair.ps1') -Seconds $Seconds -ScriptA tools\coop\gamedata.txt -ScriptB tools\coop\gamedata.txt | Out-Null
} finally {
    Remove-Item Env:TOTK_COOP_SHARE_WORLD, Env:TOTK_COOP_TEST_GAMEDATA -ErrorAction SilentlyContinue
}
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
foreach ($g in @(@{n = 'A'; d = 'build\v143'}, @{n = 'B'; d = 'build\v143b'})) {
    "=== $($g.n)"
    Select-String -Path (Join-Path $root "$($g.d)\logs\totk.log") -Pattern 'coop: gamedata|coop net|FATAL' | Select-Object -ExpandProperty Line
}

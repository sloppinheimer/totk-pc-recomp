# Shared world progress (runtime/coop_gamedata.c): two games in one session with TOTK_COOP_SHARE_WORLD=1, both on
# copies of the profile's first save, standing still (tools/coop/gamedata.txt). No map action: game A, from 25 s of
# world time, changes through its own game data queues (TOTK_COOP_TEST_GAMEDATA=A:25), 2 s apart: a shared Bool64bitKey
# that reads 0 (to 1), the bool IsGet.Item_Fruit_A, a shared Int (+1), the last shrine not cleared (its DungeonState to
# Clear) and the Float SageOfGerudo_SliderBlockPos1 (+1.5); it puts all five back from 65 s. Game B joins the sharing
# late, at 50 s of its world time (TOTK_COOP_TEST_LATE=B:50: until then it drops what it is sent), so it gets A's
# changes from A's session log (catch-up), works out its blessings for the shrine A cleared, and then gets the put-backs
# live. Prints both games' "coop: gamedata" lines.
#   powershell -ExecutionPolicy Bypass -File tools\coop\gamedata_test.ps1 [-Seconds 130] [-BDir build\v143c]
#
# A passing run shows (K the key, T its reset type, X the bool's new value, I and E the Int's and the shrine's hashes,
# n player ids; the order may vary; a value a game applies is printed in hex):
#   both: coop: gamedata: GameDataList: in the progress save 20478 of 21414 bools, 5518 of 5675 Ints, 1645 of 1696 Enums,
#           34 of 56 Floats
#         coop: gamedata: the player's entries by family: ... names from ... actors and the cooked dishes (... ms)
#         coop: gamedata: sharing world progress (manager ...): 130375 Bool64bitKey keys, 126838 shared by reset type and
#           3537 the player's; 21414 bools, 19404 shared; ...
#         coop: gamedata: Bool64bitKey reset types here: 0x150 x100751, 0x58 x22312, 0x50 x3537, 0x5a x1846, 0xd0 x982,
#           0x52 x947 (in any order)
#         coop: gamedata: Int: 5675 entries, 3383 shared, 2135 the player's, 157 not in the progress save; ...
#         coop: gamedata: Enum: 1696 entries, 1110 shared, 535 the player's, 51 not in the progress save; ...
#         coop: gamedata: Float: 56 entries, 4 shared, 30 the player's, 22 not in the progress save; ...
#           (the counts worked out offline from GameDataList 1.4.0 with the same rule: equal counts show that the game's
#           tables are what the code takes them for)
#   A:  coop: gamedata: hello sent (1 other player here)
#       coop: gamedata: test: key K (index ..., reset type T) set to 1 in this game (it read 0)   + "key K = 1 ..., shared"
#       coop: gamedata: test: bool IsGet.Item_Fruit_A (b06a5a66) set to X ...                     + "bool b06a5a66 = X ..., shared"
#       coop: gamedata: test: int I set to v+1 in this game (it read v)                            + "int I = v+1 in this game, shared"
#       coop: gamedata: test: enum DungeonState.DungeonNNN (E) set to Clear (62965740) ...         + "enum E = 62965740 ..., shared"
#       coop: gamedata: test: float SageOfGerudo_SliderBlockPos1 (42a21ad2) set to ...            + "float 42a21ad2 = ..., shared"
#       coop: gamedata: player n says hello: catching them up on the N changes of this session    (at B's 50 s; N >= 5)
#       coop: gamedata: catch-up for player n: N changes sent (pass 1 of 2), then the same with (pass 2 of 2)
#       coop: gamedata: test: key K set back to 0 ..., bool b06a5a66 set back ..., int I set back ..., enum E set back ...,
#         float 42a21ad2 set back ... (from 65 s), each followed by its "... in this game, shared" line
#   B:  coop: gamedata: hello sent (1 other player here) (joined late, the test)
#       coop: gamedata: player n's key K = 0x1 applied here (catch-up) (it read 0), the same for bool b06a5a66, int I,
#         enum E (= 0x62965740) and float 42a21ad2; each followed by "player n's ... now reads ... here (as sent)"
#       coop: gamedata: blessings: S of 152 shrines cleared here, H hearts and V stamina vessels bought, so W; the pouch
#         had P: set to W (key item slot ...)      when the save's blessings matched the formula before (W = P + 1)
#         or "...; the pouch has P: left so"       when the save holds more blessings than the formula gives
#       then, live from A's 65 s: "player n's key K = 0 applied here (it read 0x1)" and the same for the other four,
#         each with its "now reads ... (as sent)"
#   B's 30 s stats line: "... received: ... applied, ... by catch-up; dropped: D not joined yet, ..." with D > 0 (A's
#   live packets before 50 s) and applied >= 10.
# Failing signs: no "sharing world progress" line (the calc hook did not run, or no other player was seen);
# "GameDataList ... not read" (numbers are then not shared); "NOT as sent"; B's "unknown here" counting up (the two
# games' tables differ); no "says hello" line in A; "blessings: not worked out".
# Other "... in this game, shared" lines are the games' own changes while standing (time of day flags and the like).
param([int]$Seconds = 130, [string]$BDir = "build\v143b")
$ErrorActionPreference = 'Stop'
$env:TOTK_COOP_SHARE_WORLD = '1'
$env:TOTK_COOP_TEST_GAMEDATA = 'A:25'
$env:TOTK_COOP_TEST_LATE = 'B:50'
try {
    & (Join-Path $PSScriptRoot 'run_pair.ps1') -Seconds $Seconds -ScriptA tools\coop\gamedata.txt -ScriptB tools\coop\gamedata.txt -BDir $BDir | Out-Null
} finally {
    Remove-Item Env:TOTK_COOP_SHARE_WORLD, Env:TOTK_COOP_TEST_GAMEDATA, Env:TOTK_COOP_TEST_LATE -ErrorAction SilentlyContinue
}
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
foreach ($g in @(@{n = 'A'; d = 'build\v143'}, @{n = 'B'; d = $BDir})) {
    "=== $($g.n)"
    Select-String -Path (Join-Path $root "$($g.d)\logs\totk.log") -Pattern 'coop: gamedata|coop net|FATAL' | Select-Object -ExpandProperty Line
}

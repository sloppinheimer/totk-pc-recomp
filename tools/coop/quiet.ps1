# Dot-sourced by the co-op test scripts: programs started out of the user's way. No console window at all
# (CREATE_NO_WINDOW; hiding one afterwards fails under Windows Terminal), the game window behind the user's windows
# without taking the focus (TOTK_BACKGROUND_WINDOW), no mouse camera and no cursor capture (TOTK_MOUSE_SENS=0), and
# the game keeps running and reading input while it is not focused (TOTK_ASSUME_FOCUS). The child gets this script's
# environment as it is when Start-Quiet is called.
$env:TOTK_BACKGROUND_WINDOW = '1'
$env:TOTK_MOUSE_SENS = '0'
$env:TOTK_ASSUME_FOCUS = '1'
$env:TOTK_COOP_HIDE_CONSOLE = '1'
function Start-Quiet([string]$exe, [string]$arguments, [string]$dir) {
    $si = New-Object System.Diagnostics.ProcessStartInfo $exe, $arguments
    $si.UseShellExecute = $false; $si.CreateNoWindow = $true; $si.WorkingDirectory = $dir
    [System.Diagnostics.Process]::Start($si)
}

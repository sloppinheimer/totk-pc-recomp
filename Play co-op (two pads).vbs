' Co-op on one PC, one game per gamepad (tools\coop\play_two.ps1), with no window of its own.
Set sh = CreateObject("WScript.Shell")
dir = CreateObject("Scripting.FileSystemObject").GetParentFolderName(WScript.ScriptFullName)
sh.Run "powershell -NoProfile -ExecutionPolicy Bypass -File """ & dir & "\tools\coop\play_two.ps1""", 0, False

' Co-op on one PC as "Play co-op (two pads).vbs", with the TotK Online ways that are still opt-in (the PlayerPuppet
' stand-in, shared world progress): tools\coop\play_two.ps1 -Online.
Set sh = CreateObject("WScript.Shell")
dir = CreateObject("Scripting.FileSystemObject").GetParentFolderName(WScript.ScriptFullName)
sh.Run "powershell -NoProfile -ExecutionPolicy Bypass -File """ & dir & "\tools\coop\play_two.ps1"" -Online", 0, False

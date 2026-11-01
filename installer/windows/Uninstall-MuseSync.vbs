Option Explicit
Dim fs, script, args, shell, env
Set fs = CreateObject("Scripting.FileSystemObject")
script = fs.BuildPath(fs.GetParentFolderName(WScript.ScriptFullName), "Uninstall-MuseSync.ps1")
If Not fs.FileExists(script) Then
    MsgBox "Uninstall-MuseSync.ps1 is missing. Keep both uninstall files together.", 16, "MuseSync"
    WScript.Quit 1
End If
args = "-NoProfile -ExecutionPolicy Bypass -File """ & script & """"
Set shell = CreateObject("Shell.Application")
Set env = CreateObject("WScript.Shell")
On Error Resume Next
shell.ShellExecute fs.BuildPath(env.ExpandEnvironmentStrings("%SystemRoot%"), "System32\WindowsPowerShell\v1.0\powershell.exe"), args, "", "runas", 0
If Err.Number <> 0 Then MsgBox "Uninstall was cancelled or could not start.", 48, "MuseSync"

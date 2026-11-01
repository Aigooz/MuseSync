Option Explicit
Dim fs, shell, script
Set fs = CreateObject("Scripting.FileSystemObject")
Set shell = CreateObject("WScript.Shell")
script = fs.BuildPath(fs.GetParentFolderName(WScript.ScriptFullName), "installer\windows\Uninstall-MuseSync.vbs")
If Not fs.FileExists(script) Then
    MsgBox "MuseSync uninstall files are missing.", 16, "MuseSync"
    WScript.Quit 1
End If
shell.Run "wscript.exe """ & script & """", 0, False

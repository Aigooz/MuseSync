; Build with: ISCC /DMyBuildDir=M:\build-musetone-live setup-musetone.iss
#ifndef MyBuildDir
  #define MyBuildDir "..\..\build"
#endif
#ifndef MyOutputDir
  #define MyOutputDir MyBuildDir + "\setup-out"
#endif
#define MySourceDir "..\.."
#define MyAppVersion ReadIni(AddBackslash(SourcePath) + "release.ini", "Release", "Version", "")
#if MyAppVersion == ""
  #error Missing release.ini version
#endif

[Setup]
#ifdef SmokeTest
AppId=MuseToneInstallerSmoke
#else
AppId={{6A1D47D5-4A18-4B5B-9A09-1C5E8A9B7D21}
#endif
AppName=MuseTone
AppVersion={#MyAppVersion}
AppVerName=MuseTone {#MyAppVersion}
AppPublisher=MuseTone contributors
DefaultDirName={autopf}\MuseTone
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputDir={#MyOutputDir}
OutputBaseFilename=MuseTone-Setup-{#MyAppVersion}-Windows-x64
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
#ifdef SmokeTest
PrivilegesRequired=lowest
#else
PrivilegesRequired=admin
#endif
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
LicenseFile={#MySourceDir}\LICENSE
InfoBeforeFile={#MySourceDir}\installer\windows\INSTALL.txt
UninstallDisplayName=MuseTone {#MyAppVersion}
UninstallDisplayIcon={code:Vst3Root}\MuseTone.vst3\PlugIn.ico
CloseApplications=no
CloseApplicationsFilter=*.vst3,*.qml,*.js
RestartApplications=no
SetupLogging=yes

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Dirs]
; Qt Settings writes its snapshot atomically beside the QML plugin.
Name: "{code:MuseScoreRoot}\plugins\MuseToneSync"; Permissions: users-modify

[Files]
Source: "{#MyBuildDir}\VST3\Release\MuseTone.vst3\Contents\x86_64-win\MuseTone.vst3"; DestDir: "{code:Vst3Root}\MuseTone.vst3\Contents\x86_64-win"; Flags: ignoreversion
Source: "{#MyBuildDir}\VST3\Release\MuseTone.vst3\Contents\Resources\moduleinfo.json"; DestDir: "{code:Vst3Root}\MuseTone.vst3\Contents\Resources"; Flags: ignoreversion
Source: "{#MyBuildDir}\VST3\Release\MuseTone.vst3\PlugIn.ico"; DestDir: "{code:Vst3Root}\MuseTone.vst3"; Flags: ignoreversion
Source: "{#MyBuildDir}\VST3\Release\MuseTone.vst3\desktop.ini"; DestDir: "{code:Vst3Root}\MuseTone.vst3"; Flags: ignoreversion
Source: "{#MySourceDir}\companion\MuseToneSync\MuseToneSync.qml"; DestDir: "{code:MuseScoreRoot}\plugins\MuseToneSync"; Flags: ignoreversion
Source: "{#MySourceDir}\companion\MuseToneSync\ScoreSnapshot.js"; DestDir: "{code:MuseScoreRoot}\plugins\MuseToneSync"; Flags: ignoreversion
Source: "{#MySourceDir}\LICENSE"; DestDir: "{app}"
Source: "{#MySourceDir}\installer\windows\INSTALL.txt"; DestDir: "{app}"
Source: "{#MySourceDir}\docs\musetone-windows.md"; DestDir: "{app}"
Source: "{#MySourceDir}\CHANGELOG.md"; DestDir: "{app}"
Source: "{#MySourceDir}\source\third_party\signalsmith-stretch\LICENSE.txt"; DestDir: "{app}\licenses"; DestName: "signalsmith-stretch.txt"
Source: "{#MySourceDir}\source\third_party\signalsmith-stretch\dsp\LICENSE.txt"; DestDir: "{app}\licenses"; DestName: "signalsmith-dsp.txt"
Source: "{#MyBuildDir}\release-notices\*"; DestDir: "{app}\licenses"; Flags: recursesubdirs createallsubdirs
Source: "{#MySourceDir}\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"

[Registry]
#ifdef SmokeTest
Root: HKCU; Subkey: "Software\MuseToneInstallerSmoke"; ValueType: string; ValueName: "MuseScoreDir"; ValueData: "{code:MuseScoreRoot}"; Flags: uninsdeletevalue uninsdeletekeyifempty
#else
Root: HKLM; Subkey: "Software\MuseTone"; ValueType: string; ValueName: "MuseScoreDir"; ValueData: "{code:MuseScoreRoot}"; Flags: uninsdeletevalue uninsdeletekeyifempty
#endif

[Run]
Filename: "{app}\INSTALL.txt"; Description: "Open installation and startup instructions"; Flags: postinstall shellexec nowait skipifsilent

[Code]
var
  ScorePage: TInputDirWizardPage;

function Vst3Root(Param: String): String;
begin
  Result := RemoveBackslash(ExpandConstant('{param:VST3DIR|{commoncf64}\VST3}'));
end;

function MuseScoreRoot(Param: String): String;
begin
  Result := RemoveBackslash(ScorePage.Values[0]);
end;

function IsMuseScoreDir(Path: String): Boolean;
begin
  Result := FileExists(AddBackslash(Path) + 'bin\MuseScore4.exe');
end;

function DetectMuseScore(): String;
var
  Keys: TArrayOfString;
  I: Integer;
  Key, Path, Name: String;
begin
  Result := ExpandConstant('{param:MUSESCOREDIR|}');
  if Result <> '' then exit;
  if RegQueryStringValue(HKLM, 'Software\MuseTone', 'MuseScoreDir', Path) and IsMuseScoreDir(Path) then
  begin
    Result := Path;
    exit;
  end;
  if RegGetSubkeyNames(HKLM, 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall', Keys) then
    for I := 0 to GetArrayLength(Keys) - 1 do
    begin
      Key := 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\' + Keys[I];
      if RegQueryStringValue(HKLM, Key, 'DisplayName', Name) and (Pos('MuseScore', Name) > 0) then
        if RegQueryStringValue(HKLM, Key, 'InstallLocation', Path) and IsMuseScoreDir(Path) then
        begin
          Result := Path;
          exit;
        end;
    end;
  Path := ExpandConstant('{autopf}\MuseScore 4');
  if IsMuseScoreDir(Path) then Result := Path;
end;

procedure InitializeWizard();
begin
  ScorePage := CreateInputDirPage(wpLicense, 'MuseScore installation',
    'Choose your MuseScore 4 installation folder',
    'MuseTone Sync will be installed into this application''s plugins folder. Select the folder containing bin\MuseScore4.exe, not your scores folder.',
    False, '');
  ScorePage.Add('MuseScore 4 folder:');
  ScorePage.Values[0] := DetectMuseScore();
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = ScorePage.ID then
    if not IsMuseScoreDir(ScorePage.Values[0]) then
    begin
      MsgBox('Select a valid MuseScore 4 installation folder containing bin\MuseScore4.exe.', mbError, MB_OK);
      Result := False;
    end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not IsMuseScoreDir(MuseScoreRoot('')) then
    Result := 'MuseScore 4 was not found. Choose its installation folder or pass /MUSESCOREDIR="C:\Program Files\MuseScore 4".';
end;

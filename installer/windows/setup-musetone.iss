; Build with: ISCC /DMyBuildDir=M:\build-musetone-live setup-musetone.iss
#ifndef MyBuildDir
  #define MyBuildDir "..\..\build"
#endif
#ifndef MyOutputDir
  #define MyOutputDir MyBuildDir + "\setup-out"
#endif
#define MySourceDir "..\.."
#define MyAppVersion ReadIni(AddBackslash(SourcePath) + "release.ini", "Release", "Version", "")
#ifdef SmokeTest
  #ifdef MySmokeVersion
    #undef MyAppVersion
    #define MyAppVersion MySmokeVersion
  #endif
#endif
#if MyAppVersion == ""
  #error Missing release.ini version
#endif

[Setup]
#ifdef SmokeTest
AppId=MuseToneInstallerSmoke
#else
AppId={{6A1D47D5-4A18-4B5B-9A09-1C5E8A9B7D21}
#endif
AppName=MuseSync
AppVersion={#MyAppVersion}
AppVerName=MuseSync {#MyAppVersion}
AppPublisher=MuseSync contributors
AppPublisherURL=https://github.com/Aigooz/MuseSync
DefaultDirName={autopf}\MuseTone
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputDir={#MyOutputDir}
OutputBaseFilename=MuseSync-Setup-{#MyAppVersion}-Windows-x64
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
UninstallDisplayName=MuseSync {#MyAppVersion}
UninstallDisplayIcon={code:Vst3Root}\MuseSync.vst3\PlugIn.ico
CloseApplications=no
CloseApplicationsFilter=*.vst3,*.qml,*.js
RestartApplications=no
SetupLogging=yes

[Languages]
Name: "zhcn"; MessagesFile: "compiler:Default.isl,ChineseSimplified.isl"

[Dirs]
; Qt Settings writes its snapshot atomically beside the QML plugin.
Name: "{code:MuseScoreRoot}\plugins\MuseToneSync"; Permissions: users-modify

[Files]
Source: "{#MyBuildDir}\VST3\Release\MuseSync.vst3\Contents\x86_64-win\MuseSync.vst3"; DestDir: "{code:Vst3Root}\MuseSync.vst3\Contents\x86_64-win"; Flags: ignoreversion
Source: "{#MyBuildDir}\VST3\Release\MuseSync.vst3\Contents\Resources\moduleinfo.json"; DestDir: "{code:Vst3Root}\MuseSync.vst3\Contents\Resources"; Flags: ignoreversion
Source: "{#MyBuildDir}\VST3\Release\MuseSync.vst3\PlugIn.ico"; DestDir: "{code:Vst3Root}\MuseSync.vst3"; Flags: ignoreversion
Source: "{#MyBuildDir}\VST3\Release\MuseSync.vst3\desktop.ini"; DestDir: "{code:Vst3Root}\MuseSync.vst3"; Flags: ignoreversion
Source: "{#MySourceDir}\companion\MuseToneSync\MuseToneSync.qml"; DestDir: "{code:MuseScoreRoot}\plugins\MuseToneSync"; Flags: ignoreversion
Source: "{#MySourceDir}\companion\MuseToneSync\ScoreSnapshot.js"; DestDir: "{code:MuseScoreRoot}\plugins\MuseToneSync"; Flags: ignoreversion
Source: "{#MySourceDir}\LICENSE"; DestDir: "{app}"
Source: "{#MySourceDir}\installer\windows\INSTALL.txt"; DestDir: "{app}"
Source: "{#MySourceDir}\installer\windows\INSTALL.txt"; DestDir: "{app}"; DestName: "安装说明.txt"
Source: "{#MySourceDir}\docs\musetone-windows.md"; DestDir: "{app}"
Source: "{#MySourceDir}\CHANGELOG.md"; DestDir: "{app}"
Source: "{#MySourceDir}\source\third_party\signalsmith-stretch\LICENSE.txt"; DestDir: "{app}\licenses"; DestName: "signalsmith-stretch.txt"
Source: "{#MySourceDir}\source\third_party\signalsmith-stretch\dsp\LICENSE.txt"; DestDir: "{app}\licenses"; DestName: "signalsmith-dsp.txt"
Source: "{#MyBuildDir}\release-notices\*"; DestDir: "{app}\licenses"; Flags: recursesubdirs createallsubdirs
Source: "{#MySourceDir}\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"
Source: "{#MySourceDir}\installer\windows\Uninstall-MuseSync.vbs"; DestDir: "{app}"
Source: "{#MySourceDir}\installer\windows\Uninstall-MuseSync.ps1"; DestDir: "{app}"

[Icons]
Name: "{app}\一键卸载 MuseSync"; Filename: "{sys}\wscript.exe"; Parameters: """{app}\Uninstall-MuseSync.vbs"""; IconFilename: "{sys}\shell32.dll"; IconIndex: 131

[Registry]
#ifdef SmokeTest
Root: HKCU; Subkey: "Software\MuseToneInstallerSmoke"; ValueType: string; ValueName: "MuseScoreDir"; ValueData: "{code:MuseScoreRoot}"; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKCU; Subkey: "Software\MuseToneInstallerSmoke"; ValueType: string; ValueName: "Vst3Dir"; ValueData: "{code:Vst3Root}"; Flags: uninsdeletevalue uninsdeletekeyifempty
#else
Root: HKLM; Subkey: "Software\MuseTone"; ValueType: string; ValueName: "MuseScoreDir"; ValueData: "{code:MuseScoreRoot}"; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKLM; Subkey: "Software\MuseTone"; ValueType: string; ValueName: "Vst3Dir"; ValueData: "{code:Vst3Root}"; Flags: uninsdeletevalue uninsdeletekeyifempty
#endif

[Run]
Filename: "{app}\安装说明.txt"; Description: "查看安装与启用说明"; Flags: postinstall shellexec nowait skipifsilent unchecked

[Code]
var
  ScorePage: TInputDirWizardPage;

function PreviousDirectory(Name: String): String;
begin
  Result := '';
#ifdef SmokeTest
  RegQueryStringValue(HKCU, 'Software\MuseToneInstallerSmoke', Name, Result);
#else
  RegQueryStringValue(HKLM, 'Software\MuseTone', Name, Result);
#endif
end;

function Vst3Root(Param: String): String;
begin
  Result := ExpandConstant('{param:VST3DIR|}');
  if Result = '' then Result := PreviousDirectory('Vst3Dir');
  if Result = '' then Result := ExpandConstant('{commoncf64}\VST3');
  Result := RemoveBackslash(Result);
end;

function MuseScoreRoot(Param: String): String;
begin
  Result := RemoveBackslash(ScorePage.Values[0]);
end;

function IsMuseScoreDir(Path: String): Boolean;
begin
  Result := FileExists(AddBackslash(Path) + 'bin\MuseScore4.exe');
end;

function ScoreDirectoryError(Path: String): String;
begin
  Result := '';
  if IsMuseScoreDir(Path) then exit;
  if FileExists(AddBackslash(Path) + 'bin\MuseScore3.exe') or
     FileExists(AddBackslash(Path) + 'bin\mscore.exe') then
    Result := '检测到 MuseScore 3。该版本不支持 VST3，无法使用 MuseSync。请安装 MuseScore 4 后重新选择其安装文件夹。'
  else
    Result := '未找到 MuseScore 4。请选择包含 bin\MuseScore4.exe 的安装文件夹。MuseScore 3 不支持 VST3，不能安装本插件。';
end;

function DetectMuseScore(): String;
var
  Keys: TArrayOfString;
  I: Integer;
  Key, Path, Name: String;
begin
  Result := ExpandConstant('{param:MUSESCOREDIR|}');
  if Result <> '' then exit;
  Path := PreviousDirectory('MuseScoreDir');
  if IsMuseScoreDir(Path) then
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
  if IsMuseScoreDir(Path) then begin Result := Path; exit; end;
  Path := ExpandConstant('{autopf}\MuseScore4');
  if IsMuseScoreDir(Path) then begin Result := Path; exit; end;
  Path := ExpandConstant('{autopf}\MuseScore Studio 4');
  if IsMuseScoreDir(Path) then Result := Path;
end;

procedure InitializeWizard();
begin
  ScorePage := CreateInputDirPage(wpLicense, 'MuseScore 安装位置',
    '请选择 MuseScore 4 的安装文件夹',
    'MuseSync Bridge 将安装到该程序的 plugins 文件夹中。请选择包含 bin\MuseScore4.exe 的文件夹，不是存放谱面的文件夹。支持选择不同 4.x 版本；MuseScore 3 不支持 VST3，无法使用。',
    False, '');
  ScorePage.Add('MuseScore 4 安装文件夹：');
  ScorePage.Values[0] := DetectMuseScore();
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = ScorePage.ID then
    if not IsMuseScoreDir(ScorePage.Values[0]) then
    begin
      if WizardSilent then
        Log(ScoreDirectoryError(ScorePage.Values[0]))
      else
      begin
        MsgBox(ScoreDirectoryError(ScorePage.Values[0]), mbError, MB_OK);
        Result := False;
      end;
    end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := ScoreDirectoryError(MuseScoreRoot(''));
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Legacy, Binary, Backup: String;
  Metadata: AnsiString;
  I: Integer;
begin
  if CurStep <> ssPostInstall then exit;
  Legacy := AddBackslash(Vst3Root('')) + 'MuseTone.vst3';
  if not DirExists(Legacy) then exit;
  if not LoadStringFromFile(Legacy + '\Contents\Resources\moduleinfo.json', Metadata) then
    RaiseException('无法验证旧版 MuseTone 的安装信息。请检查旧版文件后重试。');
  if (Pos('4D555354544F4E455052433200000001', String(Metadata)) = 0) or
     (Pos('4D555354544F4E454354523200000002', String(Metadata)) = 0) then
    RaiseException('现有 MuseTone.vst3 属于其他插件，未对其进行修改。');
  I := 0;
  repeat
    Backup := AddBackslash(Vst3Root('')) + 'MuseTone.previous-' + IntToStr(I) + '.disabled';
    I := I + 1;
  until not FileExists(Backup) and not DirExists(Backup);
  Binary := Legacy + '\Contents\x86_64-win\MuseTone.vst3';
  if not RenameFile(Binary, Binary + '.disabled') then
    RaiseException('请关闭 MuseScore 和其他 VST3 宿主，再次运行安装程序。');
  if not RenameFile(Legacy, Backup) then
  begin
    RenameFile(Binary + '.disabled', Binary);
    RaiseException('无法归档旧版 MuseTone 插件。请关闭所有 VST3 宿主后重试。');
  end;
  Log('Archived previous MuseTone bundle: ' + Backup);
end;

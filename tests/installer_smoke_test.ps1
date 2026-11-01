#Requires -Version 7.0
[CmdletBinding()]
param(
    [string] $BuildDirectory = 'build-musetone-live',
    [string] $IsccPath = "$env:LOCALAPPDATA/Programs/Inno Setup 6/ISCC.exe"
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$build = if ([IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory } else { Join-Path $root $BuildDirectory }
$sandbox = Join-Path $build ('installer-smoke-' + [guid]::NewGuid().ToString('N'))
$score = Join-Path $sandbox 'Mock MuseScore'
$vst = Join-Path $sandbox 'VST3'
$app = Join-Path $sandbox 'Application'
$out = Join-Path $sandbox 'compiler'
if (Test-Path 'HKCU:/Software/MuseToneInstallerSmoke') { throw 'Previous smoke test registration exists' }
if (Test-Path 'HKCU:/Software/Microsoft/Windows/CurrentVersion/Uninstall/MuseToneInstallerSmoke_is1') {
    throw 'Previous smoke test uninstall registration exists'
}
New-Item -ItemType Directory -Path (Join-Path $score 'bin'), $vst, $out -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $build 'bin/Release/validator.exe') -Destination (Join-Path $score 'bin/MuseScore4.exe')
$sentinel = Join-Path $vst 'unrelated-plugin.txt'
$sync = Join-Path $score 'plugins/MuseToneSync'
New-Item -ItemType Directory -Path $sync -Force | Out-Null
$syncSentinel = Join-Path $sync 'unrelated-file.txt'
[IO.File]::WriteAllText($sentinel, 'preserve other VST plugins')
[IO.File]::WriteAllText($syncSentinel, 'preserve untracked user files')
$legacy = Join-Path $vst 'MuseTone.vst3'
New-Item -ItemType Directory -Path (Join-Path $legacy 'Contents/x86_64-win'), (Join-Path $legacy 'Contents/Resources') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $build 'VST3/Release/MuseSync.vst3/Contents/x86_64-win/MuseSync.vst3') -Destination (Join-Path $legacy 'Contents/x86_64-win/MuseTone.vst3')
Copy-Item -LiteralPath (Join-Path $build 'VST3/Release/MuseSync.vst3/Contents/Resources/moduleinfo.json') -Destination (Join-Path $legacy 'Contents/Resources/moduleinfo.json')
[IO.File]::WriteAllText((Join-Path $legacy 'user-file.txt'), 'preserve legacy bundle extras')

# Same payload and wizard code; only privilege level and registry identity are isolated.
& $IsccPath /Q /DSmokeTest /DMySmokeVersion=2.1.0 "/DMyBuildDir=$build" "/DMyOutputDir=$out" (Join-Path $root 'installer/windows/setup-musetone.iss')
if ($LASTEXITCODE -ne 0) { throw 'Smoke installer compilation failed' }
$setup = Get-ChildItem -LiteralPath $out -Filter '*.exe' | Select-Object -First 1
# A silent invocation must reject MuseScore 3 before writing payload or registration.
$score3 = Join-Path $sandbox 'Mock MuseScore 3'
$rejectedApp = Join-Path $sandbox 'Rejected Application'
$rejectedVst = Join-Path $sandbox 'Rejected VST3'
New-Item -ItemType Directory -Path (Join-Path $score3 'bin') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $build 'bin/Release/validator.exe') -Destination (Join-Path $score3 'bin/MuseScore3.exe')
$process = Start-Process -FilePath $setup.FullName -ArgumentList @(
    '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/NOCLOSEAPPLICATIONS', '/SP-',
    "/DIR=`"$rejectedApp`"", "/MUSESCOREDIR=`"$score3`"", "/VST3DIR=`"$rejectedVst`"", "/LOG=`"$sandbox/reject-ms3.log`""
) -WindowStyle Hidden -Wait -PassThru
if ($process.ExitCode -eq 0) { throw 'MuseScore 3 was incorrectly accepted' }
foreach ($path in @($rejectedApp, $rejectedVst, (Join-Path $score3 'plugins'))) {
    if (Test-Path -LiteralPath $path) { throw "Rejected installation wrote payload: $path" }
}
if (Test-Path 'HKCU:/Software/MuseToneInstallerSmoke') { throw 'Rejected installation wrote registration' }
if (-not (Select-String -LiteralPath (Join-Path $sandbox 'reject-ms3.log') -Pattern 'MuseScore 3')) {
    throw 'Rejection log did not explain the unsupported host'
}
$process = Start-Process -FilePath $setup.FullName -ArgumentList @(
    '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/NOCLOSEAPPLICATIONS', '/SP-',
    "/DIR=`"$app`"", "/MUSESCOREDIR=`"$score`"", "/VST3DIR=`"$vst`"", "/LOG=`"$sandbox/install.log`""
) -WindowStyle Hidden -Wait -PassThru
if ($process.ExitCode -ne 0) { throw "Installer failed ($($process.ExitCode)): $sandbox/install.log" }
$payload = @{
    (Join-Path $vst 'MuseSync.vst3/Contents/x86_64-win/MuseSync.vst3') = (Join-Path $build 'VST3/Release/MuseSync.vst3/Contents/x86_64-win/MuseSync.vst3')
    (Join-Path $sync 'MuseToneSync.qml') = (Join-Path $root 'companion/MuseToneSync/MuseToneSync.qml')
    (Join-Path $sync 'ScoreSnapshot.js') = (Join-Path $root 'companion/MuseToneSync/ScoreSnapshot.js')
}
foreach ($entry in $payload.GetEnumerator()) {
    if ((Get-FileHash -LiteralPath $entry.Key).Hash -ne (Get-FileHash -LiteralPath $entry.Value).Hash) {
        throw "Installed payload mismatch: $($entry.Key)"
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $app 'INSTALL.txt'))) { throw 'Instructions not installed' }
foreach ($file in @('Uninstall-MuseSync.vbs', 'Uninstall-MuseSync.ps1', '一键卸载 MuseSync.lnk', '安装说明.txt')) {
    if (-not (Test-Path -LiteralPath (Join-Path $app $file))) { throw "Uninstall entry missing: $file" }
}
if (-not (Test-Path -LiteralPath (Join-Path $app 'licenses/vst3sdk-LICENSE.txt'))) { throw 'License not installed' }
if ((Get-FileHash -LiteralPath (Join-Path $app '安装说明.txt')).Hash -ne
    (Get-FileHash -LiteralPath (Join-Path $root 'installer/windows/INSTALL.txt')).Hash) {
    throw 'Chinese installation instructions do not match the current source'
}
if (-not (Test-Path 'HKCU:/Software/MuseToneInstallerSmoke')) { throw 'Install registration missing' }
if (Test-Path -LiteralPath $legacy) { throw 'Legacy bundle is still active after upgrade' }
$backup = @(Get-ChildItem -LiteralPath $vst -Directory -Filter 'MuseTone.previous-*.disabled')
if ($backup.Count -ne 1 -or -not (Test-Path -LiteralPath (Join-Path $backup[0].FullName 'user-file.txt'))) {
    throw 'Upgrade did not preserve the previous bundle and its extra files'
}
if (Get-ChildItem -LiteralPath $backup[0].FullName -Recurse -File -Filter '*.vst3') {
    throw 'Archived legacy binary is still scannable'
}

# Synthetic older installer registration exercises an in-place version upgrade.
$uninstallKey = 'HKCU:/Software/Microsoft/Windows/CurrentVersion/Uninstall/MuseToneInstallerSmoke_is1'
if ((Get-ItemProperty -LiteralPath $uninstallKey).DisplayVersion -ne '2.1.0') {
    throw 'Older installer fixture version was not registered'
}
$stateFiles = @{
    (Join-Path $sync 'MuseToneSync.ini') = "[snapshot]`nvalue=preserve-score-state"
    (Join-Path $sync 'MuseToneCommands.ini') = "[commands]`nvalue=preserve-command-state"
}
foreach ($entry in $stateFiles.GetEnumerator()) { [IO.File]::WriteAllText($entry.Key, $entry.Value) }
foreach ($path in $payload.Keys) { [IO.File]::WriteAllText($path, 'outdated payload fixture') }
$upgradeOut = Join-Path $sandbox 'upgrade-compiler'
& $IsccPath /Q /DSmokeTest "/DMyBuildDir=$build" "/DMyOutputDir=$upgradeOut" (Join-Path $root 'installer/windows/setup-musetone.iss')
if ($LASTEXITCODE -ne 0) { throw 'Upgrade installer compilation failed' }
$upgrade = Get-ChildItem -LiteralPath $upgradeOut -Filter '*.exe' | Select-Object -First 1
# No path arguments: the installer must reuse the previous app, score and VST3 directories.
$process = Start-Process -FilePath $upgrade.FullName -ArgumentList @(
    '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/NOCLOSEAPPLICATIONS', '/SP-',
    "/LOG=`"$sandbox/upgrade.log`""
) -WindowStyle Hidden -Wait -PassThru
if ($process.ExitCode -ne 0) { throw "Upgrade failed ($($process.ExitCode)): $sandbox/upgrade.log" }
foreach ($entry in $payload.GetEnumerator()) {
    if ((Get-FileHash -LiteralPath $entry.Key).Hash -ne (Get-FileHash -LiteralPath $entry.Value).Hash) {
        throw "Upgrade did not replace outdated payload: $($entry.Key)"
    }
}
$version = (Get-Content -LiteralPath (Join-Path $root 'installer/windows/release.ini') | Where-Object { $_ -match '^Version=' }) -replace '^Version=', ''
if ((Get-ItemProperty -LiteralPath $uninstallKey).DisplayVersion -ne $version) {
    throw 'Upgrade did not update the existing uninstall registration'
}
if (@(Get-ChildItem -LiteralPath $app -Filter 'unins*.exe').Count -ne 1) { throw 'Upgrade created a duplicate uninstaller' }
if (@(Get-ChildItem -LiteralPath $vst -Directory -Filter 'MuseTone.previous-*.disabled').Count -ne 1) {
    throw 'Upgrade unexpectedly changed the previous legacy backup'
}
foreach ($entry in $stateFiles.GetEnumerator()) {
    if ([IO.File]::ReadAllText($entry.Key) -cne $entry.Value) { throw "Upgrade changed saved state: $($entry.Key)" }
}
$uninstaller = Join-Path $app 'unins000.exe'
. (Join-Path $root 'installer/windows/Uninstall-MuseSync.ps1') -LibraryOnly
Remove-MuseSyncBundle (Join-Path $vst 'MuseSync.vst3')
Remove-MuseSyncBundle $backup[0].FullName
Remove-MuseSyncBridge $sync
foreach ($path in $payload.Keys) {
    if (Test-Path -LiteralPath $path) { throw "One-click cleanup missed payload: $path" }
}
if (-not (Test-Path -LiteralPath (Join-Path $backup[0].FullName 'user-file.txt'))) {
    throw 'One-click cleanup removed a user file from the legacy archive'
}
$process = Start-Process -FilePath $uninstaller -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART') -WindowStyle Hidden -Wait -PassThru
if ($process.ExitCode -ne 0) { throw "Uninstaller failed: $($process.ExitCode)" }
foreach ($path in $payload.Keys) {
    if (Test-Path -LiteralPath $path) { throw "Tracked payload not uninstalled: $path" }
}
if (-not (Test-Path -LiteralPath $sentinel) -or -not (Test-Path -LiteralPath $syncSentinel)) {
    throw 'Uninstall removed unrelated user files'
}
if (Test-Path 'HKCU:/Software/MuseToneInstallerSmoke') { throw 'Smoke registration not removed' }
if (Test-Path -LiteralPath $uninstallKey) { throw 'Uninstall registration not removed' }
foreach ($entry in $stateFiles.GetEnumerator()) {
    if ([IO.File]::ReadAllText($entry.Key) -cne $entry.Value) { throw "Uninstall changed untracked saved state: $($entry.Key)" }
}
Write-Output "PASS: isolated install, upgrade, one-click VST3/Bridge cleanup, saved-state preservation and registered uninstall. Logs: $sandbox"

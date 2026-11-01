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

# Same payload and wizard code; only privilege level and registry identity are isolated.
& $IsccPath /Q /DSmokeTest "/DMyBuildDir=$build" "/DMyOutputDir=$out" (Join-Path $root 'installer/windows/setup-musetone.iss')
if ($LASTEXITCODE -ne 0) { throw 'Smoke installer compilation failed' }
$setup = Get-ChildItem -LiteralPath $out -Filter '*.exe' | Select-Object -First 1
$process = Start-Process -FilePath $setup.FullName -ArgumentList @(
    '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/NOCLOSEAPPLICATIONS', '/SP-',
    "/DIR=`"$app`"", "/MUSESCOREDIR=`"$score`"", "/VST3DIR=`"$vst`"", "/LOG=`"$sandbox/install.log`""
) -WindowStyle Hidden -Wait -PassThru
if ($process.ExitCode -ne 0) { throw "Installer failed ($($process.ExitCode)): $sandbox/install.log" }
$payload = @{
    (Join-Path $vst 'MuseTone.vst3/Contents/x86_64-win/MuseTone.vst3') = (Join-Path $build 'VST3/Release/MuseTone.vst3/Contents/x86_64-win/MuseTone.vst3')
    (Join-Path $sync 'MuseToneSync.qml') = (Join-Path $root 'companion/MuseToneSync/MuseToneSync.qml')
    (Join-Path $sync 'ScoreSnapshot.js') = (Join-Path $root 'companion/MuseToneSync/ScoreSnapshot.js')
}
foreach ($entry in $payload.GetEnumerator()) {
    if ((Get-FileHash -LiteralPath $entry.Key).Hash -ne (Get-FileHash -LiteralPath $entry.Value).Hash) {
        throw "Installed payload mismatch: $($entry.Key)"
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $app 'INSTALL.txt'))) { throw 'Instructions not installed' }
if (-not (Test-Path -LiteralPath (Join-Path $app 'licenses/vst3sdk-LICENSE.txt'))) { throw 'License not installed' }
if (-not (Test-Path 'HKCU:/Software/MuseToneInstallerSmoke')) { throw 'Install registration missing' }
$uninstaller = Join-Path $app 'unins000.exe'
$process = Start-Process -FilePath $uninstaller -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART') -WindowStyle Hidden -Wait -PassThru
if ($process.ExitCode -ne 0) { throw "Uninstaller failed: $($process.ExitCode)" }
foreach ($path in $payload.Keys) {
    if (Test-Path -LiteralPath $path) { throw "Tracked payload not uninstalled: $path" }
}
if (-not (Test-Path -LiteralPath $sentinel) -or -not (Test-Path -LiteralPath $syncSentinel)) {
    throw 'Uninstall removed unrelated user files'
}
if (Test-Path 'HKCU:/Software/MuseToneInstallerSmoke') { throw 'Smoke registration not removed' }
Write-Output "PASS: isolated install/uninstall, custom paths with spaces, payload hashes, licenses and preservation of unrelated files. Logs: $sandbox"

#Requires -Version 7.0
[CmdletBinding()]
param(
    [string] $BuildDirectory = 'build-musetone-live',
    [string] $OutputDirectory = '',
    [string] $IsccPath = ''
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
function AbsolutePath([string] $Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $root $Path))
}
$build = AbsolutePath $BuildDirectory
$cmake = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
$version = [regex]::Match($cmake, 'project\(MuseTone VERSION ([0-9]+\.[0-9]+\.[0-9]+)').Groups[1].Value
if (-not $version) { throw 'Cannot read project version' }
if (-not $OutputDirectory) { $OutputDirectory = "dist/releases/$version" }
$output = AbsolutePath $OutputDirectory
$installerVersion = Get-Content -LiteralPath (Join-Path $root 'installer/windows/release.ini') -Raw
if ($installerVersion -notmatch "(?m)^Version=$([regex]::Escape($version))\s*$") { throw 'Installer version differs from CMake version' }
$bundle = Join-Path $build 'VST3/Release/MuseSync.vst3'
$info = Get-Content -LiteralPath (Join-Path $bundle 'Contents/Resources/moduleinfo.json') -Raw | ConvertFrom-Json
if (@($info.Classes | Where-Object Version -ne $version).Count) { throw 'Built VST3 version differs from release version' }
if (-not $IsccPath) {
    $candidates = @(
        "${env:ProgramFiles(x86)}/Inno Setup 6/ISCC.exe",
        "$env:LOCALAPPDATA/Programs/Inno Setup 6/ISCC.exe"
    )
    $IsccPath = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $IsccPath) { throw 'Inno Setup 6 compiler is required' }
New-Item -ItemType Directory -Path $output -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'installer/windows/INSTALL.txt') -Destination (Join-Path $output '安装说明.txt') -Force
foreach ($file in @('Uninstall-MuseSync.vbs', 'Uninstall-MuseSync.ps1')) {
    Copy-Item -LiteralPath (Join-Path $root "installer/windows/$file") -Destination $output -Force
}
$notices = Join-Path $build 'release-notices'
New-Item -ItemType Directory -Path $notices -Force | Out-Null
$licenses = @{
    'minibpm-COPYING.txt' = 'source/third_party/minibpm/COPYING'
    'stb_vorbis.c' = 'source/third_party/stb_vorbis.c'
    'miniz.c' = 'vst3sdk/vstgui4/vstgui/thirdparty/miniz/miniz.c'
    'signalsmith-stretch.txt' = 'source/third_party/signalsmith-stretch/LICENSE.txt'
    'signalsmith-dsp.txt' = 'source/third_party/signalsmith-stretch/dsp/LICENSE.txt'
    'vst3sdk-LICENSE.txt' = 'vst3sdk/LICENSE.txt'
    'vst3-base-LICENSE.txt' = 'vst3sdk/base/LICENSE.txt'
    'vst3-pluginterfaces-LICENSE.txt' = 'vst3sdk/pluginterfaces/LICENSE.txt'
    'vst3-public-sdk-LICENSE.txt' = 'vst3sdk/public.sdk/LICENSE.txt'
}
foreach ($entry in $licenses.GetEnumerator()) {
    Copy-Item -LiteralPath (Join-Path $root $entry.Value) -Destination (Join-Path $notices $entry.Key) -Force
}
& $IsccPath "/DMyBuildDir=$build" "/DMyOutputDir=$output" (Join-Path $root 'installer/windows/setup-musetone.iss')
if ($LASTEXITCODE -ne 0) { throw "Installer compiler failed: $LASTEXITCODE" }
$staging = Join-Path $build ('package-stage-' + [Guid]::NewGuid().ToString('N'))
$portable = Join-Path $staging "MuseSync-$version-Windows-x64"
New-Item -ItemType Directory -Path $portable -Force | Out-Null
$portableBundle = Join-Path $portable 'MuseSync.vst3'
foreach ($file in @('Contents/x86_64-win/MuseSync.vst3', 'Contents/Resources/moduleinfo.json', 'PlugIn.ico', 'desktop.ini')) {
    $target = Join-Path $portableBundle $file
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $bundle $file) -Destination $target -Force
}
$companion = Join-Path $portable 'MuseToneSync'
New-Item -ItemType Directory -Path $companion -Force | Out-Null
foreach ($file in @('MuseToneSync.qml', 'ScoreSnapshot.js')) {
    Copy-Item -LiteralPath (Join-Path $root "companion/MuseToneSync/$file") -Destination $companion -Force
}
foreach ($file in @('LICENSE', 'CHANGELOG.md', 'THIRD_PARTY_NOTICES.md', 'installer/windows/INSTALL.txt', 'docs/musetone-windows.md')) {
    Copy-Item -LiteralPath (Join-Path $root $file) -Destination $portable -Force
}
foreach ($file in @('Uninstall-MuseSync.vbs', 'Uninstall-MuseSync.ps1')) {
    Copy-Item -LiteralPath (Join-Path $root "installer/windows/$file") -Destination $portable -Force
}
Copy-Item -LiteralPath (Join-Path $root 'installer/windows/INSTALL.txt') -Destination (Join-Path $portable '安装说明.txt') -Force
$portableLicenses = Join-Path $portable 'licenses'
New-Item -ItemType Directory -Path $portableLicenses -Force | Out-Null
Get-ChildItem -LiteralPath $notices -File | Copy-Item -Destination $portableLicenses -Force
$zip = Join-Path $output "MuseSync-$version-Windows-x64.zip"
Compress-Archive -LiteralPath $portable -DestinationPath $zip -Force

# Full corresponding source includes the pinned SDK, but not Git metadata or local sync state.
$sourceZip = Join-Path $output "MuseSync-$version-Source.zip"
$sourceInputs = @(
    'CMakeLists.txt', 'README.md', 'LICENSE', 'CHANGELOG.md', 'THIRD_PARTY_NOTICES.md', '.gitignore',
    'build.sh', 'make_pkg.sh', 'source', 'tests', 'testdata', 'installer', 'tools', 'companion',
    'pkg', '.github', 'docs/musetone-windows.md', 'docs/overview.md', "docs/RELEASE-$version.md",
    'test_player.cpp', 'ogg_vorbis_test.cpp', 'race_test.cpp', 'validate.cpp', 'decode_probe.mm',
    'factory_lifecycle_test.cpp', 'e2e_test.cpp', 'gui_repro.mm', 'vst3sdk'
)
& tar.exe -a -cf $sourceZip --exclude=.git --exclude=__pycache__ --exclude=*.jsc --exclude=MuseToneSync.ini --exclude=MuseToneCommands.ini -C $root @sourceInputs
if ($LASTEXITCODE -ne 0) { throw "Source archive failed: $LASTEXITCODE" }
$assets = @(
    (Join-Path $output "MuseSync-Setup-$version-Windows-x64.exe"), $zip, $sourceZip
)
$checksums = $assets | ForEach-Object {
    $hash = Get-FileHash -LiteralPath $_ -Algorithm SHA256
    '{0}  {1}' -f $hash.Hash.ToLowerInvariant(), [IO.Path]::GetFileName($_)
}
$checksums | Set-Content -LiteralPath (Join-Path $output 'SHA256SUMS.txt') -Encoding ascii
Copy-Item -LiteralPath (Join-Path $root "docs/RELEASE-$version.md") -Destination (Join-Path $output 'RELEASE-NOTES.md') -Force
Copy-Item -LiteralPath $assets[0] -Destination (Join-Path $root ([IO.Path]::GetFileName($assets[0]))) -Force
Copy-Item -LiteralPath (Join-Path $root 'installer/windows/INSTALL.txt') -Destination (Join-Path $root '安装说明.txt') -Force
Get-Item -LiteralPath $assets | Select-Object Name,Length

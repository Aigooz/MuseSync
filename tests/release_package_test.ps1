#Requires -Version 7.0
[CmdletBinding()]
param([string] $ReleaseDirectory = '')
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$version = [regex]::Match((Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw), 'project\(MuseTone VERSION ([0-9]+\.[0-9]+\.[0-9]+)').Groups[1].Value
if (-not $ReleaseDirectory) { $ReleaseDirectory = "dist/releases/$version" }
$release = if ([IO.Path]::IsPathRooted($ReleaseDirectory)) { $ReleaseDirectory } else { Join-Path $root $ReleaseDirectory }
$checksums = Get-Content -LiteralPath (Join-Path $release 'SHA256SUMS.txt')
if ($checksums.Count -ne 3) { throw 'Expected hashes for EXE, portable ZIP and source ZIP' }
foreach ($line in $checksums) {
    if ($line -notmatch '^([0-9a-f]{64})  ([^/\\]+)$') { throw "Invalid checksum line: $line" }
    $expected, $file = $Matches[1], $Matches[2]
    if ((Get-FileHash -LiteralPath (Join-Path $release $file)).Hash.ToLowerInvariant() -ne $expected) {
        throw "Checksum mismatch: $file"
    }
}
$exe = Get-Item -LiteralPath (Join-Path $release "MuseSync-Setup-$version-Windows-x64.exe")
if ($exe.VersionInfo.ProductVersion.Trim() -ne $version) { throw 'Installer version metadata mismatch' }
if ($exe.VersionInfo.ProductName -notlike 'MuseSync*') { throw 'Installer brand mismatch' }
$archives = @{
    "MuseSync-$version-Source.zip" = @(
        'CMakeLists.txt', 'source/win/gui_win.cpp', 'companion/MuseToneSync/MuseToneSync.qml',
        'installer/windows/release.ini', 'installer/windows/ChineseSimplified.isl',
        'vst3sdk/LICENSE.txt', 'vst3sdk/pluginterfaces/LICENSE.txt',
        'vst3sdk/public.sdk/LICENSE.txt', 'vst3sdk/base/LICENSE.txt',
        'vst3sdk/vstgui4/vstgui/thirdparty/miniz/miniz.c', 'vst3sdk/cmake/modules/SMTG_PlatformToolset.cmake'
    )
    "MuseSync-$version-Windows-x64.zip" = @(
        "MuseSync-$version-Windows-x64/MuseSync.vst3/Contents/x86_64-win/MuseSync.vst3",
        "MuseSync-$version-Windows-x64/MuseToneSync/MuseToneSync.qml",
        "MuseSync-$version-Windows-x64/licenses/signalsmith-stretch.txt",
        "MuseSync-$version-Windows-x64/INSTALL.txt",
        "MuseSync-$version-Windows-x64/安装说明.txt",
        "MuseSync-$version-Windows-x64/Uninstall-MuseSync.vbs",
        "MuseSync-$version-Windows-x64/Uninstall-MuseSync.ps1"
    )
}
foreach ($item in $archives.GetEnumerator()) {
    $zip = [IO.Compression.ZipFile]::OpenRead((Join-Path $release $item.Key))
    try {
        $names = @($zip.Entries.FullName | ForEach-Object { $_.Replace('\', '/') })
        foreach ($required in $item.Value) {
            if ($names -notcontains $required) { throw "Missing $required in $($item.Key)" }
        }
        $unwanted = $names | Where-Object {
            $_ -match '(^|/)\.git(/|$)|(^|/)__pycache__/|\.jsc$|MuseTone(Sync|Commands)\.ini$|^docs/.*\.png$|^build[^/]*/|^dist/'
        }
        if ($unwanted) { throw "Unwanted archive entries: $($unwanted -join ', ')" }
        if ($item.Key.EndsWith('Windows-x64.zip')) {
            $entry = $zip.GetEntry("MuseSync-$version-Windows-x64/安装说明.txt")
            $stream = $entry.Open()
            try { $guideHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream)) }
            finally { $stream.Dispose() }
            if ($guideHash -ne (Get-FileHash -LiteralPath (Join-Path $root 'installer/windows/INSTALL.txt')).Hash) {
                throw 'Stale Chinese installation instructions in portable archive'
            }
            if ($names -match '/MuseTone\.vst3/') { throw 'Portable archive contains the legacy bundle name' }
            $entry = $zip.GetEntry("MuseSync-$version-Windows-x64/MuseSync.vst3/Contents/Resources/moduleinfo.json")
            $reader = [IO.StreamReader]::new($entry.Open())
            try { $info = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
            if ($info.'Factory Info'.Vendor -ne 'MuseSync') { throw 'VST3 factory brand mismatch' }
            $expectedIds = @('4D555354544F4E455052433200000001', '4D555354544F4E454354523200000002')
            if (@($info.Classes).Count -ne 2) { throw 'Unexpected VST3 class count' }
            foreach ($class in $info.Classes) {
                if ($class.Name -ne 'MuseSync' -or $class.Version -ne $version) { throw 'VST3 class metadata mismatch' }
                if ($class.CID -notin $expectedIds) { throw 'VST3 class ID changed; existing scores would break' }
            }
        }
        if ($item.Key.EndsWith('Source.zip')) {
            foreach ($file in @('CMakeLists.txt', 'source/aplaysdk.cpp', 'source/win/gui_win.cpp',
                    'companion/MuseToneSync/MuseToneSync.qml', '.github/workflows/build-windows.yml',
                    'installer/windows/ChineseSimplified.isl', 'installer/windows/setup-musetone.iss',
                    'installer/windows/INSTALL.txt')) {
                $entry = $zip.GetEntry($file)
                $stream = $entry.Open()
                try { $archivedHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream)) }
                finally { $stream.Dispose() }
                if ($archivedHash -ne (Get-FileHash -LiteralPath (Join-Path $root $file)).Hash) {
                    throw "Stale source archive entry: $file"
                }
            }
        }
    } finally { $zip.Dispose() }
}
Write-Output "PASS: $version installer metadata, all release checksums, source payload hashes, SDK/license completeness and archive hygiene"

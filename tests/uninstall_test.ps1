#Requires -Version 5.1
[CmdletBinding()]
param([string] $BuildDirectory = 'build-musetone-live')
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
. (Join-Path $root 'installer/windows/Uninstall-MuseSync.ps1') -LibraryOnly
$build = if ([IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory } else { Join-Path $root $BuildDirectory }
$sandbox = Join-Path $build ('uninstall-test-' + [guid]::NewGuid().ToString('N'))
$vst = Join-Path $sandbox 'VST3'
$bridge = Join-Path $sandbox 'Mock MuseScore/plugins/MuseToneSync'
New-Item -ItemType Directory -Path $vst, $bridge -Force | Out-Null
$files = @('Contents/x86_64-win/MuseSync.vst3', 'Contents/Resources/moduleinfo.json', 'PlugIn.ico', 'desktop.ini')
$escaped = Join-Path $sandbox 'metadata-strings.json'
[IO.File]::WriteAllText($escaped, '{"text":"comma,} slash\\ quote\",]","list":[1,],}')
$parsed = Read-MuseSyncMetadata $escaped
if ($parsed.text -cne 'comma,} slash\ quote",]' -or $parsed.list[0] -ne 1) {
    throw 'Metadata normalization changed a quoted string'
}
$invalid = Join-Path $sandbox 'invalid.json'
[IO.File]::WriteAllText($invalid, '{"Classes":[BROKEN,]}')
$rejected = $false
try { Read-MuseSyncMetadata $invalid } catch { $rejected = $true }
if (-not $rejected) { throw 'Malformed metadata was accepted' }
foreach ($name in @('MuseSync.vst3', 'MuseTone.vst3', 'MuseTone.previous-123.disabled')) {
    $bundle = Join-Path $vst $name
    foreach ($file in $files) {
        $target = Join-Path $bundle $file
        New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $build "VST3/Release/MuseSync.vst3/$file") -Destination $target
    }
    if ($name -ne 'MuseSync.vst3') {
        Move-Item -LiteralPath (Join-Path $bundle $files[0]) -Destination (Join-Path $bundle 'Contents/x86_64-win/MuseTone.vst3.disabled')
    }
    $sentinel = Join-Path $bundle 'keep-user-file.txt'
    [IO.File]::WriteAllText($sentinel, 'user file')
    Remove-MuseSyncBundle $bundle
    Remove-MuseSyncBundle $bundle
    if (-not (Test-Path -LiteralPath $sentinel)) { throw 'Bundle user file was removed' }
    if (Get-ChildItem -LiteralPath $bundle -Recurse -File | Where-Object Name -ne 'keep-user-file.txt') { throw 'Owned payload was not removed' }
}
Remove-MuseSyncBundle (Join-Path $vst 'missing/MuseSync.vst3')
$locked = Join-Path $sandbox 'Locked/MuseSync.vst3'
foreach ($file in $files) {
    $target = Join-Path $locked $file
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $build "VST3/Release/MuseSync.vst3/$file") -Destination $target
}
$handle = [IO.File]::Open((Join-Path $locked $files[0]), 'Open', 'Read', 'None')
$rejected = $false
try { Remove-MuseSyncBundle $locked } catch { $rejected = $true } finally { $handle.Dispose() }
if (-not $rejected -or -not (Test-Path -LiteralPath (Join-Path $locked 'Contents/Resources/moduleinfo.json'))) {
    throw 'Locked file was not safely reported for retry'
}
Remove-MuseSyncBundle $locked
if (Test-Path -LiteralPath $locked) { throw 'Empty owned bundle directory remains' }
foreach ($file in @('MuseToneSync.qml', 'ScoreSnapshot.js', 'MuseToneSync.ini', 'MuseToneCommands.ini', 'keep.txt')) {
    [IO.File]::WriteAllText((Join-Path $bridge $file), 'test')
}
Remove-MuseSyncBridge $bridge
foreach ($file in @('MuseToneSync.qml', 'ScoreSnapshot.js')) {
    if (Test-Path -LiteralPath (Join-Path $bridge $file)) { throw 'Bridge payload remains' }
}
foreach ($file in @('MuseToneSync.ini', 'MuseToneCommands.ini', 'keep.txt')) {
    if (-not (Test-Path -LiteralPath (Join-Path $bridge $file))) { throw 'Saved state was removed' }
}
Remove-MuseSyncBridge $bridge
$foreign = Join-Path $sandbox 'Foreign/MuseSync.vst3'
New-Item -ItemType Directory -Path (Join-Path $foreign 'Contents/Resources') -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $foreign 'Contents/Resources/moduleinfo.json'), '{"Classes":[{"CID":"FOREIGN"}]}')
$rejected = $false
try { Remove-MuseSyncBundle $foreign } catch { $rejected = $true }
if (-not $rejected) { throw 'Foreign bundle was accepted' }
$junction = Join-Path $sandbox 'linked'
New-Item -ItemType Junction -Path $junction -Target $foreign | Out-Null
$rejected = $false
try { Remove-OwnedFiles $junction @('Contents/Resources/moduleinfo.json') } catch { $rejected = $true }
if (-not $rejected -or -not (Test-Path -LiteralPath (Join-Path $foreign 'Contents/Resources/moduleinfo.json'))) { throw 'Junction safety check failed' }
Write-Output "PASS (PowerShell $($PSVersionTable.PSVersion)): SDK metadata, quoted strings, malformed metadata rejection, VST3/Bridge cleanup, settings preservation, retry, foreign plugin and junction rejection. Fixtures: $sandbox"

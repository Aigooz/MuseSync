#Requires -Version 7.0
[CmdletBinding()]
param (
    [string] $BuildDirectory = 'build-musetone-live',
    [string] $MuseScoreDirectory = 'D:\Musescore 4.7'
)

$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$build = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
    [IO.Path]::GetFullPath($BuildDirectory)
} else {
    [IO.Path]::GetFullPath((Join-Path $workspace $BuildDirectory))
}
$source = Join-Path $build 'VST3/Release/MuseSync.vst3'
$targets = @(
    (Join-Path $workspace 'dist/windows-musetone/plugin-bundle/VST3/Release/MuseSync.vst3'),
    (Join-Path ([Environment]::GetFolderPath('CommonProgramFiles')) 'VST3/MuseSync.vst3')
)
$files = @(
    'Contents/x86_64-win/MuseSync.vst3',
    'Contents/Resources/moduleinfo.json',
    'PlugIn.ico',
    'desktop.ini'
)

foreach ($file in $files) {
    if (-not (Test-Path -LiteralPath (Join-Path $source $file) -PathType Leaf)) {
        throw "Missing built bundle file: $file"
    }
}

foreach ($target in $targets) {
    # Copy explicit files, never a bundle directory into an existing bundle.
    foreach ($file in $files) {
        $destination = Join-Path $target $file
        New-Item -ItemType Directory -Path (Split-Path $destination -Parent) -Force | Out-Null
        try {
            Copy-Item -LiteralPath (Join-Path $source $file) -Destination $destination -Force
        } catch {
            if ($file -ne 'Contents/x86_64-win/MuseSync.vst3') { throw }
            # A loaded image can sometimes be renamed even when writes are denied.
            $absoluteTarget = [IO.Path]::GetFullPath($target).TrimEnd('\')
            $absoluteBinary = (Resolve-Path -LiteralPath $destination).ProviderPath
            $expectedBinary = [IO.Path]::GetFullPath((Join-Path $absoluteTarget $file))
            if (-not $absoluteBinary.Equals($expectedBinary, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Refusing to rename an unexpected binary: $absoluteBinary"
            }
            $backup = $absoluteBinary + '.previous-' + [Guid]::NewGuid().ToString('N') + '.disabled'
            Move-Item -LiteralPath $absoluteBinary -Destination $backup
            try {
                Copy-Item -LiteralPath (Join-Path $source $file) -Destination $destination -Force
            } catch {
                Move-Item -LiteralPath $backup -Destination $absoluteBinary
                throw
            }
            Write-Output "Retained loaded binary: $backup"
        }
        $expected = (Get-FileHash -LiteralPath (Join-Path $source $file) -Algorithm SHA256).Hash
        $actual = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
        if ($actual -ne $expected) { throw "Hash mismatch: $destination" }
    }

    $nested = Join-Path $target 'MuseTone.vst3'
    if (Test-Path -LiteralPath $nested -PathType Container) {
        $resolvedTarget = (Resolve-Path -LiteralPath $target).ProviderPath.TrimEnd('\')
        $resolvedNested = (Resolve-Path -LiteralPath $nested).ProviderPath.TrimEnd('\')
        $expectedNested = [IO.Path]::GetFullPath((Join-Path $resolvedTarget 'MuseTone.vst3'))
        $nestedInfo = Join-Path $resolvedNested 'Contents/Resources/moduleinfo.json'
        if (-not $resolvedNested.Equals($expectedNested, [StringComparison]::OrdinalIgnoreCase) -or
            -not (Test-Path -LiteralPath $nestedInfo -PathType Leaf)) {
            throw "Refusing to move unverified nested directory: $nested"
        }
        $info = Get-Content -LiteralPath $nestedInfo -Raw | ConvertFrom-Json
        if ($info.'Factory Info'.Vendor -notin @('MuseTone', 'MuseSync')) {
            throw "Refusing to move a bundle from another vendor: $nested"
        }
        $archiveRoot = [IO.Path]::GetFullPath((Join-Path $workspace 'dist/deployment-backups'))
        if (-not $archiveRoot.StartsWith($workspace.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Backup directory must remain inside the workspace.'
        }
        New-Item -ItemType Directory -Path $archiveRoot -Force | Out-Null
        $archive = Join-Path $archiveRoot ('nested-' + [Guid]::NewGuid().ToString('N') + '.disabled')
        Move-Item -LiteralPath $resolvedNested -Destination $archive
        Write-Output "Archived nested duplicate: $archive"
    }

    $binary = Join-Path $target $files[0]
    Write-Output "Replaced: $binary"
    Write-Output "SHA256: $((Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash)"

    $parent = [IO.Path]::GetFullPath((Split-Path $target -Parent)).TrimEnd('\')
    $legacy = Join-Path $parent 'MuseTone.vst3'
    if (Test-Path -LiteralPath $legacy -PathType Container) {
        $resolved = (Resolve-Path -LiteralPath $legacy).ProviderPath.TrimEnd('\')
        if (-not $resolved.Equals([IO.Path]::GetFullPath($legacy), [StringComparison]::OrdinalIgnoreCase) -or
            (Get-Item -LiteralPath $resolved).Attributes.HasFlag([IO.FileAttributes]::ReparsePoint)) {
            throw "Refusing to archive unexpected legacy path: $resolved"
        }
        $metadata = Get-Content -LiteralPath (Join-Path $resolved 'Contents/Resources/moduleinfo.json') -Raw | ConvertFrom-Json
        $ids = @($metadata.Classes.CID)
        if ($ids.Count -ne 2 -or '4D555354544F4E455052433200000001' -notin $ids -or
            '4D555354544F4E454354523200000002' -notin $ids) {
            throw "Legacy bundle does not belong to MuseSync: $resolved"
        }
        $backup = Join-Path $parent ('MuseTone.previous-' + [Guid]::NewGuid().ToString('N') + '.disabled')
        if (-not [IO.Path]::GetFullPath($backup).StartsWith($parent + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Legacy backup must remain in the verified parent directory.'
        }
        $oldBinary = Join-Path $resolved 'Contents/x86_64-win/MuseTone.vst3'
        Move-Item -LiteralPath $oldBinary -Destination ($oldBinary + '.disabled')
        try { Move-Item -LiteralPath $resolved -Destination $backup }
        catch {
            Move-Item -LiteralPath ($oldBinary + '.disabled') -Destination $oldBinary
            throw
        }
        Write-Output "Archived legacy bundle (not scanned as VST3): $backup"
    }
}

$companionSource = Join-Path $workspace 'companion/MuseToneSync'
$companionTargets = @((Join-Path $workspace 'dist/windows-musetone/companion/MuseToneSync'))
if (Test-Path -LiteralPath (Join-Path $MuseScoreDirectory 'bin/MuseScore4.exe') -PathType Leaf) {
    $companionTargets += Join-Path $MuseScoreDirectory 'plugins/MuseToneSync'
}
foreach ($target in $companionTargets) {
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    foreach ($file in @('MuseToneSync.qml', 'ScoreSnapshot.js')) {
        $original = Join-Path $companionSource $file
        $destination = Join-Path $target $file
        Copy-Item -LiteralPath $original -Destination $destination -Force
        if ((Get-FileHash -LiteralPath $original).Hash -ne (Get-FileHash -LiteralPath $destination).Hash) {
            throw "Companion hash mismatch: $destination"
        }
    }
    Write-Output "Installed companion: $target"
}

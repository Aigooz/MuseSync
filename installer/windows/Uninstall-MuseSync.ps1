#Requires -Version 5.1
[CmdletBinding()]
param([switch] $LibraryOnly)
$ErrorActionPreference = 'Stop'

function Assert-PlainPath([string] $Path) {
    $full = [IO.Path]::GetFullPath($Path)
    $cursor = $full
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            if ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "拒绝处理链接目录：$cursor"
            }
        }
        $parent = Split-Path -Path $cursor -Parent
        if ($parent -eq $cursor) { break }
        $cursor = $parent
    }
    return $full
}

function Remove-OwnedFiles([string] $Directory, [string[]] $Files) {
    $root = (Assert-PlainPath $Directory).TrimEnd('\')
    foreach ($relative in $Files) {
        $path = Assert-PlainPath (Join-Path $root $relative)
        if (-not $path.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw "拒绝处理范围外的文件：$path"
        }
        if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Item -LiteralPath $path -Force }
    }
    # Only prune empty owned directories; never recursively delete user files.
    $parents = @($Files | ForEach-Object {
        $path = Split-Path -Path (Join-Path $root $_) -Parent
        while ($path.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) {
            $path
            $path = Split-Path -Path $path -Parent
        }
    }) + @($root)
    foreach ($path in ($parents | Select-Object -Unique | Sort-Object Length -Descending)) {
        if ((Test-Path -LiteralPath $path -PathType Container) -and
            -not (Get-ChildItem -LiteralPath $path -Force | Select-Object -First 1)) {
            Remove-Item -LiteralPath $path -Force
        }
    }
}

function Read-MuseSyncMetadata([string] $Path) {
    $text = Get-Content -LiteralPath $Path -Raw -Encoding UTF8
    # The VST3 SDK emits trailing commas; Windows PowerShell 5.1 rejects them.
    # Match entire quoted strings first so their commas and escapes remain intact.
    $text = [regex]::Replace($text, '"(?:\\.|[^"\\])*"|,(?=\s*[}\]])',
        [Text.RegularExpressions.MatchEvaluator] {
            param($match)
            if ($match.Value[0] -eq ',') { return '' }
            return $match.Value
        })
    try { return ($text | ConvertFrom-Json) }
    catch { throw "无法读取插件身份信息：$Path。文件格式损坏，未移除此插件。" }
}

function Remove-MuseSyncBundle([string] $Directory) {
    if (-not (Test-Path -LiteralPath $Directory -PathType Container)) { return }
    $root = Assert-PlainPath $Directory
    $name = Split-Path -Path $root -Leaf
    if ($name -notmatch '^(MuseSync\.vst3|MuseTone\.vst3|MuseTone\.previous-[a-zA-Z0-9-]+\.disabled)$') {
        throw "插件目录名称不匹配：$root"
    }
    $metadata = Assert-PlainPath (Join-Path $root 'Contents/Resources/moduleinfo.json')
    if (-not (Test-Path -LiteralPath $metadata -PathType Leaf)) {
        $binaryDir = Assert-PlainPath (Join-Path $root 'Contents/x86_64-win')
        if ((Test-Path -LiteralPath $binaryDir) -and
            (Get-ChildItem -LiteralPath $binaryDir -File -Force | Where-Object Name -Match '^(MuseSync|MuseTone)\.vst3')) {
            throw "无法验证插件身份：$root"
        }
        return
    }
    $info = Read-MuseSyncMetadata $metadata
    $ids = @($info.Classes.CID)
    if ($ids.Count -ne 2 -or '4D555354544F4E455052433200000001' -notin $ids -or
        '4D555354544F4E454354523200000002' -notin $ids) { throw "该目录不属于 MuseSync：$root" }
    $files = @('Contents/x86_64-win/MuseSync.vst3', 'Contents/x86_64-win/MuseTone.vst3',
        'Contents/x86_64-win/MuseTone.vst3.disabled', 'PlugIn.ico', 'desktop.ini')
    $binaryDir = Join-Path $root 'Contents/x86_64-win'
    if (Test-Path -LiteralPath $binaryDir) {
        Assert-PlainPath $binaryDir | Out-Null
        $files += @(Get-ChildItem -LiteralPath $binaryDir -File -Force | Where-Object {
            $_.Name -match '^(MuseSync|MuseTone)\.vst3\.previous-[a-fA-F0-9]{32}\.disabled$'
        } | ForEach-Object { 'Contents/x86_64-win/' + $_.Name })
    }
    # Keep identity metadata until all executable payloads have been removed successfully.
    $files += 'Contents/Resources/moduleinfo.json'
    Remove-OwnedFiles $root $files
}

function Remove-MuseSyncBridge([string] $Directory) {
    if ((Split-Path -Path $Directory -Leaf) -ne 'MuseToneSync') { throw '桥接插件路径不匹配' }
    Remove-OwnedFiles $Directory @('MuseToneSync.qml', 'ScoreSnapshot.js')
}

if ($LibraryOnly) { return }
Add-Type -AssemblyName System.Windows.Forms
try {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw '请双击 Uninstall-MuseSync.vbs，并允许管理员权限。'
    }
    $answer = [Windows.Forms.MessageBox]::Show(
        "卸载 MuseSync？`n`n将移除 MuseScore 桥接插件、MuseSync/MuseTone VST3 和已停用的旧版文件。`n谱面、音频、其他插件和同步设置会保留。`n`n请先保存谱面并退出 MuseScore 及其他使用该 VST3 的软件。",
        'MuseSync 一键卸载', 'YesNo', 'Question')
    if ($answer -ne 'Yes') { exit 0 }
    if (Get-Process -Name 'MuseScore4', 'MuseScore3', 'MuseScore' -ErrorAction SilentlyContinue) {
        throw 'MuseScore 仍在运行。请先保存并退出，然后重新点击卸载。不需要卸载 MuseScore 本身。'
    }
    $vstRoots = @((Join-Path ([Environment]::GetFolderPath('CommonProgramFiles')) 'VST3'))
    $scoreRoots = @()
    $registered = Get-ItemProperty 'HKLM:/Software/MuseTone' -ErrorAction SilentlyContinue
    if ($registered.Vst3Dir) { $vstRoots += $registered.Vst3Dir }
    if ($registered.MuseScoreDir) { $scoreRoots += $registered.MuseScoreDir }
    $keys = @('HKLM:/Software/Microsoft/Windows/CurrentVersion/Uninstall',
        'HKLM:/Software/WOW6432Node/Microsoft/Windows/CurrentVersion/Uninstall')
    foreach ($key in $keys) {
        foreach ($item in (Get-ChildItem -LiteralPath $key -ErrorAction SilentlyContinue)) {
            $entry = Get-ItemProperty -LiteralPath $item.PSPath
            if ($entry.DisplayName -like 'MuseScore*' -and $entry.InstallLocation) { $scoreRoots += $entry.InstallLocation }
        }
    }
    $scoreRoots += Join-Path $env:ProgramFiles 'MuseScore 4'
    foreach ($vst in ($vstRoots | Select-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $vst -PathType Container)) { continue }
        Assert-PlainPath $vst | Out-Null
        $bundles = @('MuseSync.vst3', 'MuseTone.vst3') | ForEach-Object { Join-Path $vst $_ }
        $bundles += @(Get-ChildItem -LiteralPath $vst -Directory -Filter 'MuseTone.previous-*.disabled' | ForEach-Object FullName)
        foreach ($bundle in $bundles) { Remove-MuseSyncBundle $bundle }
    }
    foreach ($score in ($scoreRoots | Select-Object -Unique)) {
        if (Test-Path -LiteralPath (Join-Path $score 'bin/MuseScore4.exe') -PathType Leaf) {
            Remove-MuseSyncBridge (Join-Path $score 'plugins/MuseToneSync')
        }
    }
    $key = 'HKLM:/Software/Microsoft/Windows/CurrentVersion/Uninstall/{6A1D47D5-4A18-4B5B-9A09-1C5E8A9B7D21}_is1'
    $entry = Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue
    if ($entry) {
        if ($entry.DisplayName -notmatch '^Muse(Sync|Tone)( |$)') { throw '卸载记录身份不匹配。' }
        $exe = Assert-PlainPath (Join-Path $entry.InstallLocation 'unins000.exe')
        if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw "卸载程序不存在：$exe" }
        $process = Start-Process -FilePath $exe -ArgumentList '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART' -WindowStyle Hidden -Wait -PassThru
        if ($process.ExitCode -ne 0) { throw "安装器卸载失败：$($process.ExitCode)" }
    }
    [Windows.Forms.MessageBox]::Show("MuseSync 插件及 VST3 已卸载。`n`n现在可以运行新的 EXE 重新安装，再打开 MuseScore。`n同步设置已保留；若 MuseScore 插件列表仍显示旧条目，请重新扫描。", 'MuseSync', 'OK', 'Information') | Out-Null
} catch {
    [Windows.Forms.MessageBox]::Show("卸载未完成：`n$($_.Exception.Message)`n`n如提示文件被占用，请关闭使用插件的软件后重试。其他错误请保留此提示以便排查。未删除谱面或音频。", 'MuseSync', 'OK', 'Error') | Out-Null
    exit 1
}

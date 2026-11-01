# MuseTone 2.1.0

Windows x64 / MuseTone Sync 1.2.0 / 2026-11-01

## 下载

- `MuseTone-Setup-2.1.0-Windows-x64.exe`：推荐安装方式，包含 VST3、Sync、说明和许可证。
- `MuseTone-2.1.0-Windows-x64.zip`：手动部署包。
- `MuseTone-2.1.0-Source.zip`：完整对应源码，包含构建时使用的 VST3 SDK 及子模块。
- `SHA256SUMS.txt`：以上文件的 SHA256 校验值。

安装器不是独立播放器，需要 Windows 10/11 x64 和 MuseScore 4。安装器未签名，
请核对来源和校验值，并遵循本机的安全策略。

## 更新

- 自动跟随谱面中途变速与换拍，包含小节内变速、弱起及不完整小节。
- 当前 BPM、拍号跟随播放位置；进度条右侧和最小化悬浮条显示「小节-拍数」。
- Sync 运行后弹窗确认已开启，关闭提示不会停止同步。
- 可拖动悬浮条，展开按钮对齐收起时的位置；进度条支持谱面定位。
- 九种主题、整理后的使用指南，以及「关于」中的小字署名 `@爱鸽咕子`。
- 安装器同时部署两个组件，支持自定义 MuseScore 安装目录并保留独立卸载入口。

## 安装

保存谱面并退出 MuseScore，运行 EXE，选择含 `bin\MuseScore4.exe` 的安装目录。
重启后在插件管理中启用 MuseTone Sync，每次会话运行一次；随后在混音器选择 MuseTone。
升级需同时更新 VST3 和 Sync，旧 DLL 不会在已运行的 MuseScore 中热更新。

手动部署：将 ZIP 中整个 `MuseTone.vst3` 文件夹复制到
`C:\Program Files\Common Files\VST3`，将 `MuseToneSync` 复制到 MuseScore 的 `plugins`。
Sync 需要对自身目录有写权限以保存快照，EXE 安装器会配置这一权限。

## 验证和边界

本版包含 5 组回归测试、343 组尺寸/DPI 检查、无界面 QML 桥接测试和 47 项 VST3 校验。
隔离安装/卸载测试覆盖自定义目录、文件哈希、许可证及无关文件保留；测试使用独立的
用户级注册标识，正式安装器的管理员权限/UAC 流程仍需人工确认。
这些检查不代替 MuseScore 实机验收；请先用谱面副本试用。

固定时长移调不改变伴奏速度，不提供伴奏自动变速拉伸。反复展开、临时播放速度倍率、
多 MuseScore 进程共享同一 Sync 目录仍有限制。本次仅更新 Windows 二进制。

项目基于 DaweiDrumScore，按 GPL-3.0 发布；源码及第三方许可证随附。

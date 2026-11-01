# MuseSync 2.2.0 · 谱伴同步

Windows x64 / MuseSync Bridge 1.3.0 / 2026-11-01

## 新名称

MuseTone 正式更名为 **MuseSync（谱伴同步）**，更直接地表达伴奏与乐谱同步的用途。
主面板、悬浮条、使用指南、启动提示、混音器音源名称和安装器同步更新。
谱面同步组件更名为 **MuseSync Bridge**，「关于」保留小字署名 `@爱鸽咕子`。

进度跳转、小节-拍数显示、中途变速与换拍、最小化悬浮条、九种主题和保持时长的移调均保留。

## 下载

- `MuseSync-Setup-2.2.0-Windows-x64.exe`：推荐，包含 VST3、Bridge、说明和许可证。
- `MuseSync-2.2.0-Windows-x64.zip`：手动部署包。
- `MuseSync-2.2.0-Source.zip`：完整对应源码，包含固定版本的 VST3 SDK 及子模块。
- `SHA256SUMS.txt`：以上三个文件的 SHA256 校验值。

## 升级与兼容

保存谱面并退出 MuseScore，运行安装器，选择包含 `bin\MuseScore4.exe` 的安装目录。
重启后在插件管理中启用 **MuseSync Bridge**，每次会话运行一次；混音器选择 **MuseSync**。
升级会同时更新两个组件，已经加载的 DLL 不会在正在运行的 MuseScore 中热更新。

为兼容旧谱面和已有安装，VST3 类 ID、参数状态、安装器身份及底层路径保持不变：

- VST3 文件夹仍为 `C:\Program Files\Common Files\VST3\MuseTone.vst3`。
- 同步组件仍放在 MuseScore 安装目录的 `plugins\MuseToneSync`，入口文件仍叫 `MuseToneSync.qml`。
- 说明及卸载文件仍在 `C:\Program Files\MuseTone`，Windows 应用列表显示 MuseSync。

这些旧路径是有意保留的兼容接口，不需要手动重命名或同时安装两套插件。
GitHub 旧地址会重定向到新仓库，2.1.0 发布记录保留。

## 验证和边界

发布检查覆盖本地回归、尺寸/DPI、无界面 QML 桥接、Steinberg VST3 校验、包内新名称及旧类 ID、隔离安装与卸载。
自动化测试不代替 MuseScore 实机验收；请先在谱面副本上验证旧工程载入与同步。

需要 Windows 10/11 x64 和 MuseScore 4。EXE 是插件安装器，不是独立播放器。
安装器未签名，请核对来源和 SHA256，并遵循本机安全策略。
不自动拉伸伴奏；反复展开、临时播放速度倍率、多 MuseScore 进程共享快照仍有限制。
本次仅更新 Windows 二进制，基于 DaweiDrumScore，按 GPL-3.0 发布。

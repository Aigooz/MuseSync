# MuseSync 谱伴同步

在 MuseScore 4 中，一边看谱，一边播放自己的伴奏。

MuseSync 是 Windows VST3 音频同步插件，配合 MuseSync Bridge 读取当前谱面的速度、拍号和小节信息。适合跟伴奏练习、对照音频看谱，以及调整伴奏与谱面的起始位置。

维护与定制：**@爱鸽咕子** · GitHub：**Aigooz**

[下载安装包](https://github.com/Aigooz/MuseSync/releases/latest) · [反馈问题](https://github.com/Aigooz/MuseSync/issues) · [更新记录](CHANGELOG.md)

## 下载

当前正式发布版本：**MuseSync 2.2.0**，同步组件：**MuseSync Bridge 1.3.0**。

打开 [Releases 发布页](https://github.com/Aigooz/MuseSync/releases/latest)，下载：

```text
MuseSync-Setup-2.2.0-Windows-x64.exe
```

**普通用户只需要 EXE。** 安装包包含 VST3、Bridge 和安装说明，不需要另外下载源码、SDK 或 ZIP。这个 EXE 是安装器，不是独立播放器，电脑上仍需安装 MuseScore 4。

需要手动部署时可选择 Windows ZIP；需要研究或修改代码时再下载 Source ZIP。可使用同一发布页的 `SHA256SUMS.txt` 核对文件。

> 本地开发中的改进不等于已经发布。中文安装向导、插件内更新下载、一键卸载及旧版 4.x 接口兼容等后续修改，以对应 Release 的说明和安装包为准，不代表现有 2.2.0 下载已经包含。

## 可以做什么

| 功能 | 用途 |
| --- | --- |
| 伴奏载入 | 载入 MP3、WAV 等音频，查看波形并跟随宿主播放 |
| 谱面同步 | 通过 Bridge 读取当前谱面的速度、拍号及变化 |
| 波形与小节网格 | 对照音频位置、小节线和播放头进行练习 |
| 起始偏移 | 对齐伴奏第一拍与谱面，无需先裁剪音频 |
| 播放进度与定位 | 查看进度，通过 Bridge 请求跳转到谱面位置 |
| 小节与拍数 | 以 `73-3` 表示第 73 小节、第 3 拍 |
| 最小化悬浮条 | 收起完整面板，保留位置显示与展开入口 |
| 固定时长变调 | 调整半音、音分，保持伴奏总时长 |
| 主题与使用指南 | 切换主题，通过“？”查看指南和关于信息 |

MuseSync 不会因为谱面变速而自动拉伸伴奏。同步网格与音频速度不是同一件事；伴奏本身需要与谱面的演奏安排匹配。

## 安装与启用

1. 保存谱面，退出 MuseScore 和其他正在使用插件的宿主。
2. 运行 EXE 安装器，按提示授予管理员权限。
3. 选择 MuseScore 4 的安装文件夹，例如 `D:\MuseScore 4`，其中应包含 `bin\MuseScore4.exe`。不要选择存放谱面的文件夹。
4. 安装完成后重新打开 MuseScore，在插件管理中启用 **MuseSync Bridge**。
5. 每次启动 MuseScore 后，从插件菜单运行一次 Bridge。看到“已开启”提示后，同步服务会在后台继续运行。
6. 在混音器中选择 **MuseSync** VST3 音源，保持对应通道未静音，打开面板并载入伴奏。
7. 开启“跟随谱面”，调整起始偏移，对齐伴奏与谱面后开始播放。

安装器尚未进行代码签名，可能出现 Windows SmartScreen 提示。请核对下载来源和校验值，并遵循本机安全策略，不要关闭系统防护。

## 版本兼容

| 环境 | 支持状态 |
| --- | --- |
| Windows 10 / 11 x64 | 当前维护与打包平台 |
| MuseScore 4.7 | 当前主要目标环境 |
| MuseScore 4.6 | 保留新版谱面分段接口路径，仍需实际宿主验证 |
| MuseScore 4.0–4.5 | 本地开发版已补充旧接口分支，通过接口回归测试，尚未逐版本实机验证或正式发布 |
| MuseScore 3.x | **不支持**，宿主本身不支持加载 VST3 |
| macOS / Linux | 本仓库当前不提供与 Windows 新版功能对等的安装包 |

不能将自动化测试视为对所有 4.x 补丁版本和所有谱面的兼容保证。音符之间的速度标记、渐变速度、反复展开和临时播放速度倍率仍可能造成同步偏差。

同机安装多个 MuseScore 4 时，Bridge 只部署到安装时选择的版本。换用另一个版本，需要重新运行安装器并选择对应目录。

## 更新

**覆盖安装无需先卸载。** 保存谱面并退出 MuseScore 后，从 [发布页](https://github.com/Aigooz/MuseSync/releases/latest) 下载新版 EXE，直接运行并安装到原位置即可。

安装器会复用原来的安装位置，替换插件与 Bridge。用户谱面、伴奏文件和已有同步 INI 不会被安装包覆盖；插件参数请先随谱面保存。如果 MuseScore 已移动到其他目录，需要在向导中重新选择。

插件内检查更新和下载安装包属于后续本地改进，是否可用请以所安装版本及其 Release 说明为准。无法连接 GitHub 时，可手动下载 EXE 更新。

## 常见问题

### 只把 EXE 发给别人就可以吗？

可以。Windows 用户不需要工程文件和 SDK，但仍需安装 MuseScore 4，并在安装后启用 Bridge 与 VST3。

### 装完后为什么没有自动同步？

检查 Bridge 是否已启用，并在本次 MuseScore 会话中运行。随后检查 VST3 面板的“跟随谱面”是否开启。Bridge 与 VST3 是两个配合工作的组件，不能只启用其中一个。

### 为什么没有声音？

检查是否已载入伴奏、插件所在通道是否静音、音量是否过低，以及 MuseScore 是否正在播放。空格键无反应时，先点击谱面区域，再使用 MuseScore 的播放控制。

### 为什么安装路径里还有 MuseTone？

MuseSync 曾使用 MuseTone 名称。为兼容旧谱面与覆盖升级，部分内部标识和路径保留旧名称，包括 `plugins\MuseToneSync` 与 `C:\Program Files\MuseTone`。不要自行重命名这些目录。

### 怎么反馈问题？

请在 [Issues](https://github.com/Aigooz/MuseSync/issues) 中提供 Windows 版本、MuseScore 版本、插件版本、复现步骤及截图。涉及特定谱面时，可提供不含隐私与受限内容的最小示例。

## 从源码构建

Windows 构建需要 Visual Studio C++ Build Tools、CMake、Git，以及完整的 VST3 SDK 与子模块。发布构建固定使用下列 SDK 提交：

```powershell
git clone https://github.com/Aigooz/MuseSync.git
cd MuseSync
git clone --recurse-submodules https://github.com/steinbergmedia/vst3sdk.git vst3sdk
git -C vst3sdk checkout 3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96
git -C vst3sdk submodule update --init --recursive
cmake -S . -B build -A x64 -DMUSETONE_BUILD_TESTS=ON
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
```

Windows 插件产物为 `build/VST3/Release/MuseSync.vst3`。安装 Inno Setup 6 与 PowerShell 7 后可以打包：

```powershell
pwsh -File tools/package-windows.ps1 -BuildDirectory build
```

## 关于与许可

MuseSync 谱伴同步由 **@爱鸽咕子** 维护与定制，本仓库不设置收款二维码或赞助入口。

本项目按 [GPL-3.0](LICENSE) 发布。代码来源、第三方依赖和相应版权说明集中在 [第三方声明](THIRD_PARTY_NOTICES.md)，源码中的必要声明予以保留；本项目不将第三方代码声明为原创。

MuseSync 与 MuseScore、Muse Group 及 Steinberg 无隶属或官方背书关系。相关商标归各自权利人所有。

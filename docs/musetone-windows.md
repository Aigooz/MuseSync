# MuseSync 谱伴同步 for Windows

当前发布版：**2.2.0**，同步组件：**MuseSync Bridge 1.3.0**。

原名 MuseTone。为兼容旧谱面，VST3 类 ID、参数状态、安装器身份及底层路径
`MuseTone.vst3` / `MuseToneSync` 保持不变。显示名称已统一更新，无需手动重命名文件。

## 下载与启用

普通用户下载 `MuseSync-Setup-2.2.0-Windows-x64.exe`，核对同一发布页的
`SHA256SUMS.txt` 后安装。此 EXE 是安装器，不是独立播放器。
需要 Windows 10/11 x64 和 MuseScore 4；安装前保存谱面并退出 MuseScore。

安装时选择包含 `bin\MuseScore4.exe` 的 MuseScore 目录，支持其他盘符。
安装器会同时部署 VST3 和 Bridge。重启后在插件管理中启用 `MuseSync Bridge`，
每次会话从插件菜单运行一次，出现已开启提示后可以关闭提示，后台同步仍继续。
在混音器的 VST3 音源列表选择 `MuseSync`，保持该通道未静音，然后载入伴奏。

安装器未签名，可能触发 SmartScreen，请确认来源并遵循本机安全策略。
安装器为 Sync 自身文件夹配置用户写入权限，以保存同步快照，其他插件目录不受影响。
程序说明和卸载入口位于 `C:\Program Files\MuseTone`。卸载前退出 MuseScore，
再从 Windows 设置的应用列表卸载；用户谱面、音频和其他插件不会被删除。

## 功能

MuseSync 是基于原 `DaweiDrumScore` 源码重做的 Windows VST3 插件，保留音频载入、波形、拍号网格、偏移和宿主时间轴同步，并增加：

- 半音：`-24` 到 `+24`，用于整曲移调。
- 音分：`-100` 到 `+100`，用于细微校音。
- 半音和音分可以同时设置，实际比例为 `2 ^ ((半音 * 100 + 音分) / 1200)`。
- 变调在后台线程完成，不阻塞实时音频回调。
- 变调结果保持原采样率、声道数、帧数和总时长。
- 面板获得焦点时按空格，会向宿主根窗口转发播放/暂停按键。

空格同步依赖宿主窗口接收空格快捷键。若宿主或 MuseScore 没有把焦点交给可处理播放命令的窗口，插件无法替宿主决定播放行为。

## 构建

需要 Visual Studio 2022 Build Tools、CMake 和 VST3 SDK：

```powershell
git clone --recurse-submodules https://github.com/steinbergmedia/vst3sdk.git vst3sdk
cmake -S . -B build-musetone -G "Visual Studio 17 2022" -A x64 -DVST3_SDK_DIR=.\vst3sdk
cmake --build build-musetone --config Release --parallel
```

发布版固定 VST3 SDK 提交 `3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96`，
SDK 子模块需完整。安装 Inno Setup 6 后，用 PowerShell 7 打包：

```powershell
pwsh -File tools/package-windows.ps1 -BuildDirectory build-musetone
```

生成 EXE、便携 ZIP、包含 SDK 的完整对应源码 ZIP、发布说明和 SHA256 校验文件。
Windows 使用静态 MSVC 运行库，无需单独安装 VC++ Redistributable。

VST3 bundle 通常位于：

```text
build-musetone/VST3/Release/MuseTone.vst3
```

标准安装目录为：

```text
C:\Program Files\Common Files\VST3\MuseTone.vst3
```

## 与原版共存

原版 `DaweiDrumScore.vst3` 和原版安装器位于 `dist/windows`，新版产物使用不同 bundle 名称和 UID，目标输出位于 `dist/windows-musetone`，不会覆盖原版文件。安装新版后，MuseScore 里显示为 `MuseSync`。从 MuseTone 升级则使用相同 UID 和路径，直接更新已有安装。

## 当前工程同步

部署脚本同时安装 `MuseSync Bridge` 到 MuseScore 的 plugins 目录。重启 MuseScore 后，
在插件管理中启用它，并在每次 MuseScore 会话中从插件菜单运行一次。
它在后台读取当前打开谱面的速度表、拍号和中途变化，不需要手动导入或保存谱子。
VST3 的「当前谱面」按钮可刷新连接；没有信息时保留默认网格并标注来源。
当前桥接只面向同一安装下单个 MuseScore 进程。MT3 快照读取每小节的
`timesigActual` 实际时值，支持弱起和不完整小节；显示编号使用 `no + 1`，
与内部连续小节索引分开。MuseScore 布局已将 `noOffset` 计入 `no`，不能重复叠加。
旧版 API 缺少实际时值时回退到标称拍号。
网格按速度表分段换算，包括小节内变速；BPM 和拍号框跟随当前播放位置。
进度条右侧及收起后的悬浮条显示「小节-拍数」，例如 `73-3`。
拍数以拍号分母计数，6/8 显示 1 至 6。反复展开和临时播放速度倍率仍有限制，
不自动拉伸伴奏音频。关闭「跟随谱面」后，手动速度与拍号设置保持独立。

## 悬浮窗口与主题

右上角收起后保留可拖动悬浮条，显示播放状态、时间和小节拍数。
展开按钮尽量与收起时的点击位置对齐，点击展开图标或双击悬浮条恢复完整面板。
收起不会停止音频。主题菜单提供九种配色，指南、波形和悬浮条同步使用主题。
问号按钮打开内置指南，「关于」区以小字标注 `@爱鸽咕子`。

## 音量、偏移与跳转

- 底部采用等宽的同步与移调两区，BPM、拍号、半音和音分输入框对齐；
  音量收为移调区下方的一行滑条，不再单独占一整栏。
  参数区减高 18 个逻辑像素，留给波形显示；宽窗口下限制参数区最大宽度，避免控件拉得过散。
- 拍号框的闭合外观跟随九种主题，下拉列表与键盘操作仍使用原生控件。
- 音量使用横向滑条，支持点击定位、拖动、滚轮和方向键，范围为 `0` 到 `100%`。
  百分比仅用于显示，不再需要输入数字。
- 起始偏移可直接输入毫秒，支持正负值，范围为 `-3600000` 到 `3600000 ms`。
  回车或离开输入框后应用；Ctrl / Alt 拖动波形时数值实时更新，Shift 可微调。
- 起始偏移增加 `-` / `+` 按钮和滚轮调节，默认每次 `10 ms`，按住 Shift 时为 `1 ms`。
- 半音、音分和手动 BPM 输入框支持滚轮，保留高精度滚轮的不足一格增量。
  半音和音分输入有效整数后立即请求变调，不需要先点击其他控件。
- 普通单击波形向新版 `MuseSync Bridge` 请求谱面跳转并开始播放；拖动仍平移视野。
  跳转先扣除起始偏移，再选择对应时间之前最近的音符或休止符，交由 MuseScore 播放。
  不单独启动伴奏，伴奏继续跟随宿主时间轴，避免两边不同步。
- 跳转使用 `MuseToneCommands.ini` 请求通道，并校验当前谱面 session 和请求有效期。
  需要启用并运行新版 `MuseSync Bridge`；优先使用 MuseScore 光标的谱面时间，
  旧接口回退到按速度积分。反复展开、临时播放速度倍率和旧接口下的变速仍有精度限制。

## 变调实现说明

音频文件解码后保存一份不可变原始快照。后台使用 Signalsmith Stretch 1.0.0
进行保持时长的变调，补偿算法延迟后发布结果，不改变谱面同步时间。
修复了原后台任务未记录算法成功返回值、导致非零移调结果始终无法发布的问题。

参数连续变化时先合并 90 ms 内的调整，再开始计算；旧任务按音频块取消，
以 generation 标记防止覆盖最后一次设置。处理时显示进度，恢复原调直接复用
原始快照，无需再次计算或复制整曲。长音频仍需后台处理，不是实时移调。
依赖源码、版本来源及 MIT 许可证位于 `source/third_party/signalsmith-stretch`。

## REA 静态分析边界

已对原 Windows PE 产物执行静态检查：PE32+ x64、导出入口、导入库和 Win32/Media Foundation/输入消息相关 API 均已记录。该环境没有可用的 REA MCP、IDA、Ghidra 或 Hopper，因此这是基于 PE 表和源码的静态分析，不等同于运行时追踪、动态调试或完整反编译；静态导入也不能单独证明某个 API 在每次运行中都会被调用。

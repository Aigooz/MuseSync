# Changelog

## 2.5.4 - 2026-11-10

- 整理视频小窗的谱面位置、进度、音量、延迟和不透明度控件布局。
- 增加视频小窗最小宽度，保留滑块的操作空间。

## 2.5.3 - 2026-11-10

- 在主界面和悬浮窗的小节拍数旁显示当前谱面 BPM。

## 2.5.2 - 2026-11-10

- Disable BPM analysis for online video; local audio and video analysis remains available.

## 2.5.1 - 2026-11-10

- Reduce floating video drag overhead and coalesce pending position updates.
- Avoid audio-thread allocations for double precision output and reduce video audio buffer lock contention.
- Skip waveform refresh requests while the waveform is hidden by video or the collapsed view.

## 2.5.0 - 2026-11-10

- Reuse the browser synchronization script and media element; coalesce outstanding script requests and compensate for dispatch latency.
- Smooth playback-rate estimates and correct small drift with bounded speed changes. Allow buffering and seeks to finish before retrying large drift correction.
- Add shared volume and delay controls to the video floating toolbar, with responsive layouts down to 280 x 180.
- Consume translated Enter/Space characters in numeric edits, reject whitespace and malformed numeric pastes, and retain the last valid value on invalid input.
- Delay wheel steps: 10 ms normally, 100 ms with Ctrl, and 1 ms with Shift.
- Default new instances and legacy states without a theme to dark; preserve explicitly saved themes.

## 2.4.2 - 2026-11-08

- 恢复原有控件编号，修正进度条回归测试失败。

## 2.4.1 - 2026-11-08

- Synchronize accessible video players with score playback, seeking and playback speed.
- Improve video URL entry and update download handling for slow connections.
- Video synchronization depends on the website player and has not been verified across all sites and hosts.

## 2.4.0 - 2026-11-08

Windows x64 release, with MuseSync Bridge 1.4.0.

- Add a separate WebView2 video window for local MP4, WebM and M4V files and http/https video pages, including Bilibili pages where site policies permit playback.
- Include the WebView2 SDK in Windows CI builds and document the WebView2 Runtime requirement.
- Fix player-window shutdown while its UI thread is starting, and limit the file picker to formats handled by the player.

The video window does not download online videos. Website login, regional availability, copyright restrictions and codec support may limit playback. MuseScore compatibility claims remain unchanged; automated validation does not replace host testing.

## 2.3.0 - 2026-11-02

Windows x64 release, with MuseSync Bridge 1.4.0.

- Add background update checks and a notification dot on the guide button. Download installers inside the plugin, with progress, cancellation and SHA-256 verification before launch.
- Localize the installer and installation instructions into Simplified Chinese. Reuse previous installation directories for in-place upgrades.
- Add one-click removal of the Bridge, VST3 and verified disabled legacy bundles, preserving user scores, audio, unrelated plugins and sync INI files.
- Rename the physical Windows bundle and module to MuseSync.vst3. Preserve class IDs and saved state; disable and archive the old MuseTone bundle during upgrades.
- Refresh typography, light/dark colors, waveform markers, floating position display and the built-in guide.
- Add legacy Qt 5 settings, numeric playback-time and multi-track tempo scanning paths for earlier MuseScore 4 APIs.

Compatibility: MuseScore 3 does not support VST3. Earlier MuseScore 4 API paths have regression coverage, not per-version host certification. Updates do not automatically time-stretch accompaniment or close MuseScore. The installer remains unsigned; macOS/Linux binaries are not part of this release.

## 2.2.0 - 2026-11-01

Rename the Windows plugin to MuseSync (谱伴同步), with MuseSync Bridge 1.3.0.

- Update the editor, floating strip, guide, startup popup, VST3 factory metadata and installer branding.
- Rename downloadable packages and the GitHub repository to MuseSync; keep prior releases.
- Preserve VST3 processor/controller IDs, saved parameters, bundle paths and bridge snapshot paths for upgrades from MuseTone.
- Retain floating playback controls, measure-beat display, nine themes and the About credit.

Compatibility: physical files remain MuseTone.vst3 and MuseToneSync. These are not duplicate plugins or stale builds. Restart MuseScore after upgrading both components. Existing limitations from 2.1.0 still apply.

## 2.1.0 - 2026-11-01

Windows x64 release, with MuseTone Sync 1.2.0.

- Follow mid-score tempo and time-signature changes, including tempo changes within measures.
- Display the current measure and beat beside the progress bar and in the floating strip.
- Show a startup confirmation when MuseTone Sync starts.
- Add a draggable floating strip, aligned expand control and playback progress seeking.
- Refresh the built-in guide, add the About credit, and provide nine themes.
- Include MuseTone Sync, instructions and license notices in the Windows EXE installer.
- Support custom MuseScore installation folders and statically link the MSVC runtime.

Validation: five regression suites, 343 layout/DPI combinations, headless QML startup and bridge tests, 47 Steinberg VST3 validator tests, and an isolated install/uninstall smoke test. The smoke installer uses the same payload and wizard logic with a separate per-user registry identity; the production administrator/UAC path still needs manual verification. These checks do not replace MuseScore end-to-end testing.

Known limits: this is a VST3 plugin, not a standalone player. No automatic accompaniment time-stretching. Repeat expansion and temporary playback-speed multipliers are not fully supported. The installer is unsigned. This release does not provide updated macOS/Linux binaries.

## 2.0.0

Initial local MuseTone Windows adaptation, including fixed-duration pitch shift, live score metadata, millisecond alignment controls and playback synchronization.

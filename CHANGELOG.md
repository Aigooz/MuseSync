# Changelog

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

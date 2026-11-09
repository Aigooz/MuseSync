# Third-Party Notices

MuseSync (formerly MuseTone) is a modification of the GPL-3.0 DaweiDrumScore project:
https://github.com/780566922/dawei-drumscore
The project license is provided in LICENSE. Original copyright notices are retained in source files.

## Signalsmith

- Signalsmith Stretch 1.0.0: MIT, source and license in source/third_party/signalsmith-stretch.
- Signalsmith DSP: MIT, source and license in source/third_party/signalsmith-stretch/dsp.

## Other Bundled Source

- MiniBPM: GPL-2.0-or-later, Copyright 2012-2025 Particular Programs Ltd.
  https://github.com/breakfastquay/minibpm/tree/43de9911eee6ebad20454732223dc141b73a9e6f
  Unmodified engine in source/third_party/minibpm; COPYING and README.md included.
  MuseSync applies it to overlapping eight-second windows, not a whole-track average.

- stb_vorbis: public domain or MIT, license text retained in source/third_party/stb_vorbis.c.
- dr_libs (Linux decoder sources): public domain or MIT-0, notices retained in the corresponding source files.
- miniz: MIT, supplied by the VST3 SDK's VSTGUI dependency; source notices are retained upstream.
- Steinberg VST3 SDK: license notices are provided in the pinned SDK source used to build the release.

SDK source: https://github.com/steinbergmedia/vst3sdk/tree/3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96
The release source archive includes the SDK and its submodules with their license files.

VST is a trademark of Steinberg Media Technologies GmbH. MuseScore and Windows are trademarks of their respective owners. No affiliation or endorsement is implied.

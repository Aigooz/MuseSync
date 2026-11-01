#include "win/ui_layout.h"
#include "win/numeric_input.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

bool overlaps (ap::ui::Rect a, ap::ui::Rect b)
{
    return a.x < b.right () && b.x < a.right () &&
           a.y < b.bottom () && b.y < a.bottom ();
}

bool inside (ap::ui::Rect a, ap::ui::Rect outer)
{
    return a.w > 0 && a.h > 0 && a.x >= outer.x && a.y >= outer.y &&
           a.right () <= outer.right () && a.bottom () <= outer.bottom ();
}

} // namespace

int main ()
{
    int cases = 0;
    for (const int width : {0, 760, 761, 800, 960, 1280, 1700})
        for (const int height : {0, 500, 501, 520, 560, 800, 1200})
            for (const double scale : {1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0})
            {
                const auto l = ap::ui::arrange (width, height);
                const auto guide = ap::ui::arrangeGuide (width, height);
                const ap::ui::Rect panel {0, 0, l.width, l.height};
                std::vector<ap::ui::Rect> guideBoxes {guide.title, guide.close, guide.about, guide.credit,
                    guide.updateStatus, guide.updateProgress, guide.updateCheck, guide.updateAction};
                for (auto section : guide.sections) guideBoxes.push_back (section);
                for (size_t i = 0; i < guideBoxes.size (); ++i)
                {
                    if (!inside (guideBoxes[i], panel)) return 1;
                    for (size_t j = i + 1; j < guideBoxes.size (); ++j)
                        if (overlaps (guideBoxes[i], guideBoxes[j])) return 1;
                }
                const std::vector<ap::ui::Rect> controls {
                    l.file, l.transport, l.time, l.progress, l.musicalPosition, l.wave, l.open, l.score, l.theme, l.help, l.collapse, l.followScore,
                    l.grid, l.numbers, l.zoomOut, l.zoomIn, l.zoomFit, l.follow,
                    l.syncTitle, l.pitchTitle, l.bpmLabel, l.bpmSource, l.timeSigLabel,
                    l.offsetLabel, l.semitoneLabel, l.centsLabel, l.volumeLabel,
                    l.bpm, l.timeSig, l.offset, l.offsetUnit, l.offsetReset,
                    l.semitone, l.cents, l.pitchReset, l.pitchStatus,
                    l.volumeValue, l.volumeSlider
                };
                auto scaled = [scale] (ap::ui::Rect r) {
                    return ap::ui::Rect {
                        static_cast<int> (std::lround (r.x * scale)),
                        static_cast<int> (std::lround (r.y * scale)),
                        static_cast<int> (std::lround (r.w * scale)),
                        static_cast<int> (std::lround (r.h * scale))
                    };
                };
                for (size_t i = 0; i < controls.size (); ++i)
                {
                    if (!inside (scaled (controls[i]), scaled (panel)))
                    {
                        std::printf ("Control %zu outside %dx%d at %.2f DPI scale\n",
                                     i, l.width, l.height, scale);
                        return 1;
                    }
                    for (size_t j = i + 1; j < controls.size (); ++j)
                        if (overlaps (scaled (controls[i]), scaled (controls[j])))
                        {
                            std::printf ("Controls %zu and %zu overlap at %dx%d / %.2f\n",
                                         i, j, l.width, l.height, scale);
                            return 1;
                        }
                }
                if (l.progress.w < 170 || l.progress.y != l.grid.y ||
                    l.progress.right () + 8 != l.musicalPosition.x ||
                    l.musicalPosition.right () + 12 != l.grid.x ||
                    l.wave.h < 200 || l.bpm.h != 32 || l.bpm.y != l.semitone.y ||
                    l.bpm.y != l.cents.y || l.bpm.y != l.timeSig.y ||
                    l.semitone.w - 62 < 50 || l.cents.w - 62 < 50 ||
                    l.bpm.w != l.semitone.w || std::abs (l.sync.w - l.pitch.w) > 1 ||
                    l.bpmSource.w < 92 || l.pitchStatus.w < 200 ||
                    l.volume.h != 32 || l.volume.w != 252 || l.volumeSlider.w != 160 ||
                    l.volume.y != l.offset.y || l.volumeValue.y != l.offset.y ||
                    l.parametersY != l.height - 164 ||
                    !inside (l.bpm, l.sync) || !inside (l.timeSig, l.sync) ||
                    !inside (l.semitone, l.pitch) || !inside (l.cents, l.pitch) ||
                    !inside (l.volume, l.pitch) || !inside (l.volumeSlider, l.volume) ||
                    !inside (l.volumeLabel, l.volume) || !inside (l.volumeValue, l.volume) ||
                    !inside (l.offset, l.sync) || !inside (l.offsetReset, l.sync) ||
                    !inside (l.pitchStatus, l.pitch) || !inside (l.pitchReset, l.pitch))
                {
                    std::printf ("Parameter alignment or minimum-size regression\n");
                    return 1;
                }
                ++cases;
            }
    std::printf ("PASS: %d layout / DPI combinations, bounds and overlap checks\n", cases);
    double value = 0;
    if (!ap::ui::parseNumericInput (L"-1250", -3600000, 3600000, value) || value != -1250 ||
        !ap::ui::parseNumericInput (L"+12.5", -3600000, 3600000, value) || value != 12.5)
        return 1;
    for (const auto* text : {L"", L"-", L"nan", L"inf", L"12ms", L"3600001", L"1e999"})
        if (ap::ui::parseNumericInput (text, -3600000, 3600000, value)) return 1;
    if (ap::ui::parseNumericInput (L"101", 0, 100, value)) return 1;
    return 0;
}

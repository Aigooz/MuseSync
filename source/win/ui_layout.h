#pragma once

#include <algorithm>

namespace ap::ui {

constexpr int defaultWidth = 800;
constexpr int defaultHeight = 520;
constexpr int minWidth = 760;
constexpr int minHeight = 500;

struct Rect
{
    int x, y, w, h;
    int right () const { return x + w; }
    int bottom () const { return y + h; }
};

struct Layout
{
    int width, height, parametersY;
    Rect file, transport, time, progress, musicalPosition, wave;
    Rect open, video, score, theme, help, collapse, followScore, grid, numbers, zoomOut, zoomIn, zoomFit, follow;
    Rect sync, pitch, volume;
    Rect syncTitle, pitchTitle, bpmLabel, bpmSource, timeSigLabel, offsetLabel;
    Rect semitoneLabel, centsLabel, volumeLabel;
    Rect bpm, timeSig, offset, offsetUnit, offsetReset;
    Rect semitone, cents, pitchReset, pitchStatus, volumeValue, volumeSlider;
};

struct GuideLayout
{
    Rect title, close, sections[6], about, credit, updateStatus, updateCheck, updateAction, updateProgress;
};

inline GuideLayout arrangeGuide (int width, int height)
{
    const int w = std::max (minWidth, width), h = std::max (minHeight, height);
    GuideLayout l {};
    l.title = {24, 18, w - 100, 30};
    l.close = {w - 56, 18, 32, 32};
    const int column = (w - 80) / 2;
    const int row = (h - 164) / 3;
    for (int i = 0; i < 6; ++i)
        l.sections[i] = {24 + (i / 3) * (column + 32), 76 + (i % 3) * row, column, row - 8};
    l.about = {24, h - 78, 210, 22};
    l.credit = {24, h - 46, 210, 18};
    l.updateStatus = {254, h - 78, w - 278, 22};
    l.updateProgress = {254, h - 42, w - 522, 4};
    l.updateCheck = {w - 252, h - 50, 108, 32};
    l.updateAction = {w - 132, h - 50, 108, 32};
    return l;
}

inline Layout arrange (int width, int height)
{
    Layout l {};
    l.width = std::max (minWidth, width);
    l.height = std::max (minHeight, height);
    l.parametersY = l.height - 164;

    l.file = {160, 12, l.width - 530, 23};
    l.transport = {160, 37, l.file.w, 18};
    l.score = {l.width - 370, 16, 78, 32};
    l.open = {l.width - 284, 16, 78, 32};
    l.video = {l.width - 198, 16, 78, 32};
    l.theme = {l.width - 112, 16, 28, 32};
    l.help = {l.width - 76, 16, 28, 32};
    l.collapse = {l.width - 40, 16, 28, 32};
    l.time = {20, 68, 120, 24};
    const int toolsX = l.width - 350;
    l.progress = {144, 66, toolsX - 240, 28};
    l.musicalPosition = {toolsX - 88, 66, 76, 28};
    l.grid = {toolsX, 66, 64, 28};
    l.numbers = {toolsX + 72, 66, 76, 28};
    l.zoomOut = {l.width - 164, 66, 32, 28};
    l.zoomIn = {l.width - 128, 66, 32, 28};
    l.zoomFit = {l.width - 92, 66, 32, 28};
    l.follow = {l.width - 56, 66, 36, 28};
    l.wave = {20, 100, l.width - 40, l.parametersY - 116};

    // Keep the parameter band balanced without stretching controls on wide hosts.
    const int parametersWidth = std::min (l.width - 40, 864);
    const int parametersX = (l.width - parametersWidth) / 2;
    const int sectionWidth = (parametersWidth - 32) / 2;
    l.sync = {parametersX, l.parametersY + 14, sectionWidth, 132};
    l.followScore = {l.sync.right () - 100, l.sync.y, 100, 24};
    l.pitch = {l.sync.right () + 32, l.sync.y, parametersWidth - sectionWidth - 32, 132};
    l.syncTitle = {l.sync.x, l.sync.y, 100, 24};
    l.pitchTitle = {l.pitch.x, l.pitch.y, 84, 24};
    l.pitchReset = {l.pitch.right () - 28, l.pitch.y, 28, 24};
    l.pitchStatus = {l.pitch.x + 92, l.pitch.y, l.pitch.w - 128, 24};

    const int fieldY = l.sync.y + 52;
    const int fieldWidth = (sectionWidth - 16) / 2;
    l.bpm = {l.sync.x, fieldY, fieldWidth, 32};
    l.timeSig = {l.bpm.right () + 16, fieldY, l.sync.w - fieldWidth - 16, 32};
    l.semitone = {l.pitch.x, fieldY, fieldWidth, 32};
    l.cents = {l.semitone.right () + 16, fieldY, l.pitch.w - fieldWidth - 16, 32};
    l.bpmLabel = {l.bpm.x, fieldY - 22, 32, 18};
    l.bpmSource = {l.bpm.x + 36, fieldY - 22, l.bpm.w - 36, 18};
    l.timeSigLabel = {l.timeSig.x, fieldY - 22, l.timeSig.w, 18};
    l.semitoneLabel = {l.semitone.x, fieldY - 22, l.semitone.w, 18};
    l.centsLabel = {l.cents.x, fieldY - 22, l.cents.w, 18};

    const int footerY = l.sync.y + 100;
    l.offsetLabel = {l.sync.x, footerY, 64, 32};
    l.offset = {l.sync.x + 72, footerY, l.sync.w - 134, 32};
    l.offsetUnit = {l.offset.right () + 6, l.offset.y, 20, 32};
    l.offsetReset = {l.sync.right () - 28, footerY, 28, 32};
    l.volume = {l.pitch.right () - 252, footerY, 252, 32};
    l.volumeLabel = {l.volume.x, footerY, 32, 32};
    l.volumeSlider = {l.volume.x + 40, footerY, 160, 32};
    l.volumeValue = {l.volume.right () - 44, footerY, 44, 32};
    return l;
}

} // namespace ap::ui

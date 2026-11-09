#include "scorebridge.h"
#include "video_timeline.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {
void check (bool condition, const char* message)
{
    if (!condition) throw std::runtime_error (message);
}
}

int main ()
{
    try
    {
        ap::ScoreSyncSnapshot snapshot;
        double selection = -1;
        check (ap::parseScoreSelection ("MP1|1000|1000-1|12.375", "1000-1", 2000, selection) &&
               selection == 12.375, "paused selection position");
        for (const auto* value : {"MP1|1000|1000-1|-1", "MP1|1000|1000-1|nan",
             "MP1|1000|1000-1|86401", "MP1|1000|other|1", "MP1|2001|1000-1|1",
             "MP1|1000|1000-1|1|extra", "MP1|1000|1000-1|", "MP1|-1|1000-1|1"})
            check (!ap::parseScoreSelection (value, "1000-1", 2000, selection), "invalid selection accepted");
        check (!ap::parseScoreSelection ("MP1|1000|1000-1|1", "1000-1", 16001, selection),
               "stale selection rejected");
        ap::VideoTimeline video;
        check (video.update (1, true, false, true, 2) == 2, "initial stopped selection");
        check (video.update (1, true, false, true, 3) == 3, "selection seeks with unchanged paused host");
        check (video.update (4, true, true, true, 3) == 4, "playback uses host instead of old selection");
        check (video.update (5, true, false, true, 3) == 5, "pause does not jump back to old selection");
        check (video.update (5, true, false, true, 6) == 6, "new paused selection follows immediately");
        check (video.update (7, true, false, true, 6) == 7, "native host seek supersedes selection");
        check (video.update (7, true, false, true, 8) == 8, "another selected measure");
        check (video.update (7, true, false, false, 0) == 7, "expired bridge selection cleared");
        check (ap::parseScoreSyncSnapshot ("MT1|1000|90.000000|7|8|3,3,4;4,4,4", 2000, snapshot), "valid snapshot");
        check (snapshot.bpm == 90 && snapshot.beats == 7 && snapshot.denominator == 8 &&
               snapshot.timeSigChanges.size () == 2, "snapshot metadata");
        check (ap::parseScoreSyncSnapshot ("MT1|1000|120|4|4|", 16000, snapshot), "no meter changes / expiry boundary");
        const char* invalid[] = {
            "MT1|1000|120|4|4|", "MT1|-1|120|4|4|", "MT1|18446744073709551616|120|4|4|",
            "MT1| 1000|120|4|4|", "MT1|2001|120|4|4|", "MT1|1000||4|4|", "MT1|1000|nan|4|4|",
            "MT1|1000|0|4|4|", "MT1|1000|1001|4|4|", "MT1|1000|120|33|4|", "MT1|1000|120|4|3|",
            "MT1|1000|120|4|4|3,3,4;2,4,4", "MT1|1000|120|4|4|2,3,4;2,4,4",
            "MT1|1000|120|4|4|1,3,4", "MT1|1000|120|4|4|2,3,4,extra", "MT1|1000|120|4|4|extra|",
            "MT1|1000|120|4|4|2,3,4\n", "MT1|1000|120|4|4|;", "MT1|1000|120|4|4",
            "MT1|1000|120|4|4|2,3,4,", "MT1|1000|120|4|4|2,3,4;"
        };
        for (std::size_t i = 0; i < sizeof invalid / sizeof *invalid; ++i)
            check (!ap::parseScoreSyncSnapshot (invalid[i], i == 0 ? 16001 : 2000, snapshot), "invalid snapshot accepted");

        std::vector<ap::BarMark> bars;
        ap::buildBars (0.75, 4.5, 0, 0.5, 4, 4, {}, bars);
        check (bars.size () == 3 && bars.front ().startSec == 0, "partly visible bar retained for beat lines");
        ap::buildBars (0, 8, 0, 0.5, 4, 4, {{3, 3, 4}}, bars);
        check (bars.size () == 5 && bars[2].changed && bars[2].beats == 3 &&
               std::abs (bars[3].startSec - 5.5) < 1e-8, "meter change grid geometry");
        ap::buildBars (-0.75, 0.25, 0, 0.5, 4, 4, {}, bars);
        check (bars.size () == 2 && bars.front ().bar == 0, "partly visible bar before offset");
        check (ap::parseScoreSyncSnapshot ("MT2|1000|120|4|4|3,3,4|1,1,0;2,4,1;3,2,2|1000-1", 2000, snapshot), "pickup snapshot");
        check (snapshot.measures.size () == 3 && snapshot.session == "1000-1", "pickup duration / session");
        ap::buildBars (0, 3.4, .25, .5, 4, 4, snapshot.timeSigChanges, bars, snapshot.measures);
        check (bars.size () == 4 && bars[1].beats == 4 && bars[1].durationSec == .5 &&
               bars[1].number == 0 && bars[2].startSec == .75 && bars[2].number == 1 &&
               bars[3].startSec == 2.75 && bars[3].durationSec == 1, "pickup / middle incomplete bar preserves nominal meter and grid origin");
        for (const auto* text : {"MT2|1000|120|4|4||1,0,0|1000-1", "MT2|1000|120|4|4||1,nan,0|1000-1",
             "MT2|1000|120|4|4||2,1,0;1,1,0|1000-1", "MT2|1000|120|4|4|||bad", "MT2|1000|120|4|4|||1000-1|extra"})
            check (!ap::parseScoreSyncSnapshot (text, 2000, snapshot), "invalid pickup accepted");
        check (ap::parseScoreSyncSnapshot ("MT3|1000|120|4|4|2,3,4||1000-1|2,60;5,180", 2000, snapshot),
               "tempo change snapshot");
        check (snapshot.tempos.size () == 2 && snapshot.tempos[1].quarter == 5 && snapshot.tempos[1].bpm == 180,
               "tempo map fields survive bridge");
        for (const auto* tempo : {"2,0", "2,nan", "2,60;2,90", "5,60;2,90", "-1,60", "2,60;", "2,60|extra"})
            check (!ap::parseScoreSyncSnapshot (std::string ("MT3|1000|120|4|4|||1000-1|") + tempo, 2000, snapshot),
                   "invalid tempo map accepted");
        check (ap::parseScoreSyncSnapshot ("MT3|1000|120|4|4|||1000-1|", 2000, snapshot), "empty tempo map");
        const std::vector<ap::TempoChange> tempos {{2, 60}, {5, 180}};
        ap::buildBars (.25, 6, .25, .5, 4, 4, {{2, 3, 4}}, bars, {}, tempos);
        check (std::abs (bars[1].startSec - 3.25) < 1e-8 &&
               std::abs (bars[1].durationSec - 5.0 / 3) < 1e-8 &&
               std::abs (bars[0].beatOffsets[3] - 2) < 1e-8,
               "tempo changes within measures alter bars and beat lines, with offset");
        ap::GridMetadata grid;
        grid.tempos = tempos;
        grid.changes = {{2, 3, 4}};
        auto position = ap::musicalPosition (grid, 4);
        check (position.bar == 2 && position.beat == 2 && position.bpm == 180 && position.beats == 3,
               "current tempo, meter and beat change together on exact boundary");
        position = ap::musicalPosition (grid, 3 - 1e-6);
        check (position.bar == 1 && position.beat == 4 && position.bpm == 60, "before meter boundary");
        grid.tempos.clear ();
        grid.changes.clear ();
        position = ap::musicalPosition (grid, 145);
        check (position.bar == 73 && position.beat == 3, "73-3 in 4/4");
        grid.beats = 6;
        grid.denominator = 8;
        position = ap::musicalPosition (grid, .75);
        check (position.beat == 4, "6/8 counts denominator beats");
        grid.beats = grid.denominator = 4;
        grid.measures = {{1, 1, 0}, {2, 4, 73}};
        position = ap::musicalPosition (grid, 1.5);
        check (position.bar == 73 && position.beat == 3, "pickup and printed measure numbers");
        std::puts ("PASS: bridge parser, expiry, malformed input and partially visible grid / meter changes");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf (stderr, "FAIL: %s\n", e.what ());
        return 1;
    }
}

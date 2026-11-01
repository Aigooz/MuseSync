function readScore(score, division) {
    if (!score || !score.firstMeasure) return null;
    var cursor = score.newCursor();
    cursor.filter = -1;
    cursor.rewind(0);
    var directSegments = score.firstMeasure.firstSegment && typeof cursor.rewindToFraction === "function";
    if (directSegments) cursor.rewindToFraction(score.firstMeasure.firstSegment.fraction);
    var bpm = Number(cursor.tempo) * 60;
    if (!isFinite(bpm) || bpm < 1 || bpm > 1000) return null;

    var changes = [];
    var measures = [];
    var initial = null;
    var previous = null;
    var measure = score.firstMeasure;
    var bar = 1;
    while (measure) {
        if (bar > 100000) return null;
        var fraction = measure.timesigNominal;
        if (!fraction) return null;
        var beats = Number(fraction.numerator);
        var denominator = Number(fraction.denominator);
        if (beats < 1 || beats > 32 || beats % 1 !== 0 ||
                [2, 4, 8, 16].indexOf(denominator) < 0) return null;
        var meter = beats + "," + denominator;
        if (!initial) initial = { beats: beats, denominator: denominator };
        else if (meter !== previous) changes.push(bar + "," + meter);
        if (changes.length > 10000) return null;
        var actual = measure.timesigActual || fraction;
        if (!isFinite(actual.numerator) || actual.numerator <= 0 ||
                !isFinite(actual.denominator) || actual.denominator <= 0) return null;
        var quarters = 4 * Number(actual.numerator) / Number(actual.denominator);
        // MuseScore's layout has already applied noOffset to this zero-based no.
        var number = Number(measure.no) + 1;
        if (!isFinite(number) || number % 1 !== 0) number = bar;
        if (quarters !== 4 * beats / denominator || number !== bar)
            measures.push(bar + "," + quarters.toFixed(9) + "," + number);
        previous = meter;
        measure = measure.nextMeasure;
        ++bar;
    }
    var tempos = [], points = [];
    division = Number(division) || 480;
    // next() skips segments without an element in the current track. Position
    // directly on each score segment instead (MuseScore 4.6+), without editing.
    var tracks = directSegments ? 1 : Math.min(Number(score.ntracks) || 1, 1024);
    var guard = 0;
    for (var track = 0; track < tracks; ++track) {
        if (!directSegments) {
            cursor = score.newCursor();
            cursor.filter = -1;
            cursor.track = track;
            cursor.rewind(0);
        }
        var segment = directSegments ? score.firstMeasure.firstSegment : cursor.segment;
        var lastTick = -1;
        while (segment) {
            if (++guard > 1000000) return null;
            if (directSegments) cursor.rewindToFraction(segment.fraction);
            var tick = Number(cursor.tick), tempo = Number(cursor.tempo) * 60;
            if (!isFinite(tick) || tick < 0 || tick < lastTick || !isFinite(tempo) || tempo < 1 || tempo > 1000)
                return null;
            points.push({tick: tick, tempo: tempo, order: points.length});
            lastTick = tick;
            if (directSegments) segment = segment.next;
            else {
                if (!cursor.next()) break;
                segment = cursor.segment;
            }
        }
    }
    points.sort(function(a, b) { return a.tick - b.tick || a.order - b.order; });
    if (points.length && points[0].tick === 0) bpm = points[0].tempo;
    // Collapse duplicate segments before comparing tempos (also deterministic on Qt 5).
    var lastTempo = bpm;
    for (var i = 0; i < points.length; ++i) {
        while (i + 1 < points.length && points[i + 1].tick === points[i].tick) ++i;
        tick = points[i].tick;
        tempo = points[i].tempo;
        if (Math.abs(tempo - lastTempo) > 0.000001) {
            var point = (tick / division).toFixed(9) + "," + tempo.toFixed(6);
            tempos.push(point);
            lastTempo = tempo;
        }
    }
    return { bpm: bpm, beats: initial.beats, denominator: initial.denominator,
             changes: changes.join(";"), measures: measures.join(";"), tempos: tempos.join(";") };
}

function encodeSnapshot(metadata, now, session) {
    if (!metadata) return "";
    var text = "MT3|" + Math.floor(now) + "|" + metadata.bpm.toFixed(6) + "|" +
            metadata.beats + "|" + metadata.denominator + "|" + metadata.changes + "|" +
            metadata.measures + "|" + session + "|" + (metadata.tempos || "");
    // Hex keeps QSettings' INI escaping of commas/semicolons out of the protocol.
    var hex = "";
    for (var i = 0; i < text.length; ++i)
        hex += ("0" + text.charCodeAt(i).toString(16)).slice(-2);
    return hex;
}

function parseSeekCommand(text, session, now) {
    var parts = String(text).split("|");
    if (parts.length !== 5 || parts[0] !== "MC1" || !session || parts[3] !== session ||
            !/^[0-9]+-[0-9]+$/.test(parts[1]) || !/^[0-9]+$/.test(parts[2]) ||
            !/^[0-9]+(\.[0-9]+)?$/.test(parts[4])) return null;
    var stamp = Number(parts[2]);
    var seconds = Number(parts[4]);
    if (stamp > now || now - stamp > 3000 || !isFinite(seconds) || seconds > 86400) return null;
    return { id: parts[1], seconds: seconds };
}

// Select a single note/rest, never a staff range (which can change playback muting).
// The independent cursor reads tempo at each event and does not edit score content.
function selectPlaybackPosition(score, seconds, division) {
    if (!score || !score.selection || !(division > 0) ||
            !isFinite(seconds) || seconds < 0) return false;
    var best = null;
    var bestTime = -1;
    var tracks = Math.min(Number(score.ntracks) || 1, 1024);
    for (var track = 0; track < tracks; ++track) {
        var cursor = score.newCursor();
        cursor.track = track;
        cursor.rewind(0);
        var useScoreTime = typeof cursor.time === "function";
        var previousTick = 0, elapsed = 0, tempo = Number(cursor.tempo);
        var guard = 0;
        while (cursor.segment && ++guard <= 1000000) {
            var tick = Number(cursor.tick);
            if (!isFinite(tick) || tick < previousTick) return false;
            if (useScoreTime) elapsed = Number(cursor.time(false)) / 1000;
            else if (typeof cursor.time === "number") elapsed = cursor.time / 1000;
            else {
                if (!(tempo > 0)) return false;
                elapsed += (tick - previousTick) / division / tempo;
            }
            if (!isFinite(elapsed) || elapsed < 0) return false;
            if (elapsed > seconds + 0.000001) break;
            var element = cursor.element;
            if (element && elapsed >= bestTime) {
                best = element.notes && element.notes.length ? element.notes[0] : element;
                bestTime = elapsed;
            }
            previousTick = tick;
            tempo = Number(cursor.tempo);
            if (!cursor.next()) break;
        }
    }
    return best !== null && score.selection.select(best, false);
}

function localFilePath(url) {
    var path = decodeURIComponent(String(url));
    if (path.indexOf("file:///") === 0) path = path.slice(8);
    else if (path.indexOf("file://") === 0) path = "//" + path.slice(7);
    return path;
}

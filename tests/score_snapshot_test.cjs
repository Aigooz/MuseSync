const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const script = fs.readFileSync(path.join(__dirname, '../companion/MuseToneSync/ScoreSnapshot.js'), 'utf8');
const api = vm.createContext({});
vm.runInContext(script, api);

function score(tempo, meters) {
    let next = null;
    for (let i = meters.length - 1; i >= 0; --i) {
        const [numerator, denominator, actualN = numerator, actualD = denominator, no = i, noOffset = 0] = meters[i];
        next = { timesigNominal: { numerator, denominator },
            timesigActual: { numerator: actualN, denominator: actualD }, no, noOffset, nextMeasure: next };
    }
    return { firstMeasure: next, newCursor: () => ({ tempo, rewind: mode => assert.equal(mode, 0) }) };
}
assert.equal(api.readScore(null), null);
assert.equal(api.readScore(score(NaN, [[4, 4]])), null);
assert.equal(api.readScore(score(2, [[4, 3]])), null);
const opening = api.readScore(score(1.5, [[7, 8], [7, 8], [3, 4], [4, 4]]));
assert.equal(opening.bpm, 90);
assert.equal(opening.beats, 7);
assert.equal(opening.denominator, 8);
assert.equal(opening.changes, '3,3,4;4,4,4');
assert.equal(Buffer.from(api.encodeSnapshot(opening, 1000, '1000-1'), 'hex').toString(),
    'MT3|1000|90.000000|7|8|3,3,4;4,4,4||1000-1|');
const pickup = api.readScore(score(2, [[4, 4, 1, 4, -1], [4, 4, 4, 4, 0], [3, 4, 2, 4, 1]]));
assert.equal(pickup.beats, 4);
assert.equal(pickup.changes, '3,3,4');
assert.equal(pickup.measures, '1,1.000000000,0;2,4.000000000,1;3,2.000000000,2');
const numberedPickup = api.readScore(score(2, [[4, 4, 1, 8, -1, -1], [4, 4, 4, 4, 0],
    [4, 4, 4, 4, 10, 9]]));
assert.equal(numberedPickup.measures, '1,0.500000000,0;2,4.000000000,1;3,4.000000000,11');
assert.equal(api.readScore(score(2, [[4, 4, 4, 4, 9, 9]])).measures,
    '1,4.000000000,10', 'no already contains the custom number offset');
const legacyScore = score(2, [[3, 4]]);
delete legacyScore.firstMeasure.timesigActual;
delete legacyScore.firstMeasure.noOffset;
assert.equal(api.readScore(legacyScore).measures, '');
const invalidActual = score(2, [[4, 4, 0, 4]]);
assert.equal(api.readScore(invalidActual), null);
assert.equal(api.readScore(score(2.5, [[6, 8]])).bpm, 150);
assert.equal(api.encodeSnapshot(null, 1000), '');
assert.equal(api.localFilePath('file:///D:/MuseScore%204/plugins/MuseToneSync.ini'),
    'D:/MuseScore 4/plugins/MuseToneSync.ini');
assert.equal(api.localFilePath('file://server/share/bridge.ini'), '//server/share/bridge.ini');
assert.equal(api.parseSeekCommand('MC1|123-1|1000|100-1|1.25', '100-1', 1100).seconds, 1.25);
for (const text of ['MC1|123-1|1000|other|1', 'MC1|123-1|1000|100-1|-1',
    'MC1|123-1|1000|100-1|NaN', 'MC1|123-1|9999|100-1|1', 'MC1|123-1|1|100-1|1'])
    assert.equal(api.parseSeekCommand(text, '100-1', 4000), null);
const events = [{tick: 0, tempo: 2, element: {notes: ['note0']}},
    {tick: 480, tempo: 1, element: {notes: ['note1']}},
    {tick: 960, tempo: 1, element: {rest: true}}];
const changing = score(2, [[4, 4], [3, 4], [3, 4]]);
changing.newCursor = () => {
    const points = [{tick: 0, tempo: 2}, {tick: 960, tempo: 1}, {tick: 1920, tempo: 1},
        {tick: 2400, tempo: 3}];
    let i = 0;
    return {rewind() {i = 0;}, next() {return ++i < points.length;},
        get segment() {return i < points.length;}, get tick() {return points[i].tick;},
        get tempo() {return points[i].tempo;}};
};
const changingMetadata = api.readScore(changing, 480);
assert.equal(changingMetadata.tempos, '2.000000000,60.000000;5.000000000,180.000000');
assert.equal(changingMetadata.changes, '2,3,4');
const segmentScore = score(2, [[4, 4], [3, 4]]);
let segment = null;
for (const [tick, tempo] of [[2400, 3], [960, 1], [960, 1.25], [0, 2]])
    segment = {tick, tempo, fraction: {tick, tempo}, next: segment};
segmentScore.firstMeasure.firstSegment = segment;
segmentScore.newCursor = () => {
    let point = {tick: 0, tempo: 2};
    return {rewind() {}, rewindToFraction(fraction) {point = fraction;},
        next() {throw Error('track-filtered cursor must not be used');},
        get tick() {return point.tick;}, get tempo() {return point.tempo;}};
};
assert.equal(api.readScore(segmentScore, 480).tempos,
    '2.000000000,60.000000;5.000000000,180.000000',
    'tempo between notes/on other tracks and duplicate-tick segments');
let selected;
const playable = {ntracks: 1, selection: {select: (element, add) => {selected = element; assert.equal(add, false); return true;}},
    newCursor() {
        let i = 0;
        return {rewind() { i = 0; }, next() { return ++i < events.length; },
            get segment() { return i < events.length; }, get tick() { return events[i].tick; },
            get tempo() { return events[i].tempo; }, get element() { return events[i].element; }};
    }};
assert.equal(api.selectPlaybackPosition(playable, 0.49, 480), true);
assert.equal(selected, 'note0');
assert.equal(api.selectPlaybackPosition(playable, 1.25, 480), true);
assert.equal(selected, 'note1');
assert.equal(api.selectPlaybackPosition(playable, 1.5, 480), true);
assert.equal(selected.rest, true);
const scoreTimed = { ...playable, ntracks: 2,
    newCursor() {
        let track = 0, i = 0;
        const tracks = [events, [events[2]]];
        return {set track(value) {track = value;}, rewind() {i = 0;},
            next() {return ++i < tracks[track].length;},
            get segment() {return i < tracks[track].length;},
            get tick() {return tracks[track][i].tick;},
            get tempo() {return tracks[track][i].tempo;},
            get element() {return tracks[track][i].element;},
            time(repeats) {assert.equal(repeats, false); return tracks[track][i].tick * 2;}};
    }};
assert.equal(api.selectPlaybackPosition(scoreTimed, 1.1, 480), true);
assert.equal(selected, 'note1', 'score engine time is authoritative across tempo changes and tracks');
assert.equal(api.selectPlaybackPosition(scoreTimed, 2, 480), true);
assert.equal(selected.rest, true);
for (const seconds of [-1, NaN, Infinity])
    assert.equal(api.selectPlaybackPosition(scoreTimed, seconds, 480), false);
assert.equal(api.selectPlaybackPosition({ntracks: 1, selection: playable.selection,
    newCursor: () => ({rewind() {}, segment: null})}, 1, 480), false);
console.log('PASS: current-score producer, pickup duration, printed numbering, legacy API, seek selection and INI encoding');

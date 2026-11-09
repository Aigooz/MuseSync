const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const assert = require('node:assert/strict');
const header = fs.readFileSync(path.join(__dirname, '../source/win/video_script.h'), 'utf8');
const script = header.match(/LR"JS\(([\s\S]*?)\)JS"/)[1];
const video = {
    videoWidth: 640, readyState: 4, duration: 120, currentTime: 0,
    playbackRate: 1, paused: true, controls: true,
    getBoundingClientRect: () => ({width: 640, height: 360}),
    hasAttribute: () => true,
    play() { this.paused = false; return Promise.resolve(); },
    pause() { this.paused = true; }
};
const state = {t: 30.25, playing: false, rate: 1, local: true, at: 0, seek: true};
const window = {
    __museSyncPlayback: state,
    __museSyncAudio: {sources: new WeakMap([[video, {}]]), context: {state: 'running'}}
};
const document = {
    querySelectorAll: selector => selector === 'video' ? [video] : [],
    getElementById: () => ({})
};
let now = 0;
const context = vm.createContext({window, document, performance: {now: () => now}});
const run = () => vm.runInContext(script, context);
assert.equal(run(), 120, 'duration is reported for progress and time labels');
assert.equal(video.currentTime, 30.25, 'paused seek updates video immediately');
assert.equal(video.paused, true, 'seeking does not start playback');
state.seek = false;
state.t = 30.29;
run();
assert.equal(video.currentTime, 30.29, 'small paused seeks are not swallowed by playback drift tolerance');
state.t = 200;
run();
assert.equal(video.currentTime, 120, 'seek past end clamps to duration');
state.t = -2;
run();
assert.equal(video.currentTime, 0, 'negative offset clamps to start');
state.t = 40;
state.playing = true;
run();
assert.equal(video.currentTime, 40);
assert.equal(video.paused, false, 'playback resumes after seeking');
state.playing = false;
state.t = 12.5;
run();
assert.equal(video.currentTime, 12.5);
assert.equal(video.paused, true, 'pause and seek applied together');
state.local = false;
state.t = 12.55;
run();
assert.equal(video.currentTime, 12.55, 'online players use precise paused seeking too');
video.readyState = 0;
state.t = 80;
assert.equal(run(), 0);
video.readyState = 4;
run();
assert.equal(video.currentTime, 80, 'pending seek reapplies when video metadata becomes available');
console.log('PASS: video duration, precise paused seeks, playback transitions and metadata retry');

let pcmMessage;
let audioNode;
class AudioContext {
    sampleRate = 48000;
    state = 'running';
    destination = {};
    createScriptProcessor() { return audioNode = {connect() {}}; }
    createMediaElementSource() { return {connect() {}}; }
}
delete window.__museSyncAudio;
window.chrome = {webview: {postMessage: value => pcmMessage = value}};
Object.assign(context, {
    AudioContext, Uint8Array, DataView, Float32Array,
    btoa: value => Buffer.from(value, 'binary').toString('base64')
});
Object.assign(state, {t: 15, rate: 1.5, playing: true, generation: 42});
run();
audioNode.onaudioprocess({inputBuffer: {
    length: 2, numberOfChannels: 1,
    getChannelData: () => Float32Array.of(.25, -.5)
}});
const pcm = Buffer.from(pcmMessage.slice('MuseSyncPCM:'.length), 'base64');
assert.equal(pcm.length, 40, 'PCM packet includes media-time header and stereo samples');
assert.equal(pcm.readUInt32LE(0), 42, 'packet tags seek generation');
assert.equal(pcm.readUInt32LE(4), 48000, 'packet tags actual context sample rate');
assert.equal(pcm.readDoubleLE(8), 15, 'packet tags media time for BPM segment placement');
assert.equal(pcm.readDoubleLE(16), 1.5, 'packet tags playback speed for media BPM correction');
assert.equal(pcm.readFloatLE(24), .25);
assert.equal(pcm.readFloatLE(28), .25, 'mono duplicates into stereo');
assert.equal(pcm.readFloatLE(32), -.5);
assert.equal(pcm.readFloatLE(36), -.5);
console.log('PASS: browser PCM sample rate, generation, media timestamps, speed and sample layout');

Object.assign(state, {t:15.2, at:0, rate:1, playing:true, local:false, seek:false});
video.currentTime = 15;
run();
assert.equal(video.currentTime, 15, 'small playing drift does not flush the decoder');
assert.ok(video.playbackRate > 1 && video.playbackRate <= 1.05, 'small lag uses bounded catch-up speed');
state.t = 14.8;
run();
assert.ok(video.playbackRate < 1 && video.playbackRate >= .95, 'video ahead slows down gently');
video.readyState = 2;
state.t = 30;
run();
assert.equal(video.currentTime, 15, 'network buffering does not cause repeated seeks');
video.readyState = 4;
now = 3000;
state.at = now;
run();
assert.equal(video.currentTime, 30, 'large drift resynchronizes after buffering');
state.t = 35;
run();
assert.equal(video.currentTime, 30, 'seek cooldown avoids a decoder seek loop');
state.seek = true;
run();
assert.equal(video.currentTime, 35, 'explicit score seek bypasses drift cooldown');
state.seek = false;
video.readyState = 0;
state.t = 50;
state.seek = true;
run();
state.seek = false;
video.readyState = 4;
run();
assert.equal(video.currentTime, 50, 'seek received before metadata is retained');
let scans = 0;
const query = document.querySelectorAll;
document.querySelectorAll = selector => { ++scans; return query(selector); };
run();
run();
assert.equal(scans, 0, 'steady playback reuses connected media and throttles audio scans');
console.log('PASS: smooth drift correction, buffering recovery, seek cooldown, pending seeks and media cache');

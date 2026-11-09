#pragma once

namespace ap {

// Executed in the document and its frames; leave site media pipelines intact.
inline constexpr wchar_t kVideoPlayerScript[] = LR"JS(
(window.__museSyncUpdate || (window.__museSyncUpdate = () => {
    const state = window.__museSyncPlayback;
    if (!state) return -1;
    window.__museSyncPendingSeek = window.__museSyncPendingSeek || state.seek;
    const largest = selector => Array.from(document.querySelectorAll(selector))
        .filter(e => {
            const r = e.getBoundingClientRect();
            return r.width > 80 && r.height > 45 || e.videoWidth > 0;
        }).sort((a, b) => {
            const x = a.getBoundingClientRect(), y = b.getBoundingClientRect();
            return y.width * y.height - x.width * x.height;
        })[0];
    const cached = window.__museSyncMedia;
    const reusable = cached && cached.element.isConnected !== false &&
        (cached.video || performance.now() - cached.at < 1000);
    const video = reusable ? cached.video : largest('video');
    const frame = video ? null : reusable ? cached.element : largest('iframe');
    const media = video || frame;
    if (!media) return -1;
    if (!reusable) window.__museSyncMedia = {element:media, video, at:performance.now()};

    let style = document.getElementById('__museSyncStyle');
    if (!style) {
        style = document.createElement('style');
        style.id = '__museSyncStyle';
        style.textContent = `
            html,body { background:#111!important; margin:0!important;
                width:100%!important; height:100%!important; overflow:hidden!important; }
            body * { visibility:hidden!important; }
            [data-musesync-screen] { visibility:visible!important; position:fixed!important;
                inset:0!important; width:100%!important; height:100%!important;
                max-width:none!important; max-height:none!important; min-width:0!important;
                min-height:0!important; margin:0!important; padding:0!important;
                border:0!important; object-fit:contain!important; background:#111!important;
                transform:none!important; z-index:2147483647!important; }
            video::-webkit-media-controls { display:none!important; }
        `;
        (document.head || document.documentElement).appendChild(style);
    }
    if (!media.hasAttribute('data-musesync-screen')) {
        document.querySelectorAll('[data-musesync-screen]').forEach(e => e.removeAttribute('data-musesync-screen'));
        media.setAttribute('data-musesync-screen', '');
        // A transformed/contained ancestor otherwise traps position:fixed.
        for (let p = media.parentElement; p && p !== document.documentElement; p = p.parentElement) {
            for (const [key, value] of Object.entries({transform:'none', filter:'none',
                perspective:'none', contain:'none', overflow:'visible', clip:'auto',
                'clip-path':'none', opacity:'1', display:'block'}))
                p.style.setProperty(key, value, 'important');
        }
    }
    if (!video) return -1;
    const tracking = window.__museSyncTracking || (window.__museSyncTracking = {lastSeek:-Infinity, playing:false});
    tracking.seek = tracking.seek || window.__museSyncPendingSeek;
    window.__museSyncPendingSeek = false;
    video.controls = false;
    if (video.readyState < 1) {
        video.preload = 'auto';
        if (state.playing) video.play()?.catch(() => {});
        return 0;
    }

    let bridge = window.__museSyncAudio;
    if (!bridge) {
        const context = new AudioContext({sampleRate:48000, latencyHint:'interactive'});
        bridge = window.__museSyncAudio = {context, sources:new WeakMap()};
        const node = context.createScriptProcessor(2048, 2, 2);
        bridge.node = node;
        node.onaudioprocess = event => {
            const current = window.__museSyncPlayback;
            if (!current || !current.playing || !window.chrome?.webview) return;
            const input = event.inputBuffer;
            const n = input.length;
            const bytes = new Uint8Array(24 + n * 8);
            const header = new DataView(bytes.buffer);
            header.setUint32(0, current.generation, true);
            header.setUint32(4, context.sampleRate, true);
            header.setFloat64(8, bridge.video.currentTime, true);
            header.setFloat64(16, bridge.video.playbackRate, true);
            const samples = new Float32Array(bytes.buffer, 24);
            const l = input.getChannelData(0);
            const r = input.getChannelData(input.numberOfChannels > 1 ? 1 : 0);
            for (let i = 0; i < n; ++i) { samples[i*2] = l[i]; samples[i*2+1] = r[i]; }
            let binary = '';
            for (let i = 0; i < bytes.length; i += 4096)
                binary += String.fromCharCode(...bytes.subarray(i, i+4096));
            window.chrome.webview.postMessage('MuseSyncPCM:' + btoa(binary));
        };
        // The callback output is silent. Media sources only feed the PCM bridge.
        node.connect(context.destination);
    }
    bridge.video = video;
    const scanAudio = !bridge.scannedAt || performance.now() - bridge.scannedAt >= 2000;
    const elements = scanAudio ? [video, ...document.querySelectorAll('audio')] : [video];
    if (scanAudio) bridge.scannedAt = performance.now() || 1;
    for (const element of elements) {
        if (!bridge.sources.has(element)) {
            try {
                const source = bridge.context.createMediaElementSource(element);
                source.connect(bridge.node);
                bridge.sources.set(element, source);
            } catch (error) { console.error('MuseSync audio routing', error); }
        }
        if (element.volume !== 1) element.volume = 1;
        if (element.muted) element.muted = false;
    }
    if (state.playing && bridge.context.state === 'suspended') bridge.context.resume().catch(() => {});
    const requested = state.t + (state.playing ? Math.max(0, performance.now() - state.at) / 1000 * state.rate : 0);
    const target = Math.max(0, Number.isFinite(video.duration) ? Math.min(requested, video.duration) : requested);
    const drift = target - video.currentTime;
    const now = performance.now();
    // Let an in-flight seek/buffer refill finish. Small drift is corrected by
    // bounded rate changes, avoiding repeated decoder flushes on slow players.
    const hardSeek = tracking.seek || (state.playing && !tracking.playing) ||
        (!state.playing && Math.abs(drift) > .01) ||
        (state.playing && !video.seeking && video.readyState >= 3 &&
         Math.abs(drift) > (state.local ? .6 : 1.2) && now - tracking.lastSeek >= 2000);
    if (hardSeek) {
        try { video.currentTime = target; } catch (_) {}
        tracking.lastSeek = now;
        tracking.seek = false;
    }
    tracking.playing = state.playing;
    const correction = state.playing && !hardSeek && !video.seeking && video.readyState >= 3 && Math.abs(drift) > .04 ?
        Math.max(-.05, Math.min(.05, drift * .15)) : 0;
    const rate = Math.max(.25, Math.min(4, state.rate * (1 + correction)));
    if (Math.abs(video.playbackRate - rate) > .005) {
        try { video.playbackRate = rate; } catch (_) {}
    }
    if (state.playing && video.paused) video.play()?.catch(() => {});
    else if (!state.playing && !video.paused) video.pause();
    return Number.isFinite(video.duration) ? video.duration : 0;
}))()
)JS";

} // namespace ap

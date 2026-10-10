// Open Halo 2 (web): sound.
//
// The program's sound chip model mixes 48 kHz stereo into a ring inside its own
// (shared) memory: see the __EMSCRIPTEN__ branch of src/apu/apu_xaudio2.c for
// the layout. An AudioWorklet processor reads that ring on the audio thread.
// Browsers start audio only after a key or a click, so the context is created
// on the first one (or at once where the browser allows it).
(function () {
  'use strict';
  const PROCESSOR = `
    class H2Ring extends AudioWorkletProcessor {
      constructor(o) {
        super();
        const p = o.processorOptions;
        this.head = new Uint32Array(p.sab, p.ptr, 4);
        this.cap = this.head[2];
        this.pcm = new Int16Array(p.sab, p.ptr + 16, this.cap * 2);
        this.primed = false;
      }
      process(inputs, outputs) {
        const L = outputs[0][0], R = outputs[0][1] || outputs[0][0], n = L.length;
        const w = Atomics.load(this.head, 0); let r = Atomics.load(this.head, 1);
        const avail = (w - r) >>> 0;
        if (!this.primed) { if (avail < 2048) return true; this.primed = true; }
        if (avail < n) { Atomics.add(this.head, 3, 1); this.primed = false; return true; }
        const mask = this.cap - 1;
        for (let i = 0; i < n; i++) {
          const at = ((r + i) & mask) * 2;
          L[i] = this.pcm[at] / 32768; R[i] = this.pcm[at + 1] / 32768;
        }
        Atomics.store(this.head, 1, (r + n) >>> 0);
        return true;
      }
    }
    registerProcessor('h2-ring', H2Ring);`;
  let started = false;
  async function start() {
    if (started || typeof Module === 'undefined' || !Module.h2ready) return;
    started = true;
    try {
      const ctx = new AudioContext({ sampleRate: 48000, latencyHint: 'interactive' });
      const url = URL.createObjectURL(new Blob([PROCESSOR], { type: 'text/javascript' }));
      await ctx.audioWorklet.addModule(url);
      const node = new AudioWorkletNode(ctx, 'h2-ring', {
        numberOfInputs: 0, outputChannelCount: [2],
        processorOptions: { sab: HEAPU8.buffer, ptr: Module._xa2_web_ring() },
      });
      node.connect(ctx.destination);
      if (ctx.state !== 'running') await ctx.resume();
      Module.h2audio = ctx;
      console.log('page: audio ' + ctx.state + ' at ' + ctx.sampleRate + ' Hz');
    } catch (e) { started = false; console.log('page: audio failed: ' + e); }
  }
  for (const ev of ['keydown', 'mousedown', 'touchstart']) addEventListener(ev, start);
  const t = setInterval(() => { if (started) clearInterval(t); else if (navigator.userActivation ? navigator.userActivation.hasBeenActive : false) start(); }, 1000);
  // where autoplay is allowed (the headless test browser), start as soon as the program is up
  const t2 = setInterval(() => { if (started) { clearInterval(t2); return; } if (typeof Module !== 'undefined' && Module.h2ready && Module.h2autoplay) start(); }, 500);
})();

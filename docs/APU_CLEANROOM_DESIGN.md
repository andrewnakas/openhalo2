# Clean-room MCPX APU for Open Halo 2: design

Status: design only, 2026-10-16. Nothing here is implemented. Roadmap item: M2 in
`C:/Users/andre/.claude/plans/wild-squishing-haven.md` ("our own APU").

## 0. Clean-room ground rules for whoever implements this

* **Forbidden inputs.** These files are xemu-derived (LGPL-2.1-or-later), and the
  implementer must never open, grep the contents of, or quote them:
  `third_party/xboxrecomp/src/apu/{apu_core.c, apu_vp.c, apu_dsp.c, apu_state.h, apu_regs.h, apu_debug.h, fpconv.h, apu_shim.h, apu.h}`
  and `src/nv2a/qemu_shim.h`. The same applies to xemu and Cxbx-Reloaded sources online.
* **Tainted side files. Treat these as behaviour only.**
  * `src/apu/README.md` describes the xemu files.
  * `src/apu/CMakeLists.txt` says "Extracted from xemu".
  * `tests/apu_mixdown/test_main.c` includes `apu_state.h`.
  * STATUS.md decisions 34, 42, 43 and 44, plus the 2026-10-11 lead "Audio breaks up like Halo CE's" (STATUS line ~260). These cite internal function names, field names and a line of `apu_regs.h`.

  Use only the measured numbers from them: rates, bins, step sizes, the coefficient sign convention, and the addresses in the title.
* **Who may implement.** Earlier Halo 2 sessions edited the xemu files, so they and their memory notes are not clean. The implementer is a fresh session. It gets only this document, the facts sheet produced in step 3, the public sources in section 2, and our own call sites.
* **Naming.** New code uses its own names (prefix `ma_`, "MCPX audio"). The contract symbols in section 1 are kept only as thin exported shims. They must not reuse register or field identifiers from the forbidden headers. Names on XboxDevWiki pages are public and may be cited.
* **Measurement is allowed. Reading is not.** It is allowed to log the oracle's register traffic and guest RAM, and to record its output WAVs. It is not allowed to read its source.

## 1. The contract: what our code needs from the APU module

The symbol names below come from our call sites only: `port/xr/src/main.c`, `mmio_direct.c`, `port/web/main_web.c`, `src/audio/dsound_device.c` and `templates/new-game/src/main.c`. They were checked against `dumpbin /symbols xbox_apu.lib`, using exported and imported names only.

### 1a. Called by the port (required)

| Symbol | Signature | Semantics |
|---|---|---|
| `g_apu_state` | `MCPXAPUState *` (opaque; callers only forward-declare `struct MCPXAPUState`) | Non-NULL means the model is live. `mmio_direct.c` and the VEH path test it before routing anything. |
| `mcpx_apu_init_standalone` | `MCPXAPUState *(uint8_t *ram_ptr)` | `ram_ptr` = host pointer of guest physical 0 (`xbox_GetMemoryBase()`). Starts the model's frame thread and opens the host output (XAudio2, the web ring, or none with `RECOMP_APU_MUTE=1`). Returns NULL on failure. Called once, only when `RECOMP_AC97_READY` is set, and before `xbox_kernel_init()`. |
| `mcpx_apu_mmio_read` | `uint64_t (MCPXAPUState *, uint64_t off, unsigned size)` | `off` = VA − 0xFE800000, in 0 .. 0x2FFFF only (`APU_TRAP_END` = 0xFE830000 in `mmio_direct.c`). Size is 1, 2 or 4. Must answer what DirectSound polls, such as status, idle bits and the sample counter. |
| `mcpx_apu_mmio_write` | `void (MCPXAPUState *, uint64_t off, uint64_t val, unsigned size)` | Same range as the read. Covers register writes and the PIO method FIFO. |
| `apu_hook_handle_mmio` | `bool (PCONTEXT, uintptr_t fault_addr, uint32_t xbox_va, int is_write)` | x64 page-fault path (main.c VEH). Decodes the instruction, calls the read or write above, advances RIP, and returns true. It lives in `apu_mmio_hook.c`, which is toolkit code but depends on `apu.h`. Rewrite it on `platform/mmio_decode.h`, as `usb/ohci.c` does. The direct path measured 0 faults (STATUS P2a), so this is a fallback only. |
| `mcpx_apu_shutdown` | `void (MCPXAPUState *)` | Stops the thread, flushes the WAV tap, and closes the device. |

### 1b. Software mixer used by `src/audio/dsound_device.c` (must link; Halo 2 does not use it)

`int apu_mixer_alloc_voice(void)` (slot 0..63, or −1), `void apu_mixer_free_voice(int)`, `APUMixerVoice *apu_mixer_get_voice(int)`, `void apu_mixer_play(int slot, int looping)`, `void apu_mixer_stop(int)`, `int apu_mixer_set_position(int slot, DWORD byte_pos)` (0 means OK), and `void apu_mixer_get_state(int slot, DWORD *cursor, int *active, int *looping)`. All pointer arguments are optional.

The `APUMixerVoice` fields used are `pcm_data` (const int16_t*), `pcm_bytes`, `play_offset`, `num_channels`, `sample_rate` and `volume` (linear float). The behaviour is 16-bit PCM with 16.16 resampling to 48 kHz, mixed into the same output. This is a small, original piece of code.

### 1c. Runtime services the model consumes (all ours, in `src/kernel`)

* `xbox_GetConnectedInterrupt(vector)` returns the KINTERRUPT, with the routine at +0 and the context at +4.
* `xbox_IrqlEnterInterrupt(level)` / `xbox_IrqlLeaveInterrupt(saved)` go around the ISR. Also used: `xbox_IrqlBlocksInterrupts()` and `xbox_IrqlDispatchLock` / `Unlock`.
* `xbox_AllocThreadTib()`, `xbox_worker_stack_alloc()` / `_free(slot)` and `recomp_lookup(va)` let the ISR run on the model's host thread.
* `xbox_TimerThreadWake()` is used after a DPC is queued.

`usb/ohci.c` (original code) is the template for delivering an ISR this way.

### 1d. Host output (keep as is: `apu_xaudio2.c/.h` is our code, and its header does not say xemu)

`xa2_init`, `xa2_shutdown`, `xa2_is_active`, `xa2_submit_samples(int16 stereo, n)`, `xa2_get_buffer_size`, `xa2_get_stats`, and the `__EMSCRIPTEN__` ring `_xa2_web_ring` read by `port/web/audio.js`. Blocks are 256 stereo samples at 48 kHz (187.5 per second) and the queue is bounded at 16.

### 1e. Environment switches the port sets or documents (keep the names)

* `RECOMP_APU_MUTE`
* `RECOMP_APU_WAV=<file>[,s]` (the mix tap)
* `RECOMP_APU_BINS=1` (energy per bin)
* `RECOMP_APU_VOICES=1` (voice report)
* `RECOMP_APU_GAIN_DB` (also `volume_db` in halo2.ini)
* `RECOMP_APU_LIMIT=0`
* `RECOMP_APU_HRTF=0` and `RECOMP_APU_HRTF_GAIN=raw|l1`
* `RECOMP_APU_MIXDOWN_ALL=2`
* `RECOMP_APU_DSP_ACK_GPS=0x810` (acknowledge the GP doorbell at GP-scratch offset 0x810)
* `RECOMP_APU_DSP_SENTINEL=0x387108,0xCCCCCC` (holds the word DirectSound checks at 0xFE85A018)

The last two are workarounds for the title's DSP handshakes. They stay in approach A and disappear in approach B.

### 1f. Behaviour the title depends on (measured, STATUS)

* The sample counter must advance in real time, and pauses must be waited out. The title stops it around voice-list edits (SECTL 0xF→0x7→0xF).
* 187.5 blocks per second exactly. 0 DSP resets per frame, which needs the sentinel.
* Doorbell commands must be acknowledged, or DirectSound spins at 0x37E38E / 0x37E3F3.
* Voices play at 44,098, 37,082 and 48,000 Hz.
* Bins in use: 0, 1, 2, 6, 7 and 10, plus 23–25 (effect sends) and 31 (3D source bin).
* The 3D chain is source → bin 31 → 3D multipass voice with an HRTF filter → bins 6/8 and 7/9.
* HRTF coefficient bytes are sign-magnitude.
* Four FXIN voices play the DSP output back into bins 6–9. They are silent today.

## 2. Two approaches

### A. Hardware-level APU model (voice processor fed by the title's own DSOUND)

The title's statically linked DSOUND library (558 functions, 0x379C60–0x38710A per `halo2-decompiled/config/functions.csv`) keeps running unchanged. We model what it programs: registers, the PIO FIFO, voice descriptors in RAM, SGE/SSL tables, notifiers and the frame interrupt.

**Public information**
* XboxDevWiki APU, https://xboxdevwiki.net/APU
  * VP/GP/EP split.
  * 48 kHz, 32-sample frames, 1500 frames/s.
  * 256 voices, 64 of them 3D.
  * Formats 8/16/24-bit and ADPCM.
  * 8 target bins per voice with individual volumes.
  * 0x80-byte voice structures in VPV; memory regions VPSGE / VPSSL / VPHT / VPHC.
  * 2D, 3D and MP voice lists.
  * Pitch is s16 log2 relative to 48 kHz, 4096 steps per octave.
  * Two DAHDSR envelopes with 12-bit rates and an 8-bit sustain level, counted at 1500 Hz.
  * Initial envelope state set by `NV1BA0_PIO_VOICE_ON`.
  * DLS2 low-pass coefficient formulas; parametric-EQ and I3DL2 filter modes.
  * GP MIXBUF of 0x400 words = 32 bins × 0x20 words of 24-bit samples.
  * GP output by DMA to a scratch ring of 6 × 0x200 words.
  * The page has no field-level descriptor layout and no register map.
* XboxDevWiki Xbox ADPCM, https://xboxdevwiki.net/Xbox_ADPCM
  * IMA tables, format code 0x0069.
  * 36-byte mono and 72-byte stereo blocks.
  * 65 samples per channel per block: header sample plus 64 nibbles.
  * Stereo interleaves in 4-byte groups.
* XboxDevWiki MCPX / DSP / ACI, https://xboxdevwiki.net/MCPX and https://xboxdevwiki.net/DSP
  * Location of the memory windows; the GP/EP are DSP56300-class.
* DLS Level 2.2 (MIDI Manufacturers Association): envelope and low-pass semantics.
* IASIG I3DL2 guideline: reverb parameter set and semantics.
* NVIDIA nForce APU technical brief (linked from the XboxDevWiki APU page) and https://www.nvidia.com/en-us/drivers/apu : feature-level only.
* JayFoxRox/xbox-tools, https://github.com/JayFoxRox/xbox-tools : APU inspection scripts and an ADPCM decoder. No license was shown on GitHub, so cite it for facts only and copy nothing. Check its provenance before relying on it.

**What must be measured black-box.** The facts sheet from step 3 must cover:
1. The register map in our own names. For each register: which offsets the title writes and reads, the value sequences, and which read-back bits it waits on. The trace comes from `mmio_direct.c`.
2. PIO methods: the method offsets, their arguments, and their effect on voice lists, descriptors and sample-counter state.
3. Descriptor fields:
   * format, container, channels
   * buffer base via SGE index, start/loop/end and current positions
   * pitch
   * the 8 bin selectors and volumes
   * envelope rates and levels, filter mode and coefficients
   * HRTF handle, list link, stream / SSL state
   * which fields the APU writes back (current position, envelope state, the "active" bit)

   Method: correlate DSOUND API calls with descriptor diffs. The callers in the CC0 decomp give the arguments, for example `IDirectSoundBuffer_SetFrequency(f)` changing exactly one 16-bit word by 4096·log2(f/48000).
4. SGE format (page-table entries, physical addresses) and the SSL stream segment lists with their packet-completion notifier writes. DirectSound completes `IDirectSoundStream` packets from these. Music and dialogue are streams in Halo 2 (`sound_stream_create`, decomp 0x2AE660).
5. The interrupt: its vector (from `KeConnectInterrupt` in the kernel log), which status bits the ISR reads and clears, and how often it fires.
6. Mixer order: the MP list reads bins after the 2D and 3D lists have written them. Bin 31 must be complete before the 3D voices run.
7. Headroom: `IDirectSound_SetMixBinHeadroom(bin, 0..7)` (decomp) mapped to a per-bin shift. The output gain stage explains the "hot" mix (STATUS refine (e)).
8. ADPCM: samples consumed per block (64 or 65). Find it by streaming a known encoded ramp through a probe voice and counting oracle output. STATUS and the wiki disagree on the wording ("header + 63 nibbles" against 65 samples per block), so measure it and do not assume either.

**Effort:** about 100 h (step list in section 3).

**Risk**
* Medium. The descriptor and FIFO semantics are entirely black-box. Mitigation: the probe harness and the in-process shadow comparison make every field testable.
* The clean-room risk is real: the people around this code have seen xemu. Mitigation: the fresh-session rule and the facts-sheet hand-off.
* Gain: the title's own 3D, doppler, stream timing, voice stealing and XHV/WMA paths stay exact, because DSOUND computes them. Output matches the oracle structurally (same descriptors in, same bins out).

### B. HLE of the title's statically linked DirectSound

Replace the DSOUND entry points with host C, the way kernel imports are bridged. Halo 2 game code uses about 45 distinct entry points (counted in the decomp):
* `IDirectSound_*`: Create(Sound)Buffer/Stream, SetMixBinHeadroom, SetPosition, SetOrientation, SynchPlayback, SetEffectData, GetEffectData
* `IDirectSoundBuffer_*`: Play, Pause, SetVolume, SetMixBins, SetMixBinVolumes, SetFilter, SetI3DL2Source, Set{Min,Max}Distance, SetRolloff/Doppler/Cone, SetHeadroom, SetBufferData, SetCurrentPosition, Lock, GetStatus, Release
* `IDirectSoundStream_*`: Pause, SetEG, FlushEx, SetHeadroom, SetMixBins (and Process, via the codec)
* `XAudioDownloadEffectsImage`, `DirectSoundDoWork`

The xvoice (XHV, `CVoiceChatMode::DSoundInitialize`) and wmadec libraries call into DSOUND as well, possibly through internal methods.

**Public information**
* Microsoft Learn DirectSound / DirectSound3D (PC): distance model, cone, doppler and buffer semantics. https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee416960(v=vs.85)
* The I3DL2 and DLS2 specifications above.
* The CC0 decomp for the game-side usage, flags (`DSBCAPS_MIXIN`, `FXIN`, `FXIN2`, `CTRL3D`) and mixbin numbering.
* The Xbox-specific API (mixbins, SetEG, SetFilter, packets, effects images) is documented only in the non-public XDK. Its semantics would come from the decomp plus black-box observation.
* The DS3D HRTF tables sit inside DSOUND's data. In a BYOG build they could be read from the user's XBE at run time. Otherwise a public HRTF set would be needed.

**What must be measured**
* The VA of every entry point. Find them from decompiled callers (`@retail` addresses) with `tools/xdis.py`; all 558 DSOUND functions are unnamed.
* Calling conventions (stdcall, thiscall, or LTCG-custom).
* Every struct layout passed in: DSBUFFERDESC, DSSTREAMDESC, DSMIXBINS, DSENVELOPEDESC, DSFILTERDESC, XMEDIAPACKET, and the effect-data offsets.
* Stream packet-completion timing and status/event semantics.
* The title's 3D falloff and HRTF selection, matched against the oracle per position.
* What `GetEffectData` must return.

Logging the arguments on entry is easy. Matching DSOUND's internal 3D math and timing is the hard part.

**Effort:** about 180–250 h. **Risk:** high. The semantic surface is large and partly XDK-only. A mismatch in packet completion or GetStatus shows up as stalls or missing sounds in places that are hard to reach (cutscenes, voice chat, attract mode). The title's 3D mix would be re-implemented rather than reproduced. The lifter also has no direct-call replacement pass yet: a new `h2lift` pass that rewrites the bodies of `sub_00379xxx..0038710A` would be needed, because `recomp_lookup_manual` only catches indirect calls.

**Benefits:** the AC'97 and DSP handshake hacks go away, and there is no 1500 Hz frame thread.

**Not decisive:** portability. The direct-MMIO path already runs in wasm (STATUS 62).

### Recommendation: A

The hardware is a fixed-function mixer with far fewer semantics than the DirectSound API. The title's DSOUND keeps doing the hard, title-visible parts: 3D, streams, timing and the XHV/WMA routing.

The oracle comparison is exact under A. Both models can run in one process on the same register stream (shadow mode below), so every difference points at a field, not at a game-timing artefact.

The roadmap goal "match today's output on retail data" is met most directly this way. B stays available later as a separate performance project.

## 3. Design for approach A

### 3a. Module layout

The new static library is `xbox_mcpx_audio` in `third_party/xboxrecomp/src/mcpxaudio/`. It replaces `xbox_apu` in the umbrella target. The CMake changes come later, after the gate.

| File | Contents |
|---|---|
| `mcpx_audio.h` | Public contract (section 1). Defines `struct MCPXAPUState` as our own state type. |
| `ma_regs.c` | Register file shadow over 0..0x2FFFF. Read-back logic (status, idle, sample counter). PIO FIFO decode into `ma_cmd` operations. |
| `ma_mem.c` | Guest-physical access, SGE page walk, SSL segment walk, notifier writes. |
| `ma_voice.c` | List walk (2D, 3D, MP). Per-voice fetch, decode, resample, envelope, filter, and bin accumulation. Descriptor write-back. |
| `ma_adpcm.c` | Xbox ADPCM block decoder (IMA tables from the wiki), mono and stereo, with a per-voice block cache. |
| `ma_hrtf.c` | The 3D voice's FIR from descriptor coefficients (sign-magnitude bytes). Gain mode raw or l1, plus the RMS-unity default. |
| `ma_mix.c` | 32 bins × 32 samples per frame in float. Headroom shifts. Mixdown to stereo (layout per the public mixbin numbering: 0–5 FL FR C LFE BL BR, 6–9 3D crosstalk, 10 I3DL2 send). |
| `ma_fx.c` | GP/EP stand-in: doorbell acknowledge at GPS 0x810, the sentinel word, host I3DL2 reverb on bin 10, and the look-ahead limiter. |
| `ma_thread.c` | Frame clock: 8 frames make one 256-sample block, paced to 187.5 blocks/s. Waits out counter pauses and makes the time up. Interrupt delivery in the ohci.c pattern. WAV and bin taps, stats, `[APU]` report lines. |
| `ma_softmix.c` | The `apu_mixer_*` software mixer (1b). |
| `ma_mmio_hook.c` | Replaces `apu_mmio_hook.c`, built on `mmio_decode.h`. |
| `ma_shadow.c` | Dev builds only (`MA_SHADOW`): feeds every access to both the oracle and the new model, and taps both outputs. It is never in a release. |

Keep `apu_xaudio2.c/.h` and move them under the new library.

### 3b. Data structures (in our own terms)

```c
typedef struct ma_voice_cache {      /* host-side, per hardware voice index 0..255 */
    uint32_t desc_pa;                /* where its descriptor lives */
    double   pos;                    /* fractional source position */
    float    hist[4][2];             /* Hermite history, per channel */
    int16_t  blk[2][65]; int blk_id; /* decoded ADPCM block cache */
    float    env_level[2]; int env_stage[2]; uint16_t env_count[2];
    float    flt_state[2][4];        /* biquad / DLS2 low-pass state */
    float    fir_hist[2][32];        /* HRTF history */
} ma_voice_cache;

struct MCPXAPUState {                /* opaque to callers */
    uint8_t *ram;                    /* guest physical 0 */
    uint32_t reg[0x30000 / 4];       /* shadow of written values */
    uint32_t list_head[3];           /* 2D, 3D, MP */
    uint32_t vpv_pa, sge_pa, ssl_pa, notif_pa, hrtf_pa;  /* from measured registers */
    uint32_t sample_counter; int running;   /* counter state as the title sees it */
    float    bin[32][32];            /* one frame */
    ma_voice_cache vc[256];
    ma_reverb rv; ma_limiter lim;
    ma_stats st; ma_tap tap;
};
```

### 3c. Measurement plan

The oracle is the current build: the xemu model, run as a black box.

1. **Register trace.** Add `RECOMP_APU_TRACE=<file>` to `mmio_direct.c`, which is our file and the point where every APU access already passes. Write binary records: {sample counter at the time, VA, value, size, read or write}. For reads, record the value the oracle returned.
2. **RAM snapshots.**
   * Identify the descriptor, SGE, SSL, notifier and HRTF base addresses from the trace: values written once at init that point into contiguous memory.
   * Dump those regions every N frames and at every PIO write (`RECOMP_APU_SNAP`).
   * Use `tools/peekmem.py` for ad-hoc reads of a live process.
3. **API correlation.**
   * Log DSOUND entry calls with their arguments. Find the VAs via decompiled callers and `xdis.py`, and wrap them in the `abi_wrap.py` style (`[DSAPI]` lines).
   * A per-call diff of trace and snapshots names every field.
4. **Probe harness.** Add `RECOMP_APU_PROBE=1`. After `DirectSoundCreate`, a host routine calls the title's own lifted DSOUND functions with swept arguments, on a dedicated buffer holding a known test signal: ramp, sine, impulse, and an ADPCM ramp encoded by `halo2-cleanroom/games/halo2/codecs.py`. The sweeps cover:
   * frequency (pitch word)
   * volume and bin volumes
   * SetMixBins
   * SetEG (envelope words and timing at 1500 Hz)
   * SetFilter
   * SetHeadroom
   * 3D positions on a circle (HRTF coefficients)
   * play, stop and loop (positions and write-back)
   * stream packets (SSL and notifier)

   This gives about 200 short, labelled captures.
5. **Output capture.**
   * `RECOMP_APU_WAV` (mix) and `RECOMP_APU_BINS=1` (per-bin energy lines).
   * Per-bin WAVs, if needed, are added to the oracle by a dirty-side session. Only the WAV files are handed over.
6. **Scenes.**
   * boot to menu: music stream, 2D
   * retail `01a_tutorial` armory: ambience, 3D and footsteps (`tools/ingame.sh`)
   * retail Ivory Tower multiplayer SMG fire (`tools/combat/mpfire.sh`)

   Each scene runs 3 times to see run-to-run spread.
7. **Shadow mode** (the main comparison).
   * One process, `MA_SHADOW=1`. The oracle answers reads; the new model sees the same writes and the same RAM.
   * Both mix every frame, and both are tapped to `oracle.wav` and `new.wav`, plus per-bin float dumps.
   * `tools/apu_ab.py` (new) reports differences per frame and per voice, with the first diverging voice and field.
   * Last stage: let the new model answer reads (`MA_SHADOW=2`) to prove the title runs on it alone.

### 3d. Acceptance criteria (retail Ivory Tower and armory; all must hold)

* **Shadow mode, same input**
  * Voice set: active voices per frame equal in ≥ 99.9% of frames.
  * Per-bin RMS in 1 s windows within ±0.5 dB for bins above −60 dBFS (0, 1, 2, 6, 7, 10, 23–25, 31).
  * Mix correlation ≥ 0.98 for 2D-only stretches (menu music) and ≥ 0.95 for 3D stretches.
* **Pitch**
  * Source samples consumed per output sample within 0.1% of the oracle per voice (44,098 / 37,082 / 48,000 Hz voices).
  * Spectral peak of probe sines within 0.25%.
* **Continuity**
  * `tools/clicks.py` no worse than oracle + 0.05/s (the oracle armory on retail maps reads about 0.23/s).
  * No periodic click spacing at 32, 64 or 65 samples (frame or ADPCM block).
  * `wavcheck.py`: inner silent runs under 100 ms ≤ oracle, clipped samples 0.
* **Timing**
  * 187.5 ± 0.3 blocks/s, 0 drops, ≤ 1 underrun.
  * 0 DSP resets (sentinel), 0 doorbell spins, 0 counter-pause seconds lost.
* **3D**
  * A fixed source swings L/R between about −15 and +16 dB as the view turns (±1 dB of the oracle).
  * L/R correlation positive.
* **Cost**
  * Armory frame rate within 0.5 fps of the oracle.
  * APU thread CPU time ≤ oracle.
* **Release**
  * The link map shows no object from the forbidden files.
  * The new source contains no forbidden-header identifiers (grep check).
  * The owner listens and approves.

With reverb or the new limiter on, the comparison uses the dry mix (reverb return and limiter bypassed). The wet path gets its own listening gate.

### 3e. Host reverb and limiter (part of the new mixer)

* **I3DL2 reverb on the bin-10 send**
  * Parameters come from the title's own globals, not from DSP memory. The CC0 decomp gives `s_sound_driver_reverb` (room, room_hf, room_rolloff, decay_time, decay_hf_ratio, reflections, reflections_delay, reverb, reverb_delay, diffusion, density, hf_reference). These sit in `s_sound_driver_globals.reverbs[2]` with `reverb_scales[2]` and `occlusions[2]`, reached through the pointer `g_51ebe4`.
  * Re-read each block when `reverb_dirty` flips.
  * Implementation: an original 8-line FDN (Jot-style) with an early-reflection tap line. Map the parameters per the IASIG I3DL2 guideline. Do not copy OpenAL Soft (LGPL).
  * The return goes into L/R after mixdown at −6 dB.
  * Phase 2: write the return into the GP output buffers that the four FXIN voices read. The title's own routing and volumes then apply, and bins 6–9 stop playing silence.
  * Bins 23–25 (title effect sends, 30 dB down) are dropped at first.
* **Look-ahead limiter**
  * 1.5 ms look-ahead (72 samples), threshold −1 dBFS, attack over the look-ahead, release 80 ms, applied after `RECOMP_APU_GAIN_DB`.
  * It replaces the tanh knee. `RECOMP_APU_LIMIT=0` turns it off and `=tanh` restores the old knee.
  * It adds about 1.5 ms to the current ~55 ms latency.

### 3f. Step list (hours)

| # | Step | h |
|---|---|---|
| 0 | Sanitised hand-off: copy only the measured facts from STATUS into `docs/APU_FACTS.md`; start the fresh session | 2 |
| 1 | Trace, snapshot and DSAPI logging hooks in our files (`mmio_direct.c`, `recomp_manual.c` / abi_wrap) | 6 |
| 2 | Probe harness and test signals; capture the scene corpus (3 scenes × 3 runs) | 8 |
| 3 | Analysis scripts, producing the facts sheet: register map, PIO methods, descriptor fields, write-backs, SGE/SSL, notifier, interrupt | 12 |
| 4 | Library skeleton, contract shims, shadow-mode adapter, `tools/apu_ab.py` | 6 |
| 5 | Front end: registers, read-backs, PIO, counter pauses, doorbell, sentinel, ISR delivery (`MA_SHADOW=2` boots to the menu) | 8 |
| 6 | Voice walk, PCM/ADPCM, SGE, resample, bin volumes, SSL streams and notifiers (menu music matches) | 14 |
| 7 | Envelopes, DLS2 filter / EQ, MP voices, submixes, headroom | 10 |
| 8 | HRTF 3D path (3D acceptance) | 6 |
| 9 | Output stage: mixdown, gain, limiter, taps, XAudio2 / web ring, `apu_mixer_*` | 5 |
| 10 | Host I3DL2 reverb (from game globals) | 8 |
| 11 | Acceptance runs on both maps, plus a fix loop | 12 |
| 12 | Swap the library in CMake, remove `src/apu` xemu files and `tests/apu_mixdown`, link-map check, STATUS entry (owner approval first) | 4 |
| | **Total** | **≈101 h** |

Open questions to settle while measuring:
* Is the APU interrupt vector one fixed number, or does it come from `HalGetInterruptVector`?
* Is the four-voice FXIN read address constant?
* Does the title ever use 24-bit or 8-bit containers? None has been seen yet.
* Is headroom a right shift per bin?

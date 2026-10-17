# MCPX APU facts sheet (clean room)

Every fact the new model (`port/xr/src/mcpxaudio/`, prefix `ma_`) relies on,
with where it came from. Sources are only: public pages, the title's own code
(disassembled with `tools/xdis.py`; addresses are guest VAs in the retail
XBE), and black-box measurement of the current build (register trace,
RAM snapshots, WAVs, the oracle's own report lines). No emulator source was
read. Register and field names here are ours.

Source keys:
* **[wiki-APU]** https://xboxdevwiki.net/APU (wikitext saved in `reports/apu2/wiki/APU.txt`)
* **[wiki-ADPCM]** https://xboxdevwiki.net/Xbox_ADPCM (`reports/apu2/wiki/Xbox_ADPCM.txt`)
* **[wiki-DSP]** https://xboxdevwiki.net/DSP
* **[dis 0xNNNNNN]** the title's DSOUND code at that address (`reports/dsound_dis.txt`, made with `python tools/xdis.py 379C60 D4AB`)
* **[trace menuN]** `reports/apu2/menuN/trace.bin` (RECOMP_APU_TRACE, tools/apu_trace.py); `menu2` also has descriptor dumps around every voice method (RECOMP_APU_TRACE_DESC=1)
* **[snap1]** `reports/apu2/snap1/` RAM snapshots of phys 0x3600000-0x3680000 every 2 s plus after voice-method writes
* **[oracle-log]** the oracle's `[APU]`/`[APUV]`/`[APUH]` report lines in a capture's `out.log` (observed output)

## 1. Memory windows and addressing

| Fact | Source |
|---|---|
| Registers 0xFE800000-0xFE82FFFF are trapped to the model; 0xFE830000-0xFE87FFFF (GP and EP DSP memory) are plain RAM in this runtime. | our `mmio_direct.c`, `xbox_memory_layout.c` |
| Guest physical address P is host `0x80000000 + P + xbox_GetMemoryOffset()` (the contiguous window). The model reads/writes guest memory only this way. | our `xbox_memory_layout.c` comments; [snap1] (snapshots through `xbox_GetMemoryBase()` read zeros, through the window read the descriptors) |
| 48 kHz, 32-sample frames, 1500 frames/s, 32 mixbins, 256 voices. | [wiki-APU] |
| Pitch word p: f = 48000 * 2^(p/4096). | [wiki-APU]; DSOUND clamps p to -0x7FFF..+0x1FFF [dis 0x380E4C] |
| Envelope times are 12-bit, counted at 1500 Hz after x16 (one unit = 512 samples); sustain 8 bit; states 0 off, 1 delay, 2 attack, 3 hold, 4 decay, 5 sustain, 6 release. | [wiki-APU] |
| Xbox ADPCM: IMA tables, 36-byte mono / 72-byte stereo blocks, 4-byte channel interleave, header predictor + step index per channel. | [wiki-ADPCM] |

## 2. DSOUND's memory blocks and the base registers

DSOUND allocates 16 contiguous blocks into a table at 0x387280 (16 bytes each:
virtual, physical, size, flag) [dis 0x3848B6-0x384A35]. Sizes / alignments and
the register each physical address goes to [dis 0x38401F-0x3840B7, 0x3840C1-, 0x384267]:

| Entry | Size | Register | Measured phys | Use (ours) |
|---|---|---|---|---|
| 2 (0x3872A0) | 4 | 0x1324 | 0x3602000 | fence word |
| 3 (0x3872B0) | 0x8000 | 0x202C | 0x3608000 | voice descriptors, 256 x 0x80 |
| 4 (0x3872C0) | 0x4020 | 0x115C | 0x3610000 | notifiers, 16 bytes each |
| 5 (0x3872D0) | 0x4000 | 0x2030 | 0x3618000 | buffer page table (2048 x 8) |
| 6 (0x3872E0) | 0x10000 | 0x2034 | 0x361C000 | stream segment table (8 bytes each) |
| 7 (0x3872F0) | 0x2000 | 0x2038 | 0x362C000 | HRTF target table |
| 8 (0x387300) | 0x2000 | 0x203C | 0x3630000 | HRTF current table |
| 9 (0x387310) | 0x10 | 0x2044 | 0x3634000 | (GP) |
| 12 | 0xC000 | page 0 listed at RAM[0x2040] | 0x3640000 | GP scratch (doorbell at +0x810) |

Measured: [trace menu1] records 60-108; [snap1] RAM at RAM[reg 0x2040] lists 0x3640000, 0x3641000, ... (GP scratch page table, 8 bytes per entry).

## 3. Registers (offsets from 0xFE800000)

| Offset | Ours | Behaviour | Source |
|---|---|---|---|
| 0x1000 | IRQ_STATUS | bit 0 = summary, bit 3 = DSP reset request (ISR re-inits the DSPs), bit 4 = front-end trap, bit 6 = periodic/notify (ISR queues the DPC that runs the fence and services every buffer). Write 1 to clear; init writes 0xFFFFFFFF. Oracle values seen: 0x61, 0x11, 0x71. | [dis 0x37F1F0-0x37F244], [trace menu1] |
| 0x1004 | IRQ_ENABLE | 0xD8, bit 0 set by 0x38445D to enable. | [dis 0x3847E9, 0x38445D] |
| 0x1100 | FE_CONTROL | init 0x88, 0x8, 0x100F. Helper modes: 0: &~0x60 \| 0x80; 1: &~0xE0; 2: \| 0x1007; 3: &~0x1007. ISR handles a trap only if (FE_CONTROL & 0xF00) == 0xF00. Oracle read-back during a trap: 0x1FEF (bits 5-7 and 8-11 set). | [dis 0x3844CB, 0x37F0D3], [trace menu1] |
| 0x1300 / 0x1304 | TRAP_METHOD / TRAP_ARG | on trap: method 0x8000 = "voice ended", arg = voice; DSOUND then unlinks the voice (0x37EA92) unless descriptor +04 bit 23 is set. Oracle: 0x8000 with voices 0x6D, 0x6C, 0xA3 in the menu. | [dis 0x37F052, 0x37EA92], [trace menu1] |
| 0x1324 / 0x1334 | FENCE_ADDR / FENCE_VALUE | DSOUND writes ~n to RAM at the fence block, 0x1324 = its phys address, 0x1334 = n, then spins until the RAM word equals n: the model writes the value to RAM[0x1324] on a 0x1334 write. | [dis 0x37E835-0x37E892] |
| 0x1510 | FE_LOCK | bit 0 toggled around the fence. Plain. | [dis 0x384494] |
| 0x2000 | SE_CONTROL | written 0, 7, 0xF at start. Helper modes: 0 &~0x18, 1 &~0x10\|8, 2 &~8\|0x10, 3 \|0x18, 4 \|7, 5 &~7. | [dis 0x384526], [trace menu1] |
| 0x200C | SAMPLE_COUNT | written 0 at start; read by XAPI USB audio code (0x40F14D) and compared with USB frame number x 48, i.e. a 48 kHz sample count. (Oracle returns a fast 10 MHz-rate value; nothing in DSOUND reads it.) | [dis 0x40F136-0x40F18C], [trace menu1] |
| 0x2054/58/5C, 0x2060/64/68, 0x206C/70/74 | LIST_TOP/CUR/NEXT for lists 2D, 3D, MP | table at 0x38699C; DSOUND reads TOP and writes it when unlinking the head; init 0xFFFF. | XBE data 0x38699C, [dis 0x380EE1-0x38104A] |
| 0x20010 | FIFO_FREE | DSOUND polls (value & ~3) >= 4 x words before writing methods. Oracle always returns 0x80. | [trace menu1] |
| 0x202C0 | HRTF_BINS | 0x09070806: the four 3D output bins 6, 8, 7, 9 (bytes). | [dis 0x38408D], [oracle-log] "hrtf submix 6 8 7 9" |
| 0x20280 | HRTF_HEADROOM | value & 7. | [dis 0x37ECB1] |
| 0x20200 + 4b | BIN_HEADROOM[b] | per mixbin, written 1 then 0. | [trace menu1] |

## 4. Voice methods (PIO, offset 0x20000 + m)

| m | Ours | Effect | Source |
|---|---|---|---|
| 0x2F8 | VOICE_SELECT | selects the voice the field methods address | [trace menu1/2] |
| 0x2FC | VOICE_LOCK | 1 before an edit, 0 after; no effect on the descriptor | [trace menu2] |
| 0x300-0x37C | FIELD | the value lands in the selected voice's descriptor at +(m-0x300), verbatim, except 0x320 / 0x35C (stream segment lists, not stored) and 0x37C (bits 16-31 only; the low half is the list link) | [trace menu2] methoddiff.py, [snap1] methodmap.py |
| 0x3A0 | BUF_BASE | descriptor +0x20 = byte address in buffer-page space (page = addr >> 12) | [trace menu2], [dis 0x382687] |
| 0x3A4 | LOOP_START | loop start in samples (0 when not looping) | [dis 0x382695-0x38269E] |
| 0x3D8 | CUR_POS | current position in samples -> +0x58 bits 0-23 | [dis 0x3826AB, 0x38283A] |
| 0x3DC | END_POS | last sample index -> +0x5C bits 0-23 | [dis 0x382698], [trace menu2] |
| 0x120 | ANTECEDENT | bits 16-17 = list + 1 (1 2D, 2 3D, 3 MP), bits 0-15 = voice to insert after, 0xFFFF = at the top | [dis 0x3823C5-0x3823D2] |
| 0x124 | VOICE_ON | bits 0-15 voice; bits 24-27 initial state of the pitch/filter envelope, bits 28-31 initial state of the amplitude envelope. Oracle: sets +0x54 bit 21 and links +0x7C low 16 to the old top. | [dis 0x382413-0x382453, 0x38159D-0x381628], [trace menu2] |
| 0x128 | VOICE_OFF | clears +0x54 bit 21 | [trace menu2] |
| 0x140 | VOICE_PAUSE | bits 0-15 voice, bit 18 = paused | [dis 0x38127D-0x3812AA, 0x37EE3A] |
| 0x160 | HRTF_SELECT | then 0x400-0x43C: 16 words of that HRTF entry | [trace menu1] |
| 0x190 | SSL_WINDOW | stream-segment index of window slot 0 (multiple of 64) | [dis 0x37F6FA-0x37F730] |
| 0x600 + 8k / 0x604 + 8k | SSL_ENTRY | segment (window + k): physical address; bits 0-15 sample frames, 16-17 sample size (0 8-bit, 1 16-bit, 2 ADPCM, 3 32-bit), 18-22 channels - 1, 23 multichannel. Oracle stores them at RAM[0x2034] + index x 8. | [dis 0x37F6EE-0x37F730, 0x37FF0B-0x37FF8A], [snap1] |
| 0x320 / 0x35C | SSL_A / SSL_B | for the selected voice: bits 8-23 = first segment index (voice x 2 + list) x 16, bits 0-7 = segment count (0 = empty) | [dis 0x37F7B1-0x37F7DE], [trace menu2] |
| 0x804 / 0x808 | PAGE_INDEX / PAGE_ADDR | buffer page table entry; stored at RAM[0x2030] + index x 8 | [trace menu1], [snap1] |

## 5. Descriptor (0x80 bytes per voice at RAM[0x202C] + voice x 0x80)

| Offset | Content | Source |
|---|---|---|
| +0x00 | mixbin selectors 0-2 (bits 0-4, 5-9, 10-14) and 3-5 (bits 16-20, 21-25, 26-30) | [dis 0x380B3A-0x380B67], [oracle-log] |
| +0x04 | bits 0-9 mixbin selectors 6-7; 13-15 set for ordinary buffers; 16-20 channels - 1; 22 second+ voice of a multi-voice buffer; 23 'persist' (DSOUND ignores its end trap); 24 stream; 25 loop; 27 multichannel; 28-31 sample format: 0 8-bit, 5 16-bit, 0xA ADPCM, 0xE 24-in-32, 0xF 32-bit | [dis 0x380985-0x380A06, 0x3825C2, 0x380900-0x380948] |
| +0x08 / +0x0C | amplitude envelope: +08 attack (0-11), delay (12-23); +0C decay (0-11), hold (12-23), sustain (24-31) | [dis 0x381617-0x38167D] |
| +0x10 / +0x14 / +0x18 | pitch/filter envelope: +10 attack, delay, filter-cutoff scale (24-31); +14 decay, hold, sustain (24-31); +18 release (0-11), pitch scale (24-31); +18 bit 23 set by DSOUND for most voices | [dis 0x38159D-0x381610, 0x380926] |
| +0x1C | HRTF entry (0xFFFF = none) | [trace menu2] |
| +0x20 | buffer base (byte address in page space) | [trace menu2] |
| +0x28-+0x30 | current volumes (DSOUND reads them back like +0x60) | [dis 0x38190F-0x381988] |
| +0x54 | state: bit 21 active; bits 28-30 amplitude-envelope state (oracle 0x20200000 -> 0x50200000 after one frame with all times 0) | [trace menu2] |
| +0x58 | bits 0-23 current position (samples; in a stream, within the current segment); bits 24-31 amplitude envelope level (0xFF) | [trace menu2], [snap1] writeback.py |
| +0x5C | bits 0-23 end position; bits 24-31 second envelope level | as above |
| +0x60/+0x64/+0x68 | 8 volumes, 12 bits each, attenuation in 1/64 dB (0xFFF = 64 dB): vol0 = +60 bits 4-15, vol1 = +60 bits 20-31, vol2/3 = +64, vol4/5 = +68, vol6 = low nibbles (bits 0-3) of +60, +64, +68, vol7 = bits 16-19 of the three | [dis 0x380D20-0x380DC4 (mB x 64 / 100, clamped 0xFFF)], [oracle-log] |
| +0x6C | amplitude envelope release (0-11) | [dis 0x38162B] |
| +0x7C | bits 0-15 next voice in the list (0xFFFF = end); bits 16-31 pitch | [dis 0x380F69-0x380F7F, 0x380E64], [trace menu2] |

Per-buffer position units: for ADPCM, DSOUND converts bytes to samples as bytes / block_align x 64 [dis 0x37F425-0x37F45D].

## 6. Notifiers

16 bytes each at RAM[0x115C]; entry 2 + 4 x voice + k is notifier k of a voice. DSOUND arms one by writing 0x80 to byte 15 and tests byte 15 == 1 for "signalled" [dis 0x3808E4, 0x37F407, 0x381A74-0x381A92]. The oracle writes 0x01 to bytes 14 and 15. For a stream voice, k = 0 and k = 1 alternate as segment list A and B finish [snap1].

## 7. Interrupt and DPC

KeConnectInterrupt with ISR 0x37F28C on HalGetInterruptVector(5) [dis 0x37F2FD-0x37F31F]. The DPC (0x37F0AC -> 0x37E898) runs the fence and calls every buffer's service method. In the menu the oracle raises bit 6 (with 0x61) every 0.372 s = one 16384-frame stream segment at 44098 Hz [trace menu1].

## 8. DSP windows

* GP control word 0xFE83FFFC (1 then 3), EP 0xFE85FFFC [dis 0x37E9DB-0x37EA19].
* DSOUND posts commands to GP scratch +0x810 and spins until the word is 0 [dis 0x37E386-0x37E393]; GP scratch page 0 = first entry of the table at RAM[reg 0x2040] [snap1].
* Each pass DSOUND checks that the word at 0xFE85A018 (pointer at 0x387108) equals 0x00CCCCCC and resets the DSPs otherwise [dis 0x37EC7A-0x37EC8A].

## 9. Measured in shadow mode (oracle and new model on one register stream)

Source key **[shadow NAME]**: `reports/apu2/NAME/` (oracle `mix.wav`, new `new*.wav`, `out.log`), compared with `tools/apu_ab.py`, `tools/apu2/binab2.py`, `tools/apu2/fitparts.py`.

| Fact | Source |
|---|---|
| The previous model processes 8 frames back to back once per 256-sample block, writing each voice's position back after every frame. | [shadow sh_tick] `[MAT]` lines: position deltas of 235 source samples (8 x 29.4) at 5.3 ms intervals |
| Volume law: attenuation in 1/64 dB, 0xFFF silent; output = bins x 1, no headroom or gain stage between the bins and the 16-bit output (menu music at 0x280 and a buffer at 0 match within 0.01 dB). | [shadow sh_menu18]: 100% of 1 s windows within 0.5 dB, correlation 1.000 |
| Fold-down: bins 0/6/8 left, 1/7/9 right, 2 to both, effect sends not in the mix. | per-bin energies vs mix, [shadow sh_arm12] |
| Interpolation: 4-point Hermite (same sample values as the oracle: correlation 1.000 on 44,098 Hz PCM music). | [shadow sh_menu9] |
| ADPCM: 64 output samples per 36/72-byte block (header sample + 63 nibbles); stream segment counts are frames = blocks x 64. | correlation 0.999 on ADPCM streams [shadow acc_arm2]; segment ctl 0xAD40 = 693 x 64 (trace armory1) |
| Stream double-buffers are fixed addresses the title refills after each completion (0x2428000 / 0x2438000 for the menu music): a model must latch a segment list and its entries when it starts playing them. | [shadow sh_seg] `[MAS]` log |
| Multipass voices (+04 bit 26) read the bin named by +04 bits 16-20 (31 for the 3D sources) in list order, and the read empties the bin; each 3D source is inserted just before its HRTF voice. | trace sh_arm1 voice-on order; [shadow sh_arm5] variants |
| HRTF entries: 0x40 bytes, 31 taps per ear interleaved left/right, sign-magnitude / 128; byte 63 = interaural delay in half samples, positive delays the left ear. Gain: unit energy over both ears, times 0.875 (fitted). | [APUH] prints matched byte for byte (sh_arm1); ITD sign by residual 0.19% vs 0.98% [shadow sh_ivory10]; gain fit [shadow sh_ivory3/10/12] |
| HRTF voice slots 4-7 (the bin-10 send) carry the filtered left/right output by slot parity. | bin 10 within 0.5 dB in 82% of periods (30% with the dry mono input) [shadow sh_arm13, acc_arm2] |
| VOICE_RELEASE = method 0x12C: state -> 0x66xxxxxx, +0x34 = release rate x 16 counted down per frame, output scaled by count / (rate x 16), at 0 state 0x77000000 and the voice ends. | trace armory1 at 97.198 s (v149) |
| Voice end report: after VOICE_OFF or a voice ending, the oracle traps 0x8000/voice (status 0x71 then 0x11 once more). | trace menu1 records 22011-25806 |
| Notifier for a stream list: k = 0 (list A) / 1 (list B); for an ended buffer voice k = 0. Status 0x61 per completed list. | [snap1], trace menu1 |
| Mixbins are not saturated at 1.0 (16-bit full scale) in the oracle: its pre-gain peaks reach 2.5 x full scale in Ivory Tower. | [shadow sh_ivory5] |
| The title's I3DL2 levels (s_sound_driver_globals.reverbs[0]) are in dB: -100 = off, then -12/-12/+4 in the Armory. | `[MA] reverb globals` print, rv_t |

## 10. Not established

* What the oracle does with SetFilter coefficients: a DLS2 biquad at the coded cutoff and an I3DL2 one-pole both change the mix by under 0.1 dB in Ivory Tower; the model keeps the DLS2 low-pass (on by default) and the one-pole off (RECOMP_MA_FILTER).
* The exact origin of the 0.875 HRTF gain: none of raw, l1, per-ear, max-ear or both-ear RMS normalisations gives it; it is a fitted constant.
* Envelope curve shapes beyond the release: no voice in the three scenes uses non-zero attack/decay times (the oracle reports ea 1.00 throughout).

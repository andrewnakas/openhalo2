/* ma_internal.h -- state of the clean-room APU model (see docs/APU_FACTS.md
 * for every offset and bit used here, with its source). */
#ifndef MA_INTERNAL_H
#define MA_INTERNAL_H

#include <stdint.h>
#include <stdio.h>
#include "mcpx_audio.h"

#define MA_VOICES      256
#define MA_BINS        32
#define MA_FRAME       32          /* samples per frame                    */
#define MA_BLOCK       256         /* samples per output block (8 frames)  */
#define MA_RATE        48000

/* ---- registers (offsets from 0xFE800000), our names --------------------- */
#define R_IRQ_STATUS   0x1000
#define R_IRQ_ENABLE   0x1004
#define R_FE_CONTROL   0x1100
#define R_NOTIFY_BASE  0x115C
#define R_TRAP_METHOD  0x1300
#define R_TRAP_ARG     0x1304
#define R_FENCE_ADDR   0x1324
#define R_FENCE_VALUE  0x1334
#define R_SE_CONTROL   0x2000
#define R_SAMPLE_COUNT 0x200C
#define R_DESC_BASE    0x202C
#define R_PAGE_TABLE   0x2030
#define R_SEG_TABLE    0x2034
#define R_HRTF_TARGET  0x2038
#define R_HRTF_CURRENT 0x203C
#define R_GPS_TABLE    0x2040
#define R_LIST_TOP(l)  (0x2054 + 12 * (l))
#define R_FIFO_FREE    0x20010

/* voice methods (0x20000 + m) */
#define M_ANTECEDENT   0x20120
#define M_VOICE_ON     0x20124
#define M_VOICE_OFF    0x20128
#define M_VOICE_PAUSE  0x20140
#define M_VOICE_RELEASE 0x2012C     /* both envelopes to release (trace armory1, 97.198 s) */
#define M_HRTF_SELECT  0x20160
#define M_SEG_WINDOW   0x20190
#define M_BIN_HEADROOM 0x20200      /* + 4 x bin */
#define M_HRTF_HEADROOM 0x20280
#define M_HRTF_BINS    0x202C0
#define M_VOICE_SELECT 0x202F8
#define M_VOICE_LOCK   0x202FC
#define M_FIELD0       0x20300
#define M_SSL_A        0x20320
#define M_SSL_B        0x2035C
#define M_PITCH        0x2037C
#define M_BUF_BASE     0x203A0
#define M_LOOP_START   0x203A4
#define M_CUR_POS      0x203D8
#define M_END_POS      0x203DC
#define M_HRTF_COEF    0x20400      /* .. 0x2043C */
#define M_SEG_ENTRY    0x20600      /* .. 0x207FC, 8 bytes per slot */
#define M_PAGE_INDEX   0x20804
#define M_PAGE_ADDR    0x20808

/* descriptor words (byte offsets) */
#define D_BINS         0x00
#define D_FORMAT       0x04
#define D_AENV_A       0x08         /* attack 0-11, delay 12-23            */
#define D_AENV_B       0x0C         /* decay 0-11, hold 12-23, sustain 24-31 */
#define D_MENV_A       0x10
#define D_MENV_B       0x14
#define D_MENV_C       0x18         /* release 0-11, pitch scale 24-31     */
#define D_HRTF         0x1C
#define D_BASE         0x20
#define D_LOOP         0x24         /* ours: loop start (not seen in RAM)  */
#define D_STATE        0x54
#define D_POS          0x58
#define D_END          0x5C
#define D_VOL0         0x60
#define D_AENV_REL     0x6C
#define D_LINK         0x7C

/* +0x04 format bits */
#define F_STREAM       (1u << 24)
#define F_LOOP         (1u << 25)
#define F_MULTIPASS    (1u << 26)
#define F_PERSIST      (1u << 23)

#define ST_ACTIVE      (1u << 21)

/* ---- per-voice host state ------------------------------------------------ */
typedef struct ma_voice {
    /* hardware-owned words, kept here so a shadow model has its own copy */
    uint32_t state, pos_word, end_word;
    double   frac;              /* fractional source position (0..1)        */
    int      paused;
    /* stream segment lists A and B */
    uint32_t ssl[2];            /* bits 8-23 first index, 0-7 count         */
    int      ssl_valid[2];
    uint32_t ssl_gen[2];        /* bumped by every non-empty write          */
    int      ssl_cur;           /* which list is playing                    */
    int      seg;               /* entry within that list                   */
    /* latched when a list / segment starts: the title may rewrite the list
     * and its table entries as soon as it is told the list is done */
    int      list_live;         /* a list is latched                        */
    uint32_t list_first, list_count, list_gen;
    int      seg_live;
    uint32_t seg_addr, seg_frames;
    /* interpolation history: last 4 decoded frames, per channel            */
    float    hist[4][2];
    int64_t  hist_pos;          /* source index of hist[3]                  */
    /* ADPCM block cache */
    int64_t  blk_id;            /* block index decoded, -1 none             */
    uint32_t blk_src;           /* address of the cached block              */
    int16_t  blk[2][65];
    /* amplitude envelope */
    int      aenv_state;
    int      aenv_count;
    float    aenv_level;        /* 0..1                                     */
    int      rel_count, rel_total;  /* release: frames left / at the start  */
    /* HRTF FIR history (mono in) */
    float    fir_hist[128];
    float    svf[2][2];         /* DLS2 low-pass state (low, band) per channel */
    float    i3[2];             /* I3DL2 direct-path low-pass state          */
    int      fir_pos;
    /* bookkeeping */
    uint32_t frames_played;
    int      touched;           /* a method addressed it (shadow RAM watch) */
    /* the descriptor as of the last frame the title was not editing it:
     * DSOUND brackets a multi-word edit with VOICE_LOCK 1 / 0 (0x202FC) */
    int      locked;
    uint32_t snap[32];
    uint64_t src_consumed;      /* source frames consumed (pitch check)    */
    uint64_t out_rendered;
    double   energy;            /* sum of squares this voice put into bins (report) */
} ma_voice;

typedef struct ma_limiter {
    float    buf[2][128];
    int      w;
    float    gain;
} ma_limiter;

typedef struct ma_reverb ma_reverb;

typedef struct ma_tap {
    FILE    *f;
    uint32_t frames, limit;
    int      from_start;
} ma_tap;

struct ma_state {
    int       mode;
    char      cfg[512];         /* "NAME=value;..." overrides of getenv     */
    int       index;            /* 0 = first shadow (the one that checks)  */
    uint32_t  stamp[MA_VOICES], gen;
    int       tap_tried;
    uint8_t  *phys;             /* host address of guest physical 0         */
    uint32_t  reg[0x30000 / 4];
    /* private copy of descriptor words for HW-owned fields */
    ma_voice  v[MA_VOICES];
    uint32_t  sel_voice;
    uint32_t  antecedent;
    uint32_t  seg_window;
    uint32_t  hrtf_sel;
    uint8_t   hrtf_tab[256][64];  /* HRTF entries as the methods wrote them   */
    uint32_t  page_index;
    /* front-end trap queue (voice-ended reports) */
    uint32_t  trapq[512];
    unsigned  trap_head, trap_tail;
    /* frame state */
    float     bin[MA_BINS][MA_FRAME];
    uint64_t  frames;           /* frames processed                         */
    uint32_t  sample_count;
    volatile long lock;
    /* thread */
    void     *thread;
    volatile int quit;
    /* output */
    float     gain;             /* RECOMP_APU_GAIN_DB                       */
    int       limit_mode;       /* 0 off, 1 look-ahead, 2 tanh              */
    ma_limiter lim;
    ma_reverb *rv;
    int       reverb_on;
    ma_tap    tap;
    int       device;           /* xa2 opened                               */
    int       mute;
    /* fold-down gains, per bin: left and right */
    float     fold[MA_BINS][2];
    /* stats */
    uint64_t  st_blocks, st_irqs, st_traps, st_notifies, st_clipped;
    double    bin_energy[MA_BINS];
    uint32_t  bin_energy_n;
    int       report_bins, report_voices;
    uint32_t  irq_pending;      /* status bits to raise from the thread    */
    uint32_t  gps_ack, sentinel_ptr, sentinel_val;
    uint64_t  st_doorbell;
    int       adpcm_spb;        /* samples per ADPCM block: 64 or 65       */
    int       interp;           /* 0 linear, 1 hermite                     */
    int       hrtf_norm;        /* 0 raw, 1 l1, 2 rms                      */
    int       hrtf_on;
    int       only;             /* RECOMP_MA_ONLY diagnostics: 1 HRTF voices' outputs only,
                                 * 2 everything but them */
    int       filt;             /* RECOMP_MA_FILTER bits: 1 DLS2 low-pass, 2 I3DL2 */
    int       sat;              /* RECOMP_MA_SAT: mixbins are 24-bit: 0 float, 1 saturate after
                                 * every voice, 2 saturate once per frame */
    float     hrtf_trim;        /* RECOMP_MA_HRTF_TRIM, default 0.857 */
    int       hrtf_rev;         /* RECOMP_MA_HRTF_REV: taps in reverse order (diagnostics) */
    int       mp_mode;          /* RECOMP_MA_MP_MODE: 0 in list order, a read empties the bin;
                                 * 1 all other voices first, reads keep the bin; 2 list order, keep */
    int       hrtf_itd;         /* RECOMP_MA_HRTF_ITD: 1 left on +, -1 right on +, 0 off */
    double    t0;
    uint64_t  st_pause_frames;
    uint64_t  st_sets, st_sets_equal, st_vmis;   /* shadow: active-voice sets per block */
    uint32_t  st_vmis_v[MA_VOICES][2];            /* per voice: live-only, new-only blocks */
};

/* ma_regs.c */
const char *ma_getenv(ma_state *s, const char *name);
ma_state *ma_create_cfg(int mode, const char *cfg, int index);
uint8_t *ma_pa(ma_state *s, uint32_t pa);                 /* host pointer */
uint32_t ma_rd32(ma_state *s, uint32_t pa);
void     ma_wr32(ma_state *s, uint32_t pa, uint32_t v);
uint32_t ma_desc(ma_state *s, int v, int off);           /* descriptor word */
void     ma_desc_set(ma_state *s, int v, int off, uint32_t val);
void     ma_notify(ma_state *s, int v, int k);
void     ma_voice_ended(ma_state *s, int v);
void     ma_lock(ma_state *s);
void     ma_unlock(ma_state *s);

/* ma_voice.c */
void     ma_voice_start(ma_state *s, int v);
void     ma_render_voice(ma_state *s, int v, int list);
void     ma_process_frame(ma_state *s);

/* ma_mix.c */
void     ma_mix_init(ma_state *s);
void     ma_mix_frame(ma_state *s, float *lr);             /* bins -> 32 stereo frames */
void     ma_output_block(ma_state *s, const float *lr, int frames);

/* ma_fx.c */
ma_reverb *ma_reverb_create(void);
void     ma_reverb_destroy(ma_reverb *r);
void     ma_reverb_process(ma_reverb *r, const float *in, float *lr, int frames);
void     ma_reverb_params_from_title(ma_state *s);
void     ma_limiter_process(ma_limiter *l, float *lr, int frames);

/* ma_thread.c */
int      ma_thread_start(ma_state *s);
void     ma_thread_stop(ma_state *s);
void     ma_raise(ma_state *s, uint32_t bits);

#endif

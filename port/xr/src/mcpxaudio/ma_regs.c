/* ma_regs.c -- register file, voice methods, lists, notifiers and the
 * front-end trap of the clean-room APU model. Offsets and bit meanings are in
 * docs/APU_FACTS.md sections 3-6. */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if defined(_WIN32)
#include <windows.h>
#endif
#include "ma_internal.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* ---- guest memory -------------------------------------------------------- */

/* Physical P is guest 0x80000000 + P (the contiguous window). */
uint8_t *ma_pa(ma_state *s, uint32_t pa)
{
    return s->phys + (pa & 0x03FFFFFFu);
}

uint32_t ma_rd32(ma_state *s, uint32_t pa)
{
    uint32_t v;
    memcpy(&v, ma_pa(s, pa), 4);
    return v;
}

void ma_wr32(ma_state *s, uint32_t pa, uint32_t v)
{
    if (s->mode != MA_LIVE)
        return;                         /* a shadow never writes guest RAM */
    memcpy(ma_pa(s, pa), &v, 4);
}

/* ---- descriptors ----------------------------------------------------------
 * The descriptor lives in guest RAM (written by the methods and, for the
 * links and clears, by DSOUND directly). The words the hardware itself
 * updates -- state, position, end -- are also kept per voice here, so that a
 * shadow model, which may not write RAM, still has its own. */
static uint32_t desc_pa(ma_state *s, int v, int off)
{
    return s->reg[R_DESC_BASE / 4] + (uint32_t)v * 0x80u + (uint32_t)off;
}

uint32_t ma_desc(ma_state *s, int v, int off)
{
    switch (off) {
    case D_STATE: return s->v[v].state;
    case D_POS:   return s->v[v].pos_word;
    case D_END:   return s->v[v].end_word;
    default:      return ma_rd32(s, desc_pa(s, v, off));
    }
}

void ma_desc_set(ma_state *s, int v, int off, uint32_t val)
{
    switch (off) {
    case D_STATE: s->v[v].state = val; break;
    case D_POS:   s->v[v].pos_word = val; break;
    case D_END:   s->v[v].end_word = val; break;
    default: break;
    }
    ma_wr32(s, desc_pa(s, v, off), val);
}

/* ---- locking ------------------------------------------------------------- */
#if defined(_WIN32)
static CRITICAL_SECTION s_cs;
static volatile long s_cs_init;
void ma_lock(ma_state *s)
{
    (void)s;
    if (!s_cs_init) {
        if (InterlockedExchange(&s_cs_init, 2) == 0)
            InitializeCriticalSection(&s_cs);
    }
    EnterCriticalSection(&s_cs);
}
void ma_unlock(ma_state *s) { (void)s; LeaveCriticalSection(&s_cs); }
#else
void ma_lock(ma_state *s) { (void)s; }
void ma_unlock(ma_state *s) { (void)s; }
#endif

/* ---- notifiers and traps ------------------------------------------------- */

/* Notifier k of voice v: 16 bytes at RAM[0x115C] + (2 + 4v + k) * 16; the
 * hardware signals it by writing 1 to bytes 14 and 15 (DSOUND tests byte 15
 * == 1 and re-arms it with 0x80). */
void ma_notify(ma_state *s, int v, int k)
{
    uint32_t base = s->reg[R_NOTIFY_BASE / 4];
    uint32_t pa;

    s->st_notifies++;
    if (!base || s->mode != MA_LIVE)
        return;
    pa = base + (uint32_t)(2 + 4 * v + k) * 16u;
    ma_pa(s, pa)[14] = 1;
    ma_pa(s, pa)[15] = 1;
}

/* A voice stopped: DSOUND learns it from a front-end trap with method 0x8000
 * and the voice as argument (0x37F0D3 -> 0x37EA92). Queued; the thread posts
 * one at a time while the previous one has been taken (FE_CONTROL 0xE0
 * cleared). */
void ma_voice_ended(ma_state *s, int v)
{
    unsigned n = (s->trap_tail - s->trap_head) & 511u;
    s->st_traps++;
    if (n < 511) {
        s->trapq[s->trap_tail & 511u] = (uint32_t)v;
        s->trap_tail++;
    }
}

/* ---- the lists ----------------------------------------------------------- */

/* VOICE_ON: insert after the antecedent (or at the top of the list named by
 * its bits 16-17) and mark the voice active. The link is +0x7C bits 0-15. */
static void voice_on(ma_state *s, uint32_t val)
{
    int v = (int)(val & 0xFFFF);
    uint32_t ante = s->antecedent;
    int list = (int)((ante >> 16) & 3) - 1;
    uint32_t link;

    if (v >= MA_VOICES)
        return;
    if ((ante & 0xFFFF) != 0xFFFF) {
        int a = (int)(ante & 0xFFFF);
        uint32_t al = ma_desc(s, a, D_LINK);
        link = ma_desc(s, v, D_LINK);
        ma_desc_set(s, v, D_LINK, (link & 0xFFFF0000u) | (al & 0xFFFF));
        ma_desc_set(s, a, D_LINK, (al & 0xFFFF0000u) | (uint32_t)v);
    } else if (list >= 0) {
        uint32_t top = s->reg[R_LIST_TOP(list) / 4] & 0xFFFF;
        link = ma_desc(s, v, D_LINK);
        ma_desc_set(s, v, D_LINK, (link & 0xFFFF0000u) | top);
        s->reg[R_LIST_TOP(list) / 4] = (uint32_t)v;
    }
    /* the voice starts from what the descriptor holds now: RAM as DSOUND and
     * the methods left it (a shadow takes its private words from RAM here) */
    {
        uint32_t pa = desc_pa(s, v, 0);
        s->v[v].pos_word = ma_rd32(s, pa + D_POS);
        s->v[v].end_word = ma_rd32(s, pa + D_END);
    }
    s->v[v].state = (ma_rd32(s, desc_pa(s, v, D_STATE)) & ~0x77000000u)
                  | ST_ACTIVE | (val & 0x77000000u);
    ma_wr32(s, desc_pa(s, v, D_STATE), s->v[v].state);
    s->v[v].paused = 0;
    ma_voice_start(s, v);
}

static void voice_off(ma_state *s, uint32_t val)
{
    int v = (int)(val & 0xFFFF);
    if (v >= MA_VOICES)
        return;
    if (s->v[v].state & ST_ACTIVE) {
        ma_desc_set(s, v, D_STATE, s->v[v].state & ~ST_ACTIVE);
        ma_notify(s, v, s->v[v].ssl_cur);
        ma_voice_ended(s, v);
    }
}

/* ---- creation ------------------------------------------------------------ */

/* getenv, unless this instance's cfg string names the variable */
const char *ma_getenv(ma_state *s, const char *name)
{
    static __declspec(thread) char val[512];
    size_t n = strlen(name);
    const char *p = s ? s->cfg : NULL;
    while (p && *p) {
        const char *end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len > n && !strncmp(p, name, n) && p[n] == '=') {
            size_t vl = len - n - 1;
            if (vl >= sizeof val) vl = sizeof val - 1;
            memcpy(val, p + n + 1, vl);
            val[vl] = 0;
            return val;
        }
        p = end ? end + 1 : NULL;
    }
    return getenv(name);
}

#define env_int(name, dflt) env_int_(s, name, dflt)
static int env_int_(ma_state *s, const char *name, int dflt)
{
    const char *e = ma_getenv(s, name);
    return (e && *e) ? (int)strtol(e, NULL, 0) : dflt;
}

ma_state *ma_create(int mode)
{
    return ma_create_cfg(mode, NULL, 0);
}

ma_state *ma_create_cfg(int mode, const char *cfg, int index)
{
    ma_state *s = (ma_state *)calloc(1, sizeof *s);
    const char *e;
    int i;

    if (!s)
        return NULL;
    s->mode = mode;
    s->index = index;
    if (cfg)
        strncpy(s->cfg, cfg, sizeof s->cfg - 1);
    s->phys = (uint8_t *)((uintptr_t)0x80000000u + xbox_GetMemoryOffset());
    for (i = 0; i < 3; i++)
        s->reg[R_LIST_TOP(i) / 4] = s->reg[R_LIST_TOP(i) / 4 + 1] = s->reg[R_LIST_TOP(i) / 4 + 2] = 0xFFFF;
    for (i = 0; i < MA_VOICES; i++)
        s->v[i].blk_id = -1;
    s->adpcm_spb = env_int("RECOMP_MA_ADPCM_SPB", 64);
    s->interp = env_int("RECOMP_MA_INTERP", 1);
    e = ma_getenv(s, "RECOMP_APU_HRTF_GAIN");
    s->hrtf_norm = (e && !strcmp(e, "raw")) ? 0 : (e && !strcmp(e, "l1")) ? 1
                 : (e && !strcmp(e, "max")) ? 3 : (e && !strcmp(e, "sum")) ? 4
                 : (e && !strcmp(e, "ear")) ? 5 : (e && !strcmp(e, "rms")) ? 2 : 4;
    s->hrtf_on = env_int("RECOMP_APU_HRTF", 1);
    s->hrtf_itd = env_int("RECOMP_MA_HRTF_ITD", 1);
    s->mp_mode = env_int("RECOMP_MA_MP_MODE", 0);
    s->hrtf_rev = env_int("RECOMP_MA_HRTF_REV", 0);
    s->hrtf_dry = env_int("RECOMP_MA_HRTF_DRY", 4);
    e = ma_getenv(s, "RECOMP_MA_HRTF_TRIM");
    s->hrtf_trim = (e && *e) ? (float)atof(e) : 0.875f;
    s->sat = env_int("RECOMP_MA_SAT", 0);
    s->filt = env_int("RECOMP_MA_FILTER", 1);
    e = ma_getenv(s, "RECOMP_MA_ONLY");
    s->only = (e && !strcmp(e, "hrtf")) ? 1 : (e && !strcmp(e, "nohrtf")) ? 2 : (e && !strcmp(e, "list0")) ? 3 : (e && !strcmp(e, "mpdirect")) ? 4 : 0;
    s->report_bins = env_int("RECOMP_APU_BINS", 0);
    s->report_voices = env_int("RECOMP_APU_VOICES", 0);
    e = ma_getenv(s, "RECOMP_APU_DSP_ACK_GPS");
    s->gps_ack = e ? (uint32_t)strtoul(e, NULL, 0) : 0x810;
    e = ma_getenv(s, "RECOMP_APU_DSP_SENTINEL");
    if (e && *e && strcmp(e, "0")) {
        const char *c = strchr(e, ',');
        s->sentinel_ptr = (uint32_t)strtoul(e, NULL, 0);
        s->sentinel_val = c ? (uint32_t)strtoul(c + 1, NULL, 0) : 0xCCCCCC;
    } else if (!e) {
        s->sentinel_ptr = 0x387108;
        s->sentinel_val = 0xCCCCCC;
    }
    ma_mix_init(s);
    if (ma_thread_start(s) != 0) {
        free(s);
        return NULL;
    }
    return s;
}

void ma_destroy(ma_state *s)
{
    if (!s)
        return;
    ma_thread_stop(s);
    if (s->rv)
        ma_reverb_destroy(s->rv);
    free(s);
}

void ma_set_mode(ma_state *s, int mode)
{
    s->mode = mode;
}

/* ---- reads --------------------------------------------------------------- */

uint32_t ma_read(ma_state *s, uint32_t off, int size)
{
    uint32_t v, a = off & ~3u, sh = (off & 3u) * 8u;

    if (off >= 0x30000)
        return 0;
    switch (a) {
    case R_FIFO_FREE:
        v = 0x80;                       /* always room (oracle reads 0x80) */
        break;
    case R_SAMPLE_COUNT:
        v = s->sample_count;
        break;
    default:
        v = s->reg[a / 4];
        break;
    }
    v >>= sh;
    if (size == 1) v &= 0xFF;
    else if (size == 2) v &= 0xFFFF;
    return v;
}

/* ---- writes -------------------------------------------------------------- */

static void method(ma_state *s, uint32_t m, uint32_t val)
{
    int v = (int)s->sel_voice;

    if (v < MA_VOICES)
        s->v[v].touched = 1;
    if ((m == M_VOICE_ON || m == M_VOICE_OFF || m == M_VOICE_PAUSE) && (val & 0xFFFF) < MA_VOICES)
        s->v[val & 0xFFFF].touched = 1;

    if (m >= M_FIELD0 && m < 0x20400) {
        if (v >= MA_VOICES)
            return;
        switch (m) {
        case M_SSL_A:
        case M_SSL_B: {
            int k = m == M_SSL_B;
            s->v[v].ssl[k] = val;
            s->v[v].ssl_valid[k] = (val & 0xFF) != 0;
            if (val & 0xFF)
                s->v[v].ssl_gen[k]++;
            return;
        }
        case M_PITCH:
            ma_desc_set(s, v, D_LINK, (ma_desc(s, v, D_LINK) & 0xFFFF) | (val & 0xFFFF0000u));
            return;
        case M_CUR_POS:
            ma_desc_set(s, v, D_POS, (s->v[v].pos_word & 0xFF000000u) | (val & 0xFFFFFF));
            s->v[v].frac = 0;
            s->v[v].blk_id = -1;
            return;
        case M_END_POS:
            ma_desc_set(s, v, D_END, (s->v[v].end_word & 0xFF000000u) | (val & 0xFFFFFF));
            return;
        case M_BUF_BASE:
            ma_desc_set(s, v, D_BASE, val);
            s->v[v].blk_id = -1;
            return;
        case M_LOOP_START:
            ma_desc_set(s, v, D_LOOP, val);
            return;
        default:
            if (m < 0x20380) {
                int off = (int)(m - M_FIELD0);
                if (off == D_STATE || off == D_POS || off == D_END)
                    ma_desc_set(s, v, off, val);
                else
                    ma_wr32(s, desc_pa(s, v, off), val);
            }
            return;
        }
    }
    if (m >= M_HRTF_COEF && m < M_HRTF_COEF + 0x40) {
        uint32_t base = s->reg[R_HRTF_TARGET / 4];
        memcpy(&s->hrtf_tab[s->hrtf_sel & 0xFF][m - M_HRTF_COEF], &val, 4);
        if (base)
            ma_wr32(s, base + s->hrtf_sel * 0x40u + (m - M_HRTF_COEF), val);
        return;
    }
    if (m >= M_SEG_ENTRY && m < M_SEG_ENTRY + 0x200) {
        uint32_t base = s->reg[R_SEG_TABLE / 4];
        uint32_t idx = s->seg_window + ((m - M_SEG_ENTRY) >> 3);
        if (base)
            ma_wr32(s, base + idx * 8u + (m & 4u), val);
        return;
    }
    switch (m) {
    case M_VOICE_SELECT: s->sel_voice = val & 0xFFFF; break;
    case M_VOICE_LOCK:
        if (v < MA_VOICES)
            s->v[v].locked = val & 1;
        break;
    case M_ANTECEDENT:   s->antecedent = val; break;
    case M_VOICE_ON:     voice_on(s, val); break;
    case M_VOICE_OFF:    voice_off(s, val); break;
    case M_VOICE_RELEASE: {
        /* state -> 0x66xxxxxx; +0x34 holds the release count (rate x 16,
         * counted down once per frame); at 0 the voice ends (0x77000000) */
        int rv = (int)(val & 0xFFFF);
        if (rv < MA_VOICES && (s->v[rv].state & ST_ACTIVE)) {
            ma_voice *vc = &s->v[rv];
            uint32_t rel = ma_desc(s, rv, D_AENV_REL) & 0xFFF;
            vc->state = (vc->state & ~0x77000000u) | 0x66000000u;
            vc->aenv_state = 6;
            vc->rel_total = vc->rel_count = (int)rel * 16;
            ma_desc_set(s, rv, D_STATE, vc->state);
            ma_wr32(s, s->reg[R_DESC_BASE / 4] + (uint32_t)rv * 0x80u + 0x34, (uint32_t)vc->rel_count);
        }
        break;
    }
    case M_VOICE_PAUSE:
        if ((val & 0xFFFF) < MA_VOICES)
            s->v[val & 0xFFFF].paused = (val >> 18) & 1;
        break;
    case M_HRTF_SELECT:  s->hrtf_sel = val & 0xFF; break;
    case M_SEG_WINDOW:   s->seg_window = val & 0x3FFFC0; break;
    case M_PAGE_INDEX:   s->page_index = val & 0x7FF; break;
    case M_PAGE_ADDR: {
        uint32_t base = s->reg[R_PAGE_TABLE / 4];
        if (base) {
            ma_wr32(s, base + s->page_index * 8u, val);
            ma_wr32(s, base + s->page_index * 8u + 4, 0);
        }
        break;
    }
    default: break;
    }
}

void ma_write(ma_state *s, uint32_t off, uint32_t val, int size)
{
    uint32_t a = off & ~3u;

    if (off >= 0x30000)
        return;
    if (size != 4) {                    /* merge a narrow write */
        uint32_t sh = (off & 3u) * 8u, mask = (size == 1 ? 0xFFu : 0xFFFFu) << sh;
        val = (s->reg[a / 4] & ~mask) | ((val << sh) & mask);
    }
    ma_lock(s);
    if (a >= 0x20000) {
        s->reg[a / 4] = val;
        method(s, a, val);
        ma_unlock(s);
        return;
    }
    switch (a) {
    case R_IRQ_STATUS:
        s->reg[a / 4] &= ~val;          /* write 1 to clear */
        if (!(s->reg[a / 4] & ~1u))
            s->reg[a / 4] = 0;
        break;
    case R_FENCE_VALUE:
        s->reg[a / 4] = val;
        if (s->reg[R_FENCE_ADDR / 4])
            ma_wr32(s, s->reg[R_FENCE_ADDR / 4], val);
        break;
    case R_SAMPLE_COUNT:
        s->sample_count = val;
        s->reg[a / 4] = val;
        break;
    default:
        s->reg[a / 4] = val;
        break;
    }
    ma_unlock(s);
}

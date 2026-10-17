/* h2net (web build): the room calls Open Live makes (port/xr/src/openlive_match.c)
 * handed to the page, where WebRTC lives (port/web/net.js keeps one
 * net/h2net.js mesh per room; frames cross through the network controller's
 * rings as in system link, so h2net_send / h2net_recv are not used here).
 *
 * The page side is window.h2rooms (net.js, ?link=openlive): join(code),
 * leave(code), full(code). Calls made before the page is ready are queued
 * there (window.h2roomq). External rooms (sessions past a relay room's ten
 * members) are native only for now: h2net_join_ext fails here. */
#include "h2net.h"

#include <emscripten.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ROOMS H2NET_MAX_ROOMS
static char s_code[MAX_ROOMS][32];
static pthread_mutex_t s_lk = PTHREAD_MUTEX_INITIALIZER;

int h2net_init(const char *relay_url) { (void)relay_url; return 0; }
int h2net_open(const char *room_code, const char *relay_url) { (void)relay_url; return h2net_join(room_code) >= 0 ? 0 : -1; }

int h2net_find(const char *room_code)
{
    int i, r = -1;
    pthread_mutex_lock(&s_lk);
    for (i = 0; i < MAX_ROOMS; i++) if (s_code[i][0] && !strcmp(s_code[i], room_code)) r = i;
    pthread_mutex_unlock(&s_lk);
    return r;
}

int h2net_join(const char *room_code)
{
    int i, r;
    if (!room_code || strlen(room_code) < 4 || strlen(room_code) > 24) return -1;
    if ((r = h2net_find(room_code)) >= 0) return r;
    pthread_mutex_lock(&s_lk);
    for (i = 0, r = -1; i < MAX_ROOMS && r < 0; i++) if (!s_code[i][0]) r = i;
    if (r >= 0) snprintf(s_code[r], sizeof s_code[r], "%s", room_code);
    pthread_mutex_unlock(&s_lk);
    if (r < 0) return -1;
    MAIN_THREAD_ASYNC_EM_ASM({                /* never wait for the page: see olive_ws_web.h */
        const code = UTF8ToString($0); _free($0);
        if (window.h2rooms) window.h2rooms.join(code);
        else (window.h2roomq = window.h2roomq || []).push(['join', code]);
    }, strdup(room_code));
    return r;
}

void h2net_leave(const char *room_code)
{
    int r = h2net_find(room_code);
    if (r < 0) return;
    pthread_mutex_lock(&s_lk);
    s_code[r][0] = 0;
    pthread_mutex_unlock(&s_lk);
    MAIN_THREAD_ASYNC_EM_ASM({
        const code = UTF8ToString($0); _free($0);
        if (window.h2rooms) window.h2rooms.leave(code);
        else (window.h2roomq = window.h2roomq || []).push(['leave', code]);
    }, strdup(room_code));
}

/* Open Live asks this on every pump, and a page without external rooms
 * could not act on the answer anyway: no round trip to the page for it. */
int h2net_room_full(const char *room_code) { (void)room_code; return 0; }

int h2net_room_peers(const char *room_code)
{
    if (h2net_find(room_code) < 0) return -1;
    return MAIN_THREAD_EM_ASM_INT({
        return window.h2rooms ? window.h2rooms.peers(UTF8ToString($0)) : 0;
    }, room_code);
}

int h2net_peers(void)
{
    return MAIN_THREAD_EM_ASM_INT({ return window.h2rooms ? window.h2rooms.peers('') : 0; });
}

int  h2net_join_ext(const char *room_code, h2net_signal_fn fn, void *ctx) { (void)room_code; (void)fn; (void)ctx; return -1; }
void h2net_ext_signal(const char *room_code, int from, const uint8_t *msg, int len) { (void)room_code; (void)from; (void)msg; (void)len; }
int  h2net_ext_connect(const char *room_code, int id) { (void)room_code; (void)id; return -1; }
int  h2net_ext_has(const char *room_code, int id) { (void)room_code; (void)id; return 0; }

void h2net_send(const uint8_t *f, uint32_t len) { (void)f; (void)len; }
int  h2net_recv(uint8_t *f, uint32_t max) { (void)f; (void)max; return 0; }

void h2net_status(char *buf, size_t n)
{
    int i;
    size_t o = 0;
    if (!buf || !n) return;
    buf[0] = 0;
    pthread_mutex_lock(&s_lk);
    for (i = 0; i < MAX_ROOMS && o + 40 < n; i++)
        if (s_code[i][0]) o += (size_t)snprintf(buf + o, n - o, "%sroom %s (page)", o ? " | " : "", s_code[i]);
    pthread_mutex_unlock(&s_lk);
    if (!o) snprintf(buf, n, "no rooms (page)");
}

void h2net_close(void) {}

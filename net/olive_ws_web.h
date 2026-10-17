/* olive_ws_web.h: the few libdatachannel WebSocket calls net/olive.c makes,
 * on the browser's WebSockets (Emscripten's websocket API, -lwebsocket.js),
 * so the facilitator is the same code in the web build. Included by olive.c.
 *
 * The sockets live on the page's main thread (the browser has them there
 * only, and Emscripten runs their callbacks there whatever thread one asks
 * for). Nothing here waits for the main thread: create, send and delete are
 * posted to it, and its socket events are copied and posted to one "olive"
 * thread that this file starts and that only runs its event loop, the way
 * libdatachannel's own threads deliver them natively. A game thread may
 * call in here while the runtime holds locks the page's own frame code
 * needs; a call that waited for the main thread could stop the page.
 *
 * Handles are ours (slot + 1 and a generation), not Emscripten's socket ids,
 * so a socket can be handed out before the main thread has made it. */
#ifndef OLIVE_WS_WEB_H
#define OLIVE_WS_WEB_H

#include <emscripten.h>
#include <emscripten/websocket.h>
#include <emscripten/threading.h>
#include <emscripten/proxying.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>

#define RTC_API
typedef struct { int connectionTimeoutMs; } rtcWsConfiguration;
typedef void (*rtcMessageCallbackFunc)(int id, const char *message, int size, void *ptr);
typedef void (*rtcClosedCallbackFunc)(int id, void *ptr);
typedef void (*rtcErrorCallbackFunc)(int id, const char *error, void *ptr);

#define WSW_MAX 128
typedef struct {
    int used, gen;
    int em;                         /* Emscripten's socket, once the main thread made it */
    void *ptr;
    rtcMessageCallbackFunc on_msg;
    rtcClosedCallbackFunc on_closed;
    rtcErrorCallbackFunc on_error;
} wsw_t;
static wsw_t s_wsw[WSW_MAX];
static pthread_mutex_t s_wsw_lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_t s_wsw_thread;
static volatile int s_wsw_ready;
static int s_wsw_gen;

/* handle = slot + 1 in the low 8 bits, generation above */
#define WSW_SLOT(h) (((h) & 0xFF) - 1)
#define WSW_GEN(h)  ((h) >> 8)

static void *wsw_main(void *arg)
{
    (void)arg;
    s_wsw_ready = 1;
    emscripten_exit_with_live_runtime();     /* stay, running the event loop */
    return NULL;
}

static void wsw_start(void)
{
    static pthread_mutex_t once = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&once);
    if (!s_wsw_ready) {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_create(&s_wsw_thread, &a, wsw_main, NULL);
        while (!s_wsw_ready) sched_yield();
    }
    pthread_mutex_unlock(&once);
}

static int wsw_live(int h, wsw_t *out)      /* a copy of a live handle's slot */
{
    int i = WSW_SLOT(h), ok = 0;
    if (i < 0 || i >= WSW_MAX) return 0;
    pthread_mutex_lock(&s_wsw_lk);
    if (s_wsw[i].used && s_wsw[i].gen == WSW_GEN(h)) { *out = s_wsw[i]; ok = 1; }
    pthread_mutex_unlock(&s_wsw_lk);
    return ok;
}

/* ---- events: main thread -> olive thread ------------------------------------- */

typedef struct { int h, kind, len; char data[]; } wsw_ev_t;   /* kind: 0 binary, 1 text, 2 closed, 3 error */

static void wsw_deliver(void *arg)            /* the olive thread */
{
    wsw_ev_t *e = (wsw_ev_t *)arg;
    wsw_t w;
    if (wsw_live(e->h, &w)) {
        if (e->kind == 0 && w.on_msg) w.on_msg(e->h, e->data, e->len, w.ptr);
        else if (e->kind == 1 && w.on_msg) w.on_msg(e->h, e->data, -1, w.ptr);
        else if (e->kind == 2 && w.on_closed) w.on_closed(e->h, w.ptr);
        else if (e->kind == 3 && w.on_error) w.on_error(e->h, "error", w.ptr);
    }
    free(e);
}

static void wsw_post(int h, int kind, const void *data, int len)   /* the main thread */
{
    wsw_ev_t *e = (wsw_ev_t *)malloc(sizeof *e + (size_t)len + 1);
    if (!e) return;
    e->h = h;
    e->kind = kind;
    e->len = len;
    if (len > 0) memcpy(e->data, data, (size_t)len);
    e->data[len > 0 ? len : 0] = 0;
    if (!emscripten_proxy_async(emscripten_proxy_get_system_queue(), s_wsw_thread, wsw_deliver, e))
        free(e);
}

static EM_BOOL wsw_onmessage(int type, const EmscriptenWebSocketMessageEvent *e, void *u)
{
    (void)type;
    if (e->isText) wsw_post((int)(intptr_t)u, 1, e->data, (int)strlen((const char *)e->data));
    else wsw_post((int)(intptr_t)u, 0, e->data, (int)e->numBytes);
    return EM_TRUE;
}

static EM_BOOL wsw_onclose(int type, const EmscriptenWebSocketCloseEvent *e, void *u)
{
    (void)type; (void)e;
    wsw_post((int)(intptr_t)u, 2, NULL, 0);
    return EM_TRUE;
}

static EM_BOOL wsw_onerror(int type, const EmscriptenWebSocketErrorEvent *e, void *u)
{
    (void)type; (void)e;
    wsw_post((int)(intptr_t)u, 3, NULL, 0);
    return EM_TRUE;
}

/* ---- requests: any thread -> main thread ----------------------------------------- */

typedef struct { int h, op, len; char data[]; } wsw_req_t;      /* op: 0 create (data = URL), 1 send, 2 delete */

static void wsw_main_do(void *arg)           /* the main thread */
{
    wsw_req_t *r = (wsw_req_t *)arg;
    int i = WSW_SLOT(r->h), em = 0, live = 0;
    pthread_mutex_lock(&s_wsw_lk);
    if (i >= 0 && i < WSW_MAX && s_wsw[i].gen == WSW_GEN(r->h)) { live = s_wsw[i].used; em = s_wsw[i].em; }
    pthread_mutex_unlock(&s_wsw_lk);
    if (r->op == 0 && live) {
        EmscriptenWebSocketCreateAttributes a;
        emscripten_websocket_init_create_attributes(&a);
        a.url = r->data;
        a.createOnMainThread = EM_TRUE;
        em = emscripten_websocket_new(&a);
        if (em > 0) {
            void *u = (void *)(intptr_t)r->h;
            emscripten_websocket_set_onmessage_callback_on_thread(em, u, wsw_onmessage, EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD);
            emscripten_websocket_set_onclose_callback_on_thread(em, u, wsw_onclose, EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD);
            emscripten_websocket_set_onerror_callback_on_thread(em, u, wsw_onerror, EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD);
            pthread_mutex_lock(&s_wsw_lk);
            if (s_wsw[i].used && s_wsw[i].gen == WSW_GEN(r->h)) s_wsw[i].em = em;
            else live = 0;                   /* deleted meanwhile */
            pthread_mutex_unlock(&s_wsw_lk);
            if (!live) { emscripten_websocket_close(em, 1000, "bye"); emscripten_websocket_delete(em); }
        } else {
            wsw_post(r->h, 3, NULL, 0);
        }
    } else if (r->op == 1 && live && em > 0) {
        unsigned short state = 0;
        emscripten_websocket_get_ready_state(em, &state);
        if (state == 1) emscripten_websocket_send_binary(em, r->data, (uint32_t)r->len);
    } else if (r->op == 2) {
        memcpy(&em, r->data, sizeof em);     /* taken when it was deleted (the slot may be reused) */
        if (em > 0) { emscripten_websocket_close(em, 1000, "bye"); emscripten_websocket_delete(em); }
    }
    free(r);
}

static void wsw_request(int h, int op, const void *data, int len)
{
    wsw_req_t *r = (wsw_req_t *)malloc(sizeof *r + (size_t)len + 1);
    if (!r) return;
    r->h = h;
    r->op = op;
    r->len = len;
    if (len > 0) memcpy(r->data, data, (size_t)len);
    r->data[len > 0 ? len : 0] = 0;
    if (!emscripten_proxy_async(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(), wsw_main_do, r))
        free(r);
}

/* ---- the calls olive.c makes --------------------------------------------------------- */

static int rtcCreateWebSocketEx(const char *url, const rtcWsConfiguration *cfg)
{
    int i, h = -1;
    (void)cfg;
    wsw_start();
    pthread_mutex_lock(&s_wsw_lk);
    for (i = 0; i < WSW_MAX; i++) {
        if (s_wsw[i].used) continue;
        memset(&s_wsw[i], 0, sizeof s_wsw[i]);
        s_wsw[i].used = 1;
        s_wsw[i].gen = (++s_wsw_gen & 0x3FFFFF) + 1;
        h = (i + 1) | s_wsw[i].gen << 8;
        break;
    }
    pthread_mutex_unlock(&s_wsw_lk);
    if (h > 0) wsw_request(h, 0, url, (int)strlen(url));
    return h;
}

#define WSW_SET(h, field, v) do { int i_ = WSW_SLOT(h); pthread_mutex_lock(&s_wsw_lk); \
    if (i_ >= 0 && i_ < WSW_MAX && s_wsw[i_].used && s_wsw[i_].gen == WSW_GEN(h)) s_wsw[i_].field = (v); \
    pthread_mutex_unlock(&s_wsw_lk); } while (0)
static int rtcSetUserPointer(int ws, void *ptr) { WSW_SET(ws, ptr, ptr); return 0; }
static int rtcSetClosedCallback(int ws, rtcClosedCallbackFunc f) { WSW_SET(ws, on_closed, f); return 0; }
static int rtcSetErrorCallback(int ws, rtcErrorCallbackFunc f) { WSW_SET(ws, on_error, f); return 0; }
static int rtcSetMessageCallback(int ws, rtcMessageCallbackFunc f) { WSW_SET(ws, on_msg, f); return 0; }

static int rtcSendMessage(int ws, const char *data, int size)
{
    if (ws <= 0 || size <= 0) return -1;
    wsw_request(ws, 1, data, size);
    return 0;
}

static int rtcDeleteWebSocket(int ws)
{
    int i = WSW_SLOT(ws), ok = 0, em = 0;
    if (i < 0 || i >= WSW_MAX) return -1;
    pthread_mutex_lock(&s_wsw_lk);
    if (s_wsw[i].used && s_wsw[i].gen == WSW_GEN(ws)) {
        em = s_wsw[i].em;                     /* 0: not made yet; its creation sees the slot free */
        s_wsw[i].used = 0;                    /* events still on their way are dropped */
        s_wsw[i].on_msg = NULL;
        s_wsw[i].on_closed = NULL;
        s_wsw[i].on_error = NULL;
        ok = 1;
    }
    pthread_mutex_unlock(&s_wsw_lk);
    if (ok) wsw_request(ws, 2, &em, sizeof em);
    return 0;
}

#endif

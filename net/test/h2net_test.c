/* h2net_test: one station of a mesh test.
 *
 *   h2net_test <room> <index 1-15> <stations> [frames] [relay]
 *
 * Station MAC is 02:68:32:00:00:<index>. The test waits for stations-1 open
 * peers, learns everyone's MAC from broadcast HELLOs, then for `frames`
 * rounds sends one unicast PING to every other station (echoed back as PONG)
 * and one broadcast frame. Every 10th frame is full size (1514 bytes), the
 * rest 300. Prints one summary line per peer and a totals line.
 * Exit code 0 when everything was delivered (>= 99%) and nothing misdelivered.
 */
#include "h2net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#define MAXI 16
enum { T_HELLO = 'H', T_PING = 'P', T_PONG = 'Q', T_BCAST = 'B', T_DONE = 'D' };

static int me, stations, frames;
static uint8_t my_mac[6] = { 0x02, 0x68, 0x32, 0, 0, 0 };
static int heard[MAXI], done_from[MAXI];
static int ping_rx[MAXI], pong_rx[MAXI], bcast_rx[MAXI];
static int misdelivered, bcasts_sent, malformed;
static double *rtt;
static int nrtt;
static LARGE_INTEGER qpf, qp0;

static double now_us(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - qp0.QuadPart) * 1e6 / (double)qpf.QuadPart;
}

static void put_frame(int dst_idx, int type, uint32_t seq, double t, uint32_t len)
{
    uint8_t f[1514];
    uint64_t tu = (uint64_t)t;
    memset(f, 0, sizeof f);
    if (dst_idx) { memcpy(f, my_mac, 6); f[5] = (uint8_t)dst_idx; }
    else memset(f, 0xFF, 6);
    memcpy(f + 6, my_mac, 6);
    f[12] = 0x88; f[13] = 0xB5;
    f[14] = (uint8_t)type;
    memcpy(f + 15, &seq, 4);
    memcpy(f + 19, &tu, 8);
    f[27] = (uint8_t)me;
    if (len < 60) len = 60;
    h2net_send(f, len);
    if (!dst_idx) bcasts_sent++;
}

static void poll_rx(void)
{
    uint8_t f[2048];
    int n;
    while ((n = h2net_recv(f, sizeof f)) > 0) {
        int bc = f[0] == 0xFF, src = f[11], type = f[14];
        uint32_t seq;
        uint64_t tu;
        if (n < 28 || f[12] != 0x88 || f[13] != 0xB5 || src <= 0 || src >= MAXI) { malformed++; continue; }
        if (!bc && memcmp(f, my_mac, 6)) { misdelivered++; continue; }
        memcpy(&seq, f + 15, 4);
        memcpy(&tu, f + 19, 8);
        switch (type) {
        case T_HELLO: heard[src] = 1; break;
        case T_DONE:  done_from[src] = 1; heard[src] = 1; break;
        case T_BCAST: bcast_rx[src]++; break;
        case T_PING:  ping_rx[src]++; put_frame(src, T_PONG, seq, (double)tu, (uint32_t)n); break;
        case T_PONG:
            pong_rx[src]++;
            if (nrtt < frames * MAXI) rtt[nrtt++] = (now_us() - (double)tu) / 1000.0;
            break;
        }
    }
}

static int count(const int *a)
{
    int i, c = 0;
    for (i = 1; i < MAXI; i++) if (i != me && a[i]) c++;
    return c;
}

static void wait_ms(double ms)
{
    double end = now_us() + ms * 1000.0;
    while (now_us() < end) { poll_rx(); Sleep(1); }
}

static int cmpd(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    char st[1024];
    double t_end, t0;
    int i, seq, ok = 1;
    const char *relay;

    if (argc < 4) {
        fprintf(stderr, "usage: h2net_test <room> <index 1-15> <stations> [frames] [relay]\n");
        return 2;
    }
    me = atoi(argv[2]);
    stations = atoi(argv[3]);
    frames = argc > 4 ? atoi(argv[4]) : 1000;
    relay = argc > 5 ? argv[5] : NULL;
    if (me < 1 || me >= MAXI || stations < 2 || stations > 10) return 2;
    my_mac[5] = (uint8_t)me;
    rtt = (double *)calloc((size_t)frames * MAXI, sizeof *rtt);
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&qp0);
    timeBeginPeriod(1);
    setvbuf(stdout, NULL, _IONBF, 0);

    if (h2net_open(argv[1], relay) != 0) { printf("st%d: h2net_open failed\n", me); return 1; }

    /* 1. all peers connected */
    t0 = now_us();
    while (h2net_peers() < stations - 1) {
        if (now_us() - t0 > 60e6) {
            h2net_status(st, sizeof st);
            printf("st%d: TIMEOUT waiting for peers: %s\n", me, st);
            h2net_close();
            return 1;
        }
        poll_rx();
        Sleep(5);
    }
    printf("st%d: mesh up in %.0f ms\n", me, (now_us() - t0) / 1000.0);

    /* 2. learn MACs */
    t0 = now_us();
    t_end = 0;
    while (!t_end || now_us() < t_end) {
        put_frame(0, T_HELLO, 0, now_us(), 60);
        wait_ms(50);
        if (!t_end && count(heard) >= stations - 1) t_end = now_us() + 500e3;
        if (now_us() - t0 > 20e6) { printf("st%d: TIMEOUT learning MACs (%d heard)\n", me, count(heard)); break; }
    }

    /* 3. traffic: one unicast PING to every station + one broadcast per round, ~2 ms per round */
    t0 = now_us();
    for (seq = 0; seq < frames; seq++) {
        uint32_t len = (seq % 10 == 9) ? 1514 : 300;
        for (i = 1; i < MAXI; i++)
            if (i != me && heard[i]) put_frame(i, T_PING, (uint32_t)seq, now_us(), len);
        put_frame(0, T_BCAST, (uint32_t)seq, now_us(), len);
        poll_rx();
        while (now_us() - t0 < (seq + 1) * 2000.0) { poll_rx(); Sleep(1); }
    }

    /* 4. drain, then DONE handshake so nobody leaves while others still send */
    wait_ms(1000);
    t0 = now_us();
    t_end = 0;
    while (!t_end || now_us() < t_end) {
        put_frame(0, T_DONE, 0, now_us(), 60);
        wait_ms(100);
        if (!t_end && count(done_from) >= stations - 1) t_end = now_us() + 1000e3;
        if (now_us() - t0 > 15e6) { printf("st%d: TIMEOUT on DONE (%d)\n", me, count(done_from)); break; }
    }
    h2net_status(st, sizeof st);

    qsort(rtt, (size_t)nrtt, sizeof *rtt, cmpd);
    for (i = 1; i < MAXI; i++) {
        if (i == me || !heard[i]) continue;
        printf("st%d<-st%d: ping %d/%d  bcast %d/%d  pong %d/%d\n", me, i,
               ping_rx[i], frames, bcast_rx[i], frames, pong_rx[i], frames);
        if (ping_rx[i] < frames * 99 / 100 || bcast_rx[i] < frames * 99 / 100 || pong_rx[i] < frames * 99 / 100) ok = 0;
    }
    if (count(heard) < stations - 1) ok = 0;
    if (misdelivered) ok = 0;
    {
        /* flood counter in the status counts sends with no learned target: broadcasts + unknown unicasts */
        const char *fl = strstr(st, "flood=");
        long flood = fl ? atol(fl + 6) : -1;
        printf("st%d: rtt p50=%.2f p95=%.2f max=%.2f ms (n=%d)  misdelivered=%d malformed=%d unicast-flooded=%ld\n", me,
               nrtt ? rtt[nrtt / 2] : 0, nrtt ? rtt[nrtt * 95 / 100] : 0, nrtt ? rtt[nrtt - 1] : 0, nrtt,
               misdelivered, malformed, flood >= 0 ? flood - bcasts_sent : -1);
    }
    printf("st%d: %s\nst%d: %s\n", me, st, me, ok ? "PASS" : "FAIL");
    h2net_close();
    timeEndPeriod(1);
    return ok ? 0 : 1;
}

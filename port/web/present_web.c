/* Open Halo 2 (web): the presented frame on the page's canvas.
 *
 * First picture path, for the software pixel backend: at each flip the
 * executor hands over the guest address and pitch of the surface it finished.
 * The 640x480 surface is converted to RGBA here, on the executor's thread,
 * and the browser's main thread copies it to the canvas with putImageData.
 * A frame that arrives while the previous one is still waiting for the main
 * thread replaces nothing: it is dropped (the main thread is the slow side).
 *
 * The GL backend, once it runs on WebGL2, presents by itself and this path is
 * only the fallback (RECOMP_GPU=0), as the read-back window is on Windows.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <emscripten.h>
#include <emscripten/threading.h>

extern ptrdiff_t g_xbox_mem_offset;
int nv2a_gl_active(void);

#define FB_W 640
#define FB_H 480

static uint8_t s_rgba[FB_W * FB_H * 4];
static atomic_int s_pending;
static unsigned s_frames, s_dropped;

static void draw_on_main(void)
{
    EM_ASM({
        var c = Module.canvas;
        if (c.width !== $1 || c.height !== $2) { c.width = $1; c.height = $2; }
        var ctx = Module.h2ctx || (Module.h2ctx = c.getContext('2d'));
        // a copy: ImageData cannot wrap shared memory
        var px = new Uint8ClampedArray(HEAPU8.slice($0, $0 + $1 * $2 * 4).buffer);
        ctx.putImageData(new ImageData(px, $1, $2), 0, 0);
        Module.h2frames = (Module.h2frames || 0) + 1;
    }, s_rgba, FB_W, FB_H);
    atomic_store(&s_pending, 0);
}

void xbox_web_present(uint32_t fb_va, uint32_t pitch)
{
    const uint8_t *src;
    uint32_t bpp, x, y;

    if (!fb_va || !pitch || nv2a_gl_active())   /* the GL backend presents by itself */
        return;
    s_frames++;
    if (atomic_exchange(&s_pending, 1)) {
        s_dropped++;
        return;
    }
    bpp = pitch / FB_W;                       /* 4: X8R8G8B8, 2: R5G6B5 */
    src = (const uint8_t *)((uintptr_t)fb_va + g_xbox_mem_offset);
    for (y = 0; y < FB_H; y++) {
        const uint8_t *row = src + (size_t)y * pitch;
        uint8_t *out = s_rgba + (size_t)y * FB_W * 4;
        if (bpp == 4) {
            for (x = 0; x < FB_W; x++) {
                out[x * 4 + 0] = row[x * 4 + 2];
                out[x * 4 + 1] = row[x * 4 + 1];
                out[x * 4 + 2] = row[x * 4 + 0];
                out[x * 4 + 3] = 255;
            }
        } else if (bpp == 2) {
            for (x = 0; x < FB_W; x++) {
                uint32_t p = row[x * 2] | (row[x * 2 + 1] << 8);
                out[x * 4 + 0] = (uint8_t)(((p >> 11) & 31) * 255 / 31);
                out[x * 4 + 1] = (uint8_t)(((p >> 5) & 63) * 255 / 63);
                out[x * 4 + 2] = (uint8_t)((p & 31) * 255 / 31);
                out[x * 4 + 3] = 255;
            }
        } else {
            atomic_store(&s_pending, 0);
            return;
        }
    }
    emscripten_async_run_in_main_runtime_thread(EM_FUNC_SIG_V, draw_on_main);
}

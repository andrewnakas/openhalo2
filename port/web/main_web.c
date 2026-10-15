/* Open Halo 2: WebAssembly entry point.
 *
 * The same start-up as port/xr/src/main.c without anything that belongs to a
 * Windows process: no window, no fault handlers (device registers are reached
 * through recomp_mmio.h, see h2lift/mmio_pass.py), no log redirection. Runs
 * on a worker (PROXY_TO_PTHREAD), so the title's own blocking main loop is
 * allowed to block.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>

#include "recomp_types.h"

#include <emscripten.h>
#include <emscripten/wasmfs.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>

extern ptrdiff_t g_xbox_mem_offset;

int       xbox_MemoryLayoutInit(const void *xbe_data, size_t xbe_size);
ptrdiff_t xbox_GetMemoryOffset(void);
void     *xbox_GetMemoryBase(void);
void      xbox_kernel_init(void);
void      xbox_kernel_bridge_init(void);
void      xbox_path_init(const char *game_dir, const char *save_dir);
void      xbox_SetD3DSoftwareInterruptRoutine(unsigned int);
void      xbox_OhciInit(void);
void      xbox_Nv2aRegTrapInit(void);
void      xbox_WatchdogStart(void);
int       recomp_dispatch_init(void);
void      xbe_entry_point(void);

typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;

#define OPFS_DIR  "/opfs"            /* the origin's private file system */
#define GAME_DIR  OPFS_DIR "/game"
#define SAVE_DIR  save_dir()          /* /opfs/save, or H2_SAVE (a second tab's own disk) */
#define STACK_TOP 0x00F7FFF0u     /* as the boot log of the native build prints it */

static const char *save_dir(void)
{
    const char *e = getenv("H2_SAVE");
    return e && *e ? e : OPFS_DIR "/save";
}

static void def(const char *name, const char *value)
{
    if (!getenv(name))
        setenv(name, value, 1);
}

int main(void)
{
    FILE *f;
    long size;
    void *xbe;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* What this title needs from the runtime (main.c sets the same). */
    def("RECOMP_PB_EXEC", "1");
    def("RECOMP_VBLANK", "1");
    def("RECOMP_TITLE_KEVENTS", "1");
    def("RECOMP_USB", "1");
    def("RECOMP_NO_MIRRORS", "1");
    def("RECOMP_NO_TILED", "1");
    def("RECOMP_QUIET_PROGRESS", "1");   /* no line per 200 draws: every print crosses to the page */
    {   /* Sound: on unless ?env=RECOMP_SOUND=0 (the same switches main.c sets). */
        const char *snd = getenv("RECOMP_SOUND");
        if (!snd || *snd != '0') {
            def("RECOMP_AC97_READY", "1");
            def("RECOMP_APU_DSP_ACK_GPS", "0x810");
            def("RECOMP_APU_DSP_SENTINEL", "0x387108,0xCCCCCC");
            def("RECOMP_APU_MIXDOWN_ALL", "2");
        }
    }
    def("RECOMP_GPU", "1");            /* WebGL2 pixel backend; ?env=RECOMP_GPU=0 = software */

    printf("=== Open Halo 2 (web) ===\n");

    /* The disc files and the console's hard disk live in the origin's private
     * file system (OPFS), put there once by the page (dev.html, import). The
     * kernel layer's POSIX file code then reads and writes them as files: no
     * copy in memory, and saves persist. */
    {
        backend_t opfs;
        int g, v;
        struct stat st;

        printf("OPFS: creating backend\n");
        opfs = wasmfs_create_opfs_backend();
        printf("OPFS: backend %p\n", (void *)opfs);
        g = wasmfs_create_directory(OPFS_DIR, 0777, opfs);   /* a backend directory is the OPFS root */
        printf("OPFS: game dir %d\n", g);
        v = stat(SAVE_DIR, &st);
        printf("OPFS: %s %s, %s %s\n", GAME_DIR, g ? strerror(errno) : "mounted",
               SAVE_DIR, v ? strerror(errno) : "mounted");
        if (stat(GAME_DIR "/default.xbe", &st) != 0) {
            fprintf(stderr, "no " GAME_DIR "/default.xbe in OPFS: open the page with ?import=1 first\n");
            return 1;
        }
        if (getenv("RECOMP_WEB_FSCHECK")) {   /* the partition table as this side reads it */
            unsigned char b[16] = {0};
            char p0[256];
            FILE *pf;
            snprintf(p0, sizeof p0, "%s/Partition0.img", SAVE_DIR);
            pf = fopen(p0, "rb");
            if (pf) {
                size_t got;
                fseek(pf, 0, SEEK_END);
                printf("FSCHECK: Partition0.img size %ld\n", ftell(pf));
                fseek(pf, 2048, SEEK_SET);
                got = fread(b, 1, 16, pf);
                printf("FSCHECK: @2048 got %u: %02X %02X %02X %02X '%.16s'\n", (unsigned)got, b[0], b[1], b[2], b[3], b);
                fclose(pf);
            } else {
                printf("FSCHECK: cannot open Partition0.img: %s\n", strerror(errno));
            }
        }
    }
    f = fopen(GAME_DIR "/default.xbe", "rb");
    if (!f) {
        fprintf(stderr, "no " GAME_DIR "/default.xbe\n");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    xbe = malloc((size_t)size);
    if (!xbe || fread(xbe, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "cannot read default.xbe\n");
        return 1;
    }
    fclose(f);
    printf("XBE loaded: %ld bytes\n", size);

    if (!xbox_MemoryLayoutInit(xbe, (size_t)size)) {
        fprintf(stderr, "memory layout failed\n");
        return 1;
    }
    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%lX\n", (unsigned long)g_xbox_mem_offset);

    if (getenv("RECOMP_AC97_READY"))
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
    xbox_OhciInit();
    xbox_kernel_init();
    xbox_Nv2aRegTrapInit();
    xbox_path_init(GAME_DIR, SAVE_DIR);
    xbox_SetD3DSoftwareInterruptRoutine(0x003FF240u);
    { extern void h2_title_patches(const char *game_dir); h2_title_patches(GAME_DIR); }
    xbox_kernel_bridge_init();

    g_esp = STACK_TOP;
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable\n");
    xbox_WatchdogStart();
    printf("Starting game...\n");
    xbe_entry_point();
    printf("Game returned.\n");
    return 0;
}

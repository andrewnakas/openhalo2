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

#define GAME_DIR  "/game"
#define SAVE_DIR  "/save"
#define STACK_TOP 0x00F7FFF0u     /* as the boot log of the native build prints it */

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

    printf("=== Open Halo 2 (web) ===\n");
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

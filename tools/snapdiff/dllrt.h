/* runtime definitions for the differential DLL */
RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_seh_ebp, g_fs_base, g_last_ret_n;
RECOMP_TLS double g_fp_stack[8]; RECOMP_TLS int g_fp_top, g_df, g_fp_cmp;
RECOMP_TLS uint16_t g_fp_control_word = 0x027F, g_fp_cc = 0x4000;
RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3, g_mm4, g_mm5, g_mm6, g_mm7;
RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3, g_xmm4, g_xmm5, g_xmm6, g_xmm7;
RECOMP_TLS uint32_t g_recomp_eflags = 0x202, g_recomp_mxcsr = 0x1F80;
ptrdiff_t g_xbox_mem_offset; int g_force_return; uint32_t g_xbox_code_lo = 0x10000, g_xbox_code_hi = 0x600000;
volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE]; volatile uint32_t g_icall_trace_idx; volatile uint64_t g_icall_count;
volatile uint64_t g_icall_guard_hits, g_icall_guard_misses;
void recomp_debug_service(uint32_t s, uint32_t a) { (void)s; (void)a; }
int recomp_guest_longjmp(uint32_t b, uint32_t v) { (void)b; (void)v; RaiseException(0xE0000001u, 0, 0, 0); return 0; }
__declspec(dllexport) uint32_t g_fail_va; void recomp_icall_fail_log(uint32_t va) { g_fail_va = va; RaiseException(0xE0000002u, 0, 0, 0); }
void recomp_icall_not_code_log(uint32_t va) { g_fail_va = va | 1; RaiseException(0xE0000002u, 0, 0, 0); }
uint64_t xbox_ReadTimeStampCounter(void) { return 0; }
void recomp_trace_enter(const char *n, uint32_t va) { (void)n; (void)va; }
void recomp_trace_exit(const char *n, uint32_t va) { (void)n; (void)va; }
void recomp_trace_esp(const char *n, const char *t) { (void)n; (void)t; }
recomp_func_t dll_lookup(uint32_t va);
recomp_func_t recomp_lookup(uint32_t va) { return dll_lookup(va); }
int recomp_dispatch_init(void) { return 0; }
recomp_func_t recomp_lookup_kernel(uint32_t va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return 0; }
void recomp_san_abi(uint32_t va, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) { (void)va;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
void recomp_abi_violation_log(uint32_t va, uint32_t a, uint32_t b, uint32_t c, uint32_t d) { (void)va;(void)a;(void)b;(void)c;(void)d; }
void recomp_call_profile_hit(uint32_t va) { (void)va; }
uint32_t recomp_rep_guard(uint32_t site, uint32_t dst, uint32_t src, uint32_t count, uint32_t unit) { (void)site;(void)dst;(void)src;(void)unit; return count; }
void recomp_unimpl(const char *t, uint32_t va) { (void)t; (void)va; }

static uint8_t *g_base;
static uint8_t *g_contig;
__declspec(dllexport) uint64_t h_init(uint64_t size) {
    HANDLE m;
    g_base = (uint8_t *)VirtualAlloc(NULL, 0x100000000ull, MEM_RESERVE, PAGE_NOACCESS);
    if (!g_base) return 0;
    VirtualFree(g_base, 0, MEM_RELEASE);
    if (!VirtualAlloc(g_base, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return 0;
    m = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 64u << 20, NULL);
    g_contig = MapViewOfFileEx(m, FILE_MAP_ALL_ACCESS, 0, 0, 64u << 20, g_base + 0x80000000ull);
    if (!g_contig || !MapViewOfFileEx(m, FILE_MAP_ALL_ACCESS, 0, 0, 64u << 20, g_base + 0xF0000000ull)) return 0;
    g_xbox_mem_offset = (ptrdiff_t)g_base;
    return (uint64_t)(uintptr_t)g_base;
}
/* r: eax ecx edx ebx esp ebp seh_ebp esi edi eflags df fp_top fpcw (in/out); mm: 8 x u64 (in/out); fp: 8 doubles */
__declspec(dllexport) int h_run(uint32_t va, uint32_t *r, uint64_t *mm, double *fp) {
    recomp_func_t f = dll_lookup(va);
    if (!f) return -1;
    g_eax = r[0]; g_ecx = r[1]; g_edx = r[2]; g_ebx = r[3]; g_esp = r[4]; g_ebp = r[5]; g_seh_ebp = r[6];
    g_esi = r[7]; g_edi = r[8]; g_recomp_eflags = r[9]; g_df = (int)r[10]; g_fp_top = (int)r[11]; g_fp_control_word = (uint16_t)r[12];
    memcpy(g_fp_stack, fp, 64);
    g_mm0.q = mm[0]; g_mm1.q = mm[1]; g_mm2.q = mm[2]; g_mm3.q = mm[3]; g_mm4.q = mm[4]; g_mm5.q = mm[5]; g_mm6.q = mm[6]; g_mm7.q = mm[7];
    __try { f(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode() == 0xE0000001u || GetExceptionCode() == 0xE0000002u ? 3 : 2; }
    r[0] = g_eax; r[1] = g_ecx; r[2] = g_edx; r[3] = g_ebx; r[4] = g_esp; r[5] = g_ebp; r[6] = g_seh_ebp; r[7] = g_esi; r[8] = g_edi;
    mm[0] = g_mm0.q; mm[1] = g_mm1.q; mm[2] = g_mm2.q; mm[3] = g_mm3.q; mm[4] = g_mm4.q; mm[5] = g_mm5.q; mm[6] = g_mm6.q; mm[7] = g_mm7.q;
    return 0;
}
uint32_t recomp_mmio_read(uint32_t va, int size) { (void)va; (void)size; RaiseException(0xE0000001u, 0, 0, 0); return 0; }
void recomp_mmio_write(uint32_t va, uint32_t val, int size) { (void)va; (void)val; (void)size; RaiseException(0xE0000001u, 0, 0, 0); }

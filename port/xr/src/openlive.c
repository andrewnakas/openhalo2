/* Open Live: the title's Xbox Live API answered by host code
 * (docs/OPEN_LIVE_DESIGN.md).
 *
 * h2lift/xo_hle_pass.py puts a wrapper on each public XOnline entry point
 * (the thunk table at 0x3A0620.., XOnlineStartup/Cleanup), on the XNet QoS
 * calls, and (trace only) on the XNet key and address calls. Each wrapper
 * calls xo_hook(va, lifted) when g_xo_mode is non-zero.
 *
 *   RECOMP_OPEN_LIVE=1   answer the calls here (local gamertag account,
 *                        peer-to-peer facilitator, our own title-storage files)
 *   RECOMP_OPEN_LIVE=0   (default) the lifted library, unchanged
 *   RECOMP_XO_TRACE=1    one line per call: name, arguments, result, bytes
 *                        popped (stdcall argument size), caller; 20 lines per
 *                        entry point, =2 for all of them
 *
 * Calling convention (all entry points are __stdcall): the return address is
 * at [esp], the arguments above it; the callee pops 4 + 4n bytes and returns
 * in eax. A handler here does the same and records RECOMP_NOTE_RET so the
 * sanitizer build's ABI check holds.
 *
 * Every structure layout used here is our own, from the title's code (the
 * CC0 decomp's field use, the lifted C) and measured guest memory, as noted
 * at each definition. No XDK header, library or sample was used.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#define XO_TLS __declspec(thread)
#else
#define XO_TLS _Thread_local
#endif

#include "openlive.h"

extern XO_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern XO_TLS uint32_t g_ebx, g_esi, g_edi;
#ifdef RECOMP_SANITIZE
extern XO_TLS uint32_t g_last_ret_n;   /* the sanitizer build's ABI check */
#endif
extern ptrdiff_t g_xbox_mem_offset;

int g_xo_mode = -1;          /* -1 = not read yet, 0 = off, else XO_MODE_* bits */
static int s_trace;
static unsigned s_trace_lines[512];
static unsigned s_unanswered[512];

/* ---- names (docs/OPEN_LIVE_DESIGN.md 1.2; the trace confirms them) ------- */

static const struct { uint32_t va; const char *name; } k_names[] = {
    {0x3A0C39, "XOnlineStartup"}, {0x3A0D36, "XOnlineCleanup"},
    {0x3A0620, "XOnlineTaskContinue"}, {0x3A062B, "XOnlineTaskClose"},
    {0x3A0636, "XOnlineGetUsers"}, {0x3A0641, "XOnlineLogon"},
    {0x3A0650, "XOnlineLogonTaskGetResults"}, {0x3A065B, "XOnlineChangeLogonUsers"},
    {0x3A0666, "XOnlineChangeLogonUsersTaskGetResults"}, {0x3A0671, "XOnlineGetLogonUsers"},
    {0x3A067C, "XOnlineGetServiceInfo"}, {0x3A0687, "XOnlineSaveLogonState"},
    {0x3A0692, "XOnline?0692"}, {0x3A069D, "XOnlineOfferingIsNewContentAvailable"},
    {0x3A06A8, "XOnlineMatchSessionCreate"}, {0x3A06B7, "XOnlineMatchSessionUpdate"},
    {0x3A06E7, "XOnlineMatchSessionGetInfo"}, {0x3A06F2, "XOnlineMatchSessionDelete"},
    {0x3A0710, "XOnlineMatchSessionFindFromID"}, {0x3A072E, "XOnlineMatchSearch"},
    {0x3A073D, "XOnlineMatchSearchGetResults"}, {0x3A0748, "XOnlineMatchSearchParse"},
    {0x3A0753, "XOnlineMatchSearchResultsLen"}, {0x3A075E, "XOnlineTitleIdIsSameTitle?075E"},
    {0x3A0769, "XOnlineTitleIdIsSameTitle?0769"}, {0x3A0774, "XOnlineNotificationSetState"},
    {0x3A0798, "XOnlineGetNotificationEx"}, {0x3A07A3, "XOnlineFriendsStartup"},
    {0x3A07AE, "XOnlineFriendsEnumerate"}, {0x3A07B9, "XOnlineFriendsEnumerateFinish"},
    {0x3A07C4, "XOnlineFriendsGetLatest"}, {0x3A07CF, "XOnlineFriendsGetTitleName"},
    {0x3A07DA, "XOnlineFriendsRemove"}, {0x3A07E5, "XOnlineFriendsGameInvite"},
    {0x3A0806, "XOnlineFriendsAnswerRequest"}, {0x3A0811, "XOnlineFriendsAnswerGameInvite"},
    {0x3A081C, "XOnlineFriendsJoinGame"}, {0x3A0827, "XOnlineFriendsGetAcceptedGameInvite"},
    {0x3A0832, "XOnlineMutelistGet"}, {0x3A0841, "XOnlineMutelistStartup"},
    {0x3A084C, "XOnlineMutelistAdd"}, {0x3A086E, "XOnlineMutelistRemove"},
    {0x3A0890, "XOnlineFeedbackSend"}, {0x3A08C0, "XOnlineStatWrite"},
    {0x3A08CB, "XOnlineStatRead"}, {0x3A08D6, "XOnlineStatReadGetResult"},
    {0x3A11AE, "XOnlineGetNatType"}, {0x3A695E, "XOnlinePresenceAdd"},
    {0x3A6969, "XOnlinePresenceClear"}, {0x3A6F9B, "XOnlinePresenceSubmit"},
    {0x3A6FA6, "XOnlinePresenceGetLatest"}, {0x3A8316, "XOnlinePresenceInit"},
    {0x3A8321, "XOnlineGameInviteSend"}, {0x3A834E, "XOnlineGameInviteAnswer"},
    {0x3A835D, "XOnlineFriendsRequestEx"}, {0x3A838A, "XOnlineFriendsRequestByNameEx"},
    {0x3A8399, "XOnlineTeams?8399"}, {0x3A83D4, "XOnlineTeams?83D4"}, {0x3A8407, "XOnlineTeams?8407"},
    {0x3A9D65, "XOnlineMessageDetailsGetResultsProperty"}, {0x3A9D74, "XOnlineMessage?9D74"},
    {0x3A9D83, "XOnlineMessageDownloadAttachment?"}, {0x3A9D8E, "XOnlineMessageCreate"},
    {0x3A9DB8, "XOnlineMessageDestroy"}, {0x3A9DC3, "XOnlineMessageSetProperty"},
    {0x3AB04A, "XOnlineMessageEnumerate"}, {0x3AB055, "XOnlineMessage?B055"},
    {0x3AB060, "XOnlineMessageSend"}, {0x3AB2A6, "XOnlineMessageDetails"},
    {0x3AB6A6, "XOnlineStatWriteGetResult"}, {0x3AB6B1, "XOnlineStorageCreateServerPath"},
    {0x3AB6DB, "XOnlineStorageDownload"}, {0x3AB6EA, "XOnlineStorageDownloadToMemory"},
    {0x3AB6F9, "XOnlineStorageDownloadToMemoryGetResults"}, {0x3AD149, "XOnlineStorageUploadByServerPath"},
    {0x3AD176, "XOnlineStorageUploadFromMemory"}, {0x3AD1A6, "XOnlineTaskProgress"},
    {0x3ADC4C, "XOnlineArbitrationCreateRoundID"}, {0x3AE32B, "XOnlineArbitrationRegister"},
    {0x3AE33A, "XOnlineArbitrationExtendRound"}, {0x3AE36A, "XOnlineArbitrationReport"},
    {0x3AEBA5, "XOnlineTeams?EBA5"}, {0x3AEBB0, "XOnlineTeams?EBB0"},
    {0x3AFB80, "XOnlineTeams?FB80"}, {0x3AFB8F, "XOnlineTeams?FB8F"}, {0x3AFBB9, "XOnlineTeams?FBB9"},
    {0x3AFBF1, "XOnlineTeams?FBF1"}, {0x3AFC26, "XOnlineTeams?FC26"}, {0x3AFC50, "XOnlineTeams?FC50"},
    {0x3AFC76, "XOnlineTeams?FC76"}, {0x3AFCA3, "XOnlineTeams?FCA3"},
    {0x3B0254, "XOnlineQuerySearch"}, {0x3B0263, "XOnlineQuerySearchGetResults"},
    {0x3CD0C0, "XNetRandom"}, {0x3CD0CB, "XNetCreateKey"}, {0x3CD0D6, "XNetRegisterKey"},
    {0x3CD0E1, "XNetUnregisterKey"}, {0x3CD0EC, "XNetXnAddrToInAddr"},
    {0x3CD103, "XNetServerToInAddr?"}, {0x3CD147, "XNetQosListen"}, {0x3CD156, "XNetQosLookup"},
    {0x3CD172, "XNetQosRelease"}, {0x3CD17D, "XNetGetTitleXnAddr"},
    {0x3CD32D, "XNetInAddrToXnAddr"}, {0x3CD344, "XNet?D344"}, {0x3CD34F, "XNetConnect"},
    {0x3CD35A, "XNetGetConnectStatus"}, {0x3CD365, "XNetQosServiceLookup"},
    {0x3CD576, "XNetGetEthernetLinkStatus"}, {0x3CD58D, "XNetStartup"}, {0x3CD688, "XNetCleanup"},
};

const char *xo_name(uint32_t va)
{
    size_t i;
    for (i = 0; i < sizeof k_names / sizeof k_names[0]; i++)
        if (k_names[i].va == va)
            return k_names[i].name;
    return "?";
}

static unsigned name_index(uint32_t va)
{
    size_t i;
    for (i = 0; i < sizeof k_names / sizeof k_names[0]; i++)
        if (k_names[i].va == va)
            return (unsigned)i;
    return 511;
}

/* ---- guest memory --------------------------------------------------------- */

uint8_t *xo_ptr(uint32_t va) { return (uint8_t *)((uintptr_t)va + g_xbox_mem_offset); }
uint32_t xo_rd32(uint32_t va) { uint32_t v; memcpy(&v, xo_ptr(va), 4); return v; }
void     xo_wr32(uint32_t va, uint32_t v) { memcpy(xo_ptr(va), &v, 4); }
uint32_t xo_arg(int i) { return xo_rd32(g_esp + 4 + 4u * (uint32_t)i); }
uint32_t xo_arg_addr(int i) { return g_esp + 4 + 4u * (uint32_t)i; }

uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);
void     xbox_HeapFree(uint32_t xbox_va);

uint32_t xo_alloc(uint32_t size)
{
    uint32_t va = xbox_HeapAlloc(size, 16);
    if (va) memset(xo_ptr(va), 0, size);
    return va;
}

void xo_free(uint32_t va) { if (va) xbox_HeapFree(va); }

/* Calls a lifted __stdcall function from a handler, on the calling guest
 * thread: arguments and a return address on the guest stack, the callee
 * pops them. eax is the result; ecx and edx are the callee's (the API the
 * handler answers may clobber them too). */
uint32_t xo_guest_call(void (*fn)(void), int nargs, const uint32_t *args)
{
    uint32_t sp = g_esp;
    int i;
    for (i = nargs - 1; i >= 0; i--) { g_esp -= 4; xo_wr32(g_esp, args[i]); }
    g_esp -= 4;
    xo_wr32(g_esp, 0x0E5AC0DEu);
    fn();
    if (g_esp != sp) {
        fprintf(stderr, "  [XO] guest call left esp off by %d\n", (int)(g_esp - sp));
        g_esp = sp;
    }
    return g_eax;
}

void xo_return(uint32_t eax, int nargs)
{
    g_eax = eax;
    g_esp += 4 + 4u * (uint32_t)nargs;
#ifdef RECOMP_SANITIZE
    g_last_ret_n = 4u * (uint32_t)nargs;
#endif
}

/* ---- the hook -------------------------------------------------------------- */

static void xo_init(void)
{
    const char *e = getenv("RECOMP_OPEN_LIVE");
    const char *t = getenv("RECOMP_XO_TRACE");
    int mode = 0;
    s_trace = t && *t ? atoi(t) : 0;
    if (s_trace) mode |= XO_MODE_TRACE;
    if (e && *e && *e != '0') mode |= XO_MODE_LIVE;
    if (mode & XO_MODE_LIVE)
        xo_live_init();
    fprintf(stderr, "  [XO] Open Live %s%s\n", (mode & XO_MODE_LIVE) ? "on" : "off (the title's own library)",
            s_trace ? ", tracing" : "");
    g_xo_mode = mode;
}

static int trace_this(uint32_t va)
{
    unsigned k = name_index(va);
    if (s_trace >= 2) return 1;
    return s_trace_lines[k]++ < 20;
}

void xo_hook(uint32_t va, void (*lifted)(void))
{
    uint32_t sp, ret, a[8];
    int i, handled = 0;

    if (g_xo_mode < 0)
        xo_init();
    sp = g_esp;
    ret = xo_rd32(sp);
    for (i = 0; i < 8; i++)
        a[i] = xo_rd32(sp + 4 + 4u * (uint32_t)i);
    if (g_xo_mode & XO_MODE_LIVE)
        handled = xo_live_call(va);
    if (!handled) {
        unsigned k = name_index(va);
        if ((g_xo_mode & XO_MODE_LIVE) && va < 0x3CD000 && va != 0x3A0C39 && va != 0x3A0D36
            && !s_unanswered[k]++)
            fprintf(stderr, "  [XO] not answered by Open Live: %s (0x%06X) from %08X, the title's library runs\n",
                    xo_name(va), va, ret);
        if (g_xo_mode & XO_MODE_LIVE) {
            uint32_t mark = xo_match_before(va);    /* may point XNet at unmarked ids */
            lifted();
            xo_match_unmark_done(mark);
            xo_match_after(va, a, g_eax);
        } else {
            lifted();
        }
    }
    if ((g_xo_mode & XO_MODE_TRACE) && trace_this(va)) {
        int popped = (int)(g_esp - sp) - 4;
        fprintf(stderr, "  [XO] %s%s(%08X %08X %08X %08X %08X %08X %08X %08X) = %08X  pop=%d from %08X\n",
                handled ? "HLE " : "", xo_name(va), a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
                g_eax, popped, ret);
        if (!handled)
            xo_trace_detail(va, a, g_eax);
    }
}

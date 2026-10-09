"""Sampling profiler for halo2_recomp.exe: suspends each thread, reads RIP, maps
it through the linker .map file. Prints the top functions overall and per thread.
usage: python sample_prof.py <pid> <seconds> [map file] [interval_ms]"""
import bisect, collections, ctypes, re, sys, time
from ctypes import wintypes as W
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
pid = int(sys.argv[1]); secs = float(sys.argv[2])
mapf = sys.argv[3] if len(sys.argv) > 3 else 'D:/n64work/halo2/build/xr/halo2_recomp.map'
interval = (float(sys.argv[4]) if len(sys.argv) > 4 else 5) / 1000.0

class THREADENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', W.DWORD), ('cntUsage', W.DWORD), ('th32ThreadID', W.DWORD),
                ('th32OwnerProcessID', W.DWORD), ('tpBasePri', W.LONG), ('tpDeltaPri', W.LONG), ('dwFlags', W.DWORD)]
class MODULEENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', W.DWORD), ('th32ModuleID', W.DWORD), ('th32ProcessID', W.DWORD), ('GlblcntUsage', W.DWORD),
                ('ProccntUsage', W.DWORD), ('modBaseAddr', ctypes.c_void_p), ('modBaseSize', W.DWORD),
                ('hModule', W.HMODULE), ('szModule', ctypes.c_char * 256), ('szExePath', ctypes.c_char * 260)]
k32.CreateToolhelp32Snapshot.restype = W.HANDLE
k32.OpenThread.restype = W.HANDLE

def threads():
    snap = k32.CreateToolhelp32Snapshot(4, 0); te = THREADENTRY32(); te.dwSize = ctypes.sizeof(te); out = []
    ok = k32.Thread32First(W.HANDLE(snap), ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid: out.append(te.th32ThreadID)
        ok = k32.Thread32Next(W.HANDLE(snap), ctypes.byref(te))
    k32.CloseHandle(W.HANDLE(snap)); return out

def modules():
    snap = k32.CreateToolhelp32Snapshot(0x18, pid); me = MODULEENTRY32(); me.dwSize = ctypes.sizeof(me); out = []
    ok = k32.Module32First(W.HANDLE(snap), ctypes.byref(me))
    while ok:
        out.append((me.modBaseAddr or 0, me.modBaseSize, me.szModule.decode('latin1')))
        ok = k32.Module32Next(W.HANDLE(snap), ctypes.byref(me))
    k32.CloseHandle(W.HANDLE(snap)); return sorted(out)

# linker map: " 0001:00000000       name       0000000140001000 f   obj"
syms = []
pref = 0x140000000
for line in open(mapf, errors='replace'):
    m = re.match(r'\s*Preferred load address is ([0-9a-fA-F]+)', line)
    if m: pref = int(m.group(1), 16)
    m = re.match(r'\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s+(f\s+)?(i\s+)?(\S+)', line)
    if m and int(m.group(2), 16) >= pref: syms.append((int(m.group(2), 16) - pref, m.group(1), m.group(5)))
syms.sort(); addrs = [s[0] for s in syms]
mods = modules(); exe = [m for m in mods if m[2].lower().endswith('.exe')][0]

CTX = (ctypes.c_ubyte * 1232)   # x64 CONTEXT, 16-byte aligned
buf = ctypes.create_string_buffer(1232 + 16)
base = ctypes.addressof(buf); base += (-base) % 16
ctx = CTX.from_address(base)
def rip_of(h):
    ctypes.memset(base, 0, 1232)
    ctypes.c_uint32.from_address(base + 0x30).value = 0x100001   # CONTEXT_CONTROL
    if not k32.GetThreadContext(W.HANDLE(h), ctypes.c_void_p(base)): return None
    return ctypes.c_uint64.from_address(base + 0xF8).value

def name(rip):
    if exe[0] <= rip < exe[0] + exe[1]:
        i = bisect.bisect_right(addrs, rip - exe[0]) - 1
        return syms[i][1] if i >= 0 else 'exe+%X' % (rip - exe[0])
    for b, s, n in mods:
        if b <= rip < b + s: return '[' + n + ']'
    return '[?]'

tot = collections.Counter(); per = collections.defaultdict(collections.Counter); n = 0
handles = {}
end = time.time() + secs
while time.time() < end:
    for t in threads():
        h = handles.get(t)
        if h is None:
            h = handles[t] = k32.OpenThread(0x1A, False, t)   # SUSPEND_RESUME|GET_CONTEXT|QUERY_INFORMATION
        if not h: continue
        if k32.SuspendThread(W.HANDLE(h)) == 0xFFFFFFFF: continue
        r = rip_of(h); k32.ResumeThread(W.HANDLE(h))
        if r is None: continue
        f = name(r); tot[f] += 1; per[t][f] += 1
    n += 1; time.sleep(interval)
print('%d sample rounds, %d threads' % (n, len(per)))
idle = ('[ntdll.dll]', '[win32u.dll]', '[KERNELBASE.dll]', '[KERNEL32.DLL]')
busy = sum(c for f, c in tot.items() if f not in idle)
print('== overall, excluding waits (%d busy samples)' % busy)
for f, c in tot.most_common(45):
    if f not in idle: print('%6.2f%%  %s' % (100.0 * c / max(busy, 1), f))
print('== per thread: busy share of its samples, top 3')
for t, c in sorted(per.items(), key=lambda kv: -sum(v for f, v in kv[1].items() if f not in idle))[:10]:
    b = sum(v for f, v in c.items() if f not in idle); s = sum(c.values())
    print('  tid %-6d busy %5.1f%%  %s' % (t, 100.0 * b / s, ', '.join('%s %d' % (f, v) for f, v in c.most_common(4) if f not in idle)))

"""stall_sym.py <log> <linker .map> [exe name] [--all]: symbolise RECOMP_STALL_MS
samples ("[STALL] no flip for N ms" + one "[STALL]  tid ..." line per thread)
from the toolkit's kernel_bridge.c. Program offsets (<exe>+hex) map through the
.map file; DLL frames stay module+offset. Threads parked in their usual idle
wait at every sample (thread-pool workers, driver threads) are left out unless
--all; what remains is who was doing what while no frame came."""
import bisect, collections, re, sys

log, mapf = sys.argv[1], sys.argv[2]
exe = (sys.argv[3] if len(sys.argv) > 3 and not sys.argv[3].startswith('--') else None)
show_all = '--all' in sys.argv
pref, syms = 0x140000000, []
for line in open(mapf, errors='replace'):
    m = re.match(r'\s*Preferred load address is ([0-9a-fA-F]+)', line)
    if m:
        pref = int(m.group(1), 16)
    m = re.match(r'\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s+(f\s+)?(i\s+)?(\S+)', line)
    if m and int(m.group(2), 16) >= pref:
        syms.append((int(m.group(2), 16) - pref, m.group(1)))
syms.sort()
addrs = [s[0] for s in syms]


def sym(fr):
    m = re.match(r'(\S+?\.exe)\+([0-9a-f]+)$', fr, re.I)
    if not m or (exe and m.group(1).lower() != exe.lower()):
        return fr
    off = int(m.group(2), 16) - 0x1000 + 0x1000   # map RVAs are image offsets
    i = bisect.bisect_right(addrs, off) - 1
    return '%s+%x' % (syms[i][1], off - syms[i][0]) if i >= 0 else fr


stalls, cur = [], None
for line in open(log, errors='replace'):
    m = re.match(r'\[STALL\] no flip for (\d+) ms', line)
    if m:
        cur = {'ms': int(m.group(1)), 'th': []}
        stalls.append(cur)
        continue
    m = re.match(r'\[STALL\]  tid (\d+) start (\S+): (.*)', line.rstrip())
    if m and cur is not None:
        fr = [f.strip() for f in m.group(3).split('<-')]
        cur['th'].append((m.group(1), m.group(2), fr))
    m = re.match(r'\[STALL\] over after (\d+) ms', line)
    if m and stalls:
        stalls[-1]['over'] = int(m.group(1))

# a thread's "idle" stack = the one it shows in most samples
seen = collections.defaultdict(collections.Counter)
for s in stalls:
    for tid, st, fr in s['th']:
        seen[tid][tuple(fr[:6])] += 1
idle = {t: c.most_common(1)[0][0] for t, c in seen.items()
        if c.most_common(1)[0][1] >= max(3, 0.8 * sum(c.values()))}
print('%d samples; %d threads always in one wait (hidden)' % (len(stalls), len(idle)))
for s in stalls:
    print('--- no flip for %d ms' % s['ms'])
    for tid, st, fr in s['th']:
        if not show_all and idle.get(tid) == tuple(fr[:6]):
            continue
        print('  tid %s (%s)' % (tid, sym(st)))
        print('     ' + ' <- '.join(sym(f) for f in fr))

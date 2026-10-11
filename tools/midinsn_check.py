"""midinsn_check.py [functions.json]

Lists functions that begin before the function ahead of them ends, other than
the alias entries that share a body on purpose (tail_jump_alias). Such a
start is a false one, a call or pointer read out of bytes that were not code,
landing inside an instruction, and it truncates the real function: its lifted
C falls off its end without restoring registers or popping its arguments.

One of these (0x003D41DA inside XNET's 0x003D4191) made every system-link
join fail; tools/disasm/functions.py now rejects that shape. This is the
check that the rule holds after a re-lift: it should print 1 or fewer (the
remaining one, 0x00408940, is a 2-byte overlap between two seeded entries).
"""
import json
import sys

path = sys.argv[1] if len(sys.argv) > 1 else \
    'D:/n64work/halo2/third_party/xboxrecomp/tools/disasm/output/functions.json'
f = json.load(open(path))
fs = f if isinstance(f, list) else f.get('functions', f)
if isinstance(fs, dict):
    fs = list(fs.values())
fs = sorted(fs, key=lambda x: int(x['start'], 16))
n = 0
for a, b in zip(fs, fs[1:]):
    if int(b['start'], 16) < int(a['end'], 16) \
            and 'tail_jump_alias' not in (a['detection_method'], b['detection_method']):
        n += 1
        print('%s-%s (%s) is cut by %s-%s (%s, called by %s)' % (
            a['start'], a['end'], a['detection_method'], b['start'], b['end'],
            b['detection_method'], ','.join(b.get('called_by', [])[:3]) or 'nothing'))
print('%d overlapping non-alias functions of %d' % (n, len(fs)))

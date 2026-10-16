"""methoddiff.py <trace.bin> [method hex ...]

For each voice method write, compares the selected voice's descriptor
before and after (RECOMP_APU_TRACE_DESC=1) and tallies, per method, which
descriptor words changed and how the new word relates to the written value:
  =      word == value
  m:XXXXXXXX  bits that changed are within mask (union shown)
Prints a few raw examples per method too.
"""
import sys, os
from collections import defaultdict, Counter
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from apu_trace import load_full

us, off, size, wr, v, desc = load_full(sys.argv[1])
want = set(int(a, 16) for a in sys.argv[2:])
stats = defaultdict(Counter)
masks = defaultdict(lambda: defaultdict(int))
examples = defaultdict(list)
count = Counter()
i = 0
n = len(us)
while i < n:
    if (i + 1) in desc and desc[i + 1][0] == 0x7FFFFFFE and (i + 2) in desc and i not in desc:
        pre = desc[i + 1][2]; post = desc[i + 2][2]; vo = desc[i + 1][1]
        o = int(off[i]); val = int(v[i])
        count[o] += 1
        ch = [k for k in range(32) if pre[k] != post[k]]
        key = ' '.join('+%02X%s' % (k * 4, '=' if post[k] == val else '') for k in ch) or '-'
        stats[o][key] += 1
        for k in ch:
            masks[o][k] |= int(pre[k]) ^ int(post[k])
        if len(examples[o]) < 4 and ch:
            examples[o].append('v%d val=%08X ' % (vo, val) + ' '.join(
                '+%02X:%08X->%08X' % (k * 4, pre[k], post[k]) for k in ch))
        i += 3
        continue
    i += 1
for o in sorted(count):
    if want and o not in want:
        continue
    print('%05X n=%d  %s' % (o, count[o], ' | '.join('%s x%d' % kv for kv in stats[o].most_common(5))))
    print('        masks: ' + ' '.join('+%02X:%08X' % (k * 4, m) for k, m in sorted(masks[o].items())))
    for e in examples[o]:
        print('        ' + e)

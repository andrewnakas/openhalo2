"""entry_maps.csv from the lanes' bench notes: every 'true/real/right/forced map ...' they read off
retail's disassembly, expanded to one location per parameter (p0<-ecx, p1<-stk0, ...)."""
import csv, json, re, sys

REG = r'(?:e[abcd]x|e[sd]i|ebp|xmm\d)'
LOC = r'(?:%s|stk\d+(?:(?:-|\.\.)\d+)?)' % REG


def expand(tok):
    m = re.match(r'stk(\d+)(?:(?:-|\.\.)(\d+))?$', tok)
    if m:
        a, b = int(m.group(1)), int(m.group(2) or m.group(1))
        return ['stk%d' % i for i in range(a, b + 1)]
    return [tok]


def parse(text):
    text = text.replace('&lt;', '<').replace(' ', '')
    if '<-' in text:   # p0<-ecx,p1..p3<-stk0..2,p2/p3<-edx/ecx
        slots = {}
        for part in re.findall(r'p[\d./p]+<-[\w./-]+', text):
            ps, ls = part.split('<-')
            if '/' in ps:
                for p, l in zip(ps.split('/'), ls.split('/')):
                    slots[int(p.lstrip('p'))] = l
                continue
            m = re.match(r'p(\d+)(?:\.\.p?(\d+))?$', ps)
            if not m:
                return None
            a, b = int(m.group(1)), int(m.group(2) or m.group(1))
            locs = expand(ls)
            if len(locs) != b - a + 1:
                return None
            for i, l in zip(range(a, b + 1), locs):
                slots[i] = l
        if not slots or sorted(slots) != list(range(len(slots))):
            return None
        return [slots[i] for i in range(len(slots))]
    toks = [t for t in text.split(',') if t]
    if not toks or not all(re.fullmatch(LOC, t) for t in toks):
        return None
    return [x for t in toks for x in expand(t)]


KEY = re.compile(r'(?:true|real|right|correct|forced)\s+(?:entry\s+)?(?:map|entry|one)\s*(?:is|\(|:)?\s*\(?((?:[^.;()]|\.\.)+)', re.I)
rows = {}
for path in sys.argv[1:]:
    r = json.load(open(path))
    r = r if isinstance(r, list) else r.get('result', r)
    for lane in r:
        for b in lane['bench_suspect']:
            for m in KEY.finditer(b['note'].replace('&lt;', '<')):
                seg = re.split(r'\s(?:is|was|passes|with|and|\(|lies|never|With)\b', m.group(1).strip())[0]
                seg = re.sub(r'^\(|\)$', '', seg.strip()).rstrip(',')
                locs = parse(seg)
                if locs:
                    rows.setdefault(b['va'], set()).add(', '.join('p%d<-%s' % (i, l) for i, l in enumerate(locs)))
with open(sys.argv[1].rsplit('\\', 1)[0] + '\\entry_maps.csv', 'w', newline='') as f:
    w = csv.writer(f)
    w.writerow(['va', 'map', 'evidence'])
    for va in sorted(rows):
        for mp in sorted(rows[va]):
            w.writerow([va, mp, 'lane notes, retail disassembly'])
print('%d maps for %d functions' % (sum(len(v) for v in rows.values()), len(rows)))

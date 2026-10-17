"""perf_summary.py <run dir> [from_mark] [seconds] [skip_s]: the executor's frame
rate and time split over a window of a headless run (tools/ingame.sh-style
out.log with RECOMP_PB_REPORT_MS=5000 and marks.txt).

The window starts at the first report after marks.txt's <from_mark> (default
"ingame"; "mpgame" for the multiplayer rigs) plus skip_s seconds and lasts
<seconds> (default 125: the Cairo opening cinematic). Prints the mean fps
(flips / time), the slowest 5 s, and per frame: executor busy (scan), vertex
program transform, triangle setup, GL flushes ([XF] / [GPT] lines)."""
import re, sys

d = sys.argv[1]
mark = sys.argv[2] if len(sys.argv) > 2 else 'ingame'
secs = float(sys.argv[3]) if len(sys.argv) > 3 else 125.0
skip = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0
flips0 = None
for line in open(d + '/marks.txt', errors='replace'):
    m = re.match(r'MARK (\S+) at flips (\d+)', line)
    if m and m.group(1) == mark:
        flips0 = int(m.group(2))
    elif line.startswith('MARK ' + mark + ' '):
        flips0 = -1                      # mpgame mark carries no flips: from its time
if flips0 is None:
    sys.exit('no mark %s in %s/marks.txt' % (mark, d))

# Reports: a [GPU] fps line, then [XF] and [GPT] lines of the same interval.
reps, cur = [], None
for line in open(d + '/out.log', errors='replace'):
    m = re.match(r'\[GPU\] ([\d.]+) fps \((\d+) flips\)', line)
    if m:
        cur = {'fps': float(m.group(1)), 'flips': int(m.group(2))}
        reps.append(cur)
        continue
    if cur is None:
        continue
    m = re.match(r'\[XF\] transform (\d+) ms \((\d+) vertices run, (\d+) indices, (\d+) batches(?:, (\d+) vertices on threads)?\) \| setup (\d+) ms \| gl flush (\d+) ms', line)
    if m:
        cur.update(xf=int(m.group(1)), verts=int(m.group(2)), idx=int(m.group(3)),
                   batches=int(m.group(4)), mt=int(m.group(5) or 0), setup=int(m.group(6)), flush=int(m.group(7)))
    m = re.match(r'\[GLT\] download (\d+) ms/\d+ upload \d+ ms/\d+ surfcheck \d+ ms/\d+ texhash (\d+) ms/\d+ texdecode (\d+) ms/\d+ draw (\d+) ms/\d+ query (\d+) ms', line)
    if m:
        cur.update(dl=int(m.group(1)), hash=int(m.group(2)), dec=int(m.group(3)),
                   draw=int(m.group(4)), query=int(m.group(5)))
    m = re.match(r'\[GPT\] .*scan (\d+) ms', line)
    if m:
        cur['scan'] = int(m.group(1))
if flips0 < 0:
    # the mark has no flip count: the report nearest its line in out.log is unknown,
    # so take the last reports instead (the map runs to the end of the log)
    start = max(0, len(reps) - int(secs / 5.0) - 1)
else:
    start = next(i for i, r in enumerate(reps) if r['flips'] >= flips0)
start += int(skip / 5.0)
win = reps[start + 1:start + 1 + int(secs / 5.0)]
if not win:
    sys.exit('no reports in the window')
n = len(win)
flips = win[-1]['flips'] - reps[start]['flips']
fps = flips / (5.0 * n)
def per_frame(k):
    v = [r.get(k) for r in win if k in r]
    return sum(v) / max(flips, 1) if v else float('nan')
print('%s: %d reports from flips %d: %.1f fps mean, slowest 5 s %.1f, fastest %.1f'
      % (d.split('/')[-1], n, reps[start]['flips'], fps, min(r['fps'] for r in win),
         max(r['fps'] for r in win)))
print('  per frame (ms): executor %.1f | transform %.1f | setup %.1f | gl flush %.1f'
      ' | vertices run %.0f (%.0f on worker threads), batches %.0f'
      % (per_frame('scan'), per_frame('xf'), per_frame('setup'), per_frame('flush'),
         per_frame('verts'), per_frame('mt'), per_frame('batches')))
print('  GL (ms per frame, sampled): draw %.1f of which occlusion-query waits %.1f |'
      ' texture decode+upload %.1f | texture hashing %.1f | surface downloads %.1f'
      % (per_frame('draw'), per_frame('query'), per_frame('dec'), per_frame('hash'),
         per_frame('dl')))

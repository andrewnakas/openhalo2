"""cov_drive.py crawl <report dir> <depth> <maxrows> <tag>

Blind menu crawler for one headless copy started by rigs_link.sh / rigs_cov.sh (pad.txt live pad,
frame dumps through tools/shot.sh). From the screen the copy is on, it walks the list: for each
row it presses A, saves what opened as <tag>_<row>[_<row>...].png, crawls that screen too while
depth allows, and backs out with B until the screen it started from is back. Rows are walked with
down presses; the walk stops when a down press brings back a row already seen (the list wrapped
or ended). Nothing is known about the menus beforehand: screens are told apart by their title
band and a small thumbnail, so this reaches every row a pad reaches, including ones that start
something (a game, a search, a dialog) -- those are left with B, and when B does not bring the
starting screen back the crawl reports 'lost' (exit 3) and the caller navigates back itself.

Every step is one line in <report dir>/crawl.txt (path, distance, what happened).
SKIP=<path>,<path>... skips those rows (rows that would start a match etc.); a path is the tag
and row numbers joined by _ (setup_0 = row 0 of the crawl tagged setup), and an entry ending in *
skips everything under it (setup_0_* = the rows of the screen row 0 opens).
CRAWL_MAX_STEPS (default 120) and CRAWL_MAX_MIN (default 60) bound one crawl.
"""
import os
import subprocess
import sys
import time

from PIL import Image

T = os.environ.get('RIG_TOOLS', os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WAIT_A = float(os.environ.get('CRAWL_WAIT_A', '6'))
WAIT_B = float(os.environ.get('CRAWL_WAIT_B', '5'))
SKIP = [x for x in os.environ.get('SKIP', '').split(',') if x]
BUDGET = [int(os.environ.get('CRAWL_MAX_STEPS', '120'))]   # A presses per crawl
DEADLINE = time.time() + 60 * float(os.environ.get('CRAWL_MAX_MIN', '60'))


def skipped(path):
    """SKIP entries: an exact row path, or a prefix ending in * (all rows under it)"""
    return any(path == s or (s.endswith('*') and path.startswith(s[:-1])) for s in SKIP)


def winpath(p):
    p = p.replace('\\', '/')
    if len(p) > 2 and p[0] == '/' and p[2] == '/':
        return p[1].upper() + ':' + p[2:]
    return p


def bashpath(p):
    p = p.replace('\\', '/')
    if len(p) > 2 and p[1] == ':':
        return '/' + p[0].lower() + p[2:]
    return p


class Copy:
    def __init__(self, d):
        self.d = winpath(d)
        self.log = open(os.path.join(self.d, 'crawl.txt'), 'a')

    def note(self, *a):
        line = ' '.join(str(x) for x in a)
        self.log.write(time.strftime('%H:%M:%S ') + line + '\n')
        self.log.flush()

    def pad(self, key, wait):
        with open(os.path.join(self.d, 'pad.txt'), 'a') as f:
            f.write(key + '\n')
        time.sleep(wait)

    def shot(self, label='cw'):
        p = os.path.join(self.d, label + '.png')
        for _ in range(6):
            if os.path.exists(p):
                os.remove(p)
            subprocess.run(['bash', T + '/shot.sh', bashpath(self.d), label], capture_output=True)
            if os.path.exists(p):
                try:
                    return Image.open(p).convert('L')
                except OSError:
                    pass
            time.sleep(1)
        return None


def ident(g):
    """title band (full detail: screen names differ there) + a coarse thumbnail (dialogs)"""
    return g.crop((40, 25, 400, 62)).resize((180, 18)).tobytes() + g.resize((40, 30)).tobytes()


def listsig(g):
    """the part of the screen a highlight moves in: everything below the title band"""
    return g.crop((0, 70, 640, 420)).resize((128, 70)).tobytes()


def dist(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def settle(c, label='cw', tries=8):
    """a frame once two looks in a row agree (a screen fading in, a slow A): (frame, noise)"""
    a = c.shot(label)
    if a is None:
        return None, 0
    for _ in range(tries):
        time.sleep(1.5)
        b = c.shot(label)
        if b is None:
            return None, 0
        n = dist(ident(a), ident(b))
        if n < 4:
            return b, n
        a = b
    return a, 4


def crawl(c, depth, maxrows, path):
    base, noise = settle(c, 'c_' + '_'.join(path) + '_base')
    if base is None:
        return 'noframe'
    th = max(3.0, 2.5 * noise)          # same screen within this distance
    bid = ident(base)
    seen = [listsig(base)]
    step = None                          # list change of one down press (row 0 -> 1)
    for r in range(maxrows):
        here = '_'.join(path + [str(r)])
        if r > 0:
            c.pad('down:300', 2.5)
            s, _ = settle(c, 'cw', 3)
            if s is None:
                return 'noframe'
            if not same_screen(ident(s), bid, th):     # the down press left the screen (a slow A?)
                c.note(here, 'down left the screen: stop')
                return 'lost' if not back_to(c, bid, th, here) else 'ok'
            ls = listsig(s)
            near = min(dist(ls, x) for x in seen)
            if step is None:
                step = dist(ls, seen[0])
            if near < max(0.8, 0.35 * step):
                c.note(here, 'list wrapped/ended (%.1f, step %.1f)' % (near, step))
                break
            seen.append(ls)
        if skipped(here):
            c.note(here, 'skipped')
            continue
        if BUDGET[0] <= 0 or time.time() > DEADLINE:
            c.note(here, 'step/time budget used up')
            break
        BUDGET[0] -= 1
        before = c.shot()
        c.pad('a:400', WAIT_A)
        after, _ = settle(c, 'c_' + here, 4)
        if before is None or after is None:
            return 'noframe'
        d = dist(ident(after), ident(before))
        if same_screen(ident(after), ident(before), th) and d <= th:
            c.note(here, 'A: no new screen (%.1f)' % d)
            continue
        c.note(here, 'A: new screen (%.1f)' % d)
        if depth > 1:
            res = crawl(c, depth - 1, maxrows, path + [str(r)])
            if res == 'noframe':
                return res
            # 'lost' inside: B may still bring this level back
        if not back_to(c, bid, th, here):
            c.note(here, 'B did not bring the screen back: lost')
            return 'lost'
        # the list may have put its highlight back on the first row: walk down to row r again
        s = c.shot()
        if s is not None and r > 0 and dist(listsig(s), seen[0]) < dist(listsig(s), seen[-1]):
            for _ in range(r):
                c.pad('down:300', 2)
    return 'ok'


def same_screen(a, b, th):
    """the title band decides (3240 bytes of head first in ident); the thumbnail only has to be
    roughly alike: map previews and backgrounds move"""
    return dist(a[:3240], b[:3240]) <= 3.5 and dist(a[3240:], b[3240:]) <= max(8.0, th * 2)


def back_to(c, bid, th, here=''):
    for k in range(4):
        c.pad('b:400', WAIT_B)
        s, _ = settle(c, 'cw', 3)
        if s is not None and same_screen(ident(s), bid, th):
            return True
        if s is not None and here:
            s.save(os.path.join(c.d, 'c_%s_back%d.png' % (here, k)))
    return False


def main():
    cmd = sys.argv[1]
    if cmd == 'crawl':
        d, depth, maxrows, tag = sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
        c = Copy(d)
        c.note('crawl', tag, 'depth', depth, 'rows', maxrows)
        res = crawl(c, depth, maxrows, [tag])
        c.note('crawl', tag, '->', res)
        print(res)
        sys.exit(0 if res == 'ok' else 3)
    print(__doc__)
    sys.exit(2)


if __name__ == '__main__':
    main()

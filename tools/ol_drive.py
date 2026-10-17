"""Open Live rig driver: reads where a headless copy is from its screen and
presses what gets it to the Xbox Live menu.

  python tools/ol_drive.py state  <outdir> a b c     prints each copy's screen
  python tools/ol_drive.py tolive <outdir> a b c     drives each to the Live menu

<outdir>/<copy> is a folder tools/ol_boot.sh started (pad.txt, out.log).
Screens are told apart by nearest reference (a small grey thumbnail) from
REF (screenshots taken by these rigs, kept outside the repo).
"""
import os
import subprocess
import sys
import time

from PIL import Image

REF = os.environ.get('OL_REF', 'D:/n64work/halo2/reports/openlive2/ref')
TOOLS = '/d/n64work/openhalo2/tools'


def winpath(p):
    if p.startswith('/d/'):
        return 'D:/' + p[3:]
    return p


def bashpath(p):
    p = p.replace('\\', '/')
    if len(p) > 2 and p[1] == ':':
        return '/' + p[0].lower() + p[2:]
    return p


# each screen is matched on a part that does not move (menus have live 3D
# backgrounds): the mean grey difference there, per pixel
BOX = {
    'signin': (180, 135, 460, 345),
    'plist': (150, 135, 490, 345),
    'profile': (0, 30, 640, 60),
    'live': (0, 30, 640, 60),
    'lobby': (0, 30, 640, 60),
}

_refs = None


def refs():
    global _refs
    if _refs is None:
        _refs = {}
        for f in os.listdir(REF):
            k = f[:-4]
            if f.endswith('.png') and k in BOX:
                _refs[k] = Image.open(os.path.join(REF, f)).convert('L').crop(BOX[k])
    return _refs


def shot(d, label='drv'):
    subprocess.run(['bash', TOOLS + '/shot.sh', bashpath(d), label], capture_output=True)
    p = os.path.join(winpath(d), label + '.png')
    return Image.open(p) if os.path.exists(p) else None


def diff(g, ref, box):
    a, b = g.crop(box).get_flattened_data(), ref.crop(box).get_flattened_data()
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


LOGO = (100, 195, 550, 255)


def classify(im):
    """(screen, distance); 'press_start' is the title logo with no menu
    under it (its own text fades in and out), 'other' when nothing fits"""
    g = im.convert('L')
    best, bd = None, None
    for k, r in refs().items():
        a, b = g.crop(BOX[k]).get_flattened_data(), r.get_flattened_data()
        dist = sum(abs(x - y) for x, y in zip(a, b)) / len(a)
        if bd is None or dist < bd:
            best, bd = k, dist
    if bd > 12:
        main = Image.open(os.path.join(REF, 'main.png')).convert('L')
        dl = diff(g, main, LOGO)
        if dl < 14:
            rows = [sum(g.crop((250, y - 6, 400, y + 6)).get_flattened_data()) for y in (371, 399)]
            return ('main' if min(rows) > 50000 else 'press_start'), dl
        return 'other', bd
    return best, bd


def press(d, *keys, wait=3.0):
    for k in keys:
        if ':' not in k:
            k += ':400'
        with open(os.path.join(winpath(d), 'pad.txt'), 'a') as f:
            f.write(k + '\n')
        time.sleep(wait)


def main_row(im):
    """the lit row of the main menu: its highlight glows past the text, on
    both sides (rows at y = 287 + 28 n)"""
    b = im.convert('RGB').split()[2]
    v = [sum(b.crop((205, y - 2, 250, y + 2)).get_flattened_data()) + sum(b.crop((395, y - 2, 440, y + 2)).get_flattened_data())
         for y in (287, 315, 343, 371, 399)]
    return v.index(max(v))


def list_row(im):
    """the lit row of the sign-in list (rows at y 242, 264, 286)"""
    g = im.convert('L')
    v = [sum(g.crop((240, y - 5, 440, y + 5)).get_flattened_data()) for y in (242, 264, 286)]
    return v.index(max(v))


def tolive(d, tries=20):
    others = 0
    for _ in range(tries):
        im = shot(d)
        if im is None:
            time.sleep(3)
            continue
        s, dist = classify(im)
        print('  %s: %s (%d)' % (os.path.basename(d), s, dist), flush=True)
        if s == 'live':
            return True
        others = others + 1 if s == 'other' else 0
        if s == 'press_start':
            press(d, 'start:1500', wait=6)
        elif s == 'signin':        # the first account: the row under "Don't Sign In"
            r = list_row(im)
            if r == 1:
                press(d, 'a:300', wait=15)
            else:
                press(d, 'down:200' if r < 1 else 'up:200', wait=2)
        elif s == 'main':
            r = main_row(im)
            if r == 1:
                press(d, 'a:300', wait=8)
            else:
                press(d, 'down:200' if r < 1 else 'up:200', wait=3)
        elif s == 'plist':      # the highlighted profile is the copy's last one,
            g = im.convert('L')  # unless it is "Create New Profile" (the top row)
            v = [sum(g.crop((215, y - 5, 465, y + 5)).get_flattened_data()) for y in (242, 263, 284, 305)]
            if v.index(max(v)) == 0:
                press(d, 'down:200', wait=2)
            else:
                press(d, 'a:300', wait=8)
        elif s == 'profile':
            press(d, 'a:300', wait=8)
        elif others < 3:   # a fade or a progress dialog: wait
            time.sleep(4)
        else:              # a lobby or anything else: back out
            press(d, 'b:300', wait=4)
    return False


def lit_row(im, ys, x0, x1):
    g = im.convert('L')
    v = [sum(g.crop((x0, y - 4, x1, y + 4)).get_flattened_data()) for y in ys]
    return v.index(max(v))


LIVE_ROWS = (127, 149, 171, 193)            # Quickmatch, Optimatch, Create Party, Content Download
PLAYLIST_ROWS = (80, 102, 124, 146, 168, 190)


def menu_to(d, ys, x0, x1, target, tries=10):
    """moves a list's highlight to row target, reading the screen"""
    for _ in range(tries):
        r = lit_row(shot(d), ys, x0, x1)
        if r == target:
            return True
        press(d, 'down:200' if r < target else 'up:200', wait=1.5)
    return False


def optimatch(d, playlist):
    """from the Live menu: Optimatch, the playlist (row), Start Game"""
    if not menu_to(d, LIVE_ROWS, 200, 290, 1):
        return 'Live menu row not reached'
    press(d, 'a:300', wait=6)
    if not menu_to(d, PLAYLIST_ROWS, 200, 285, playlist):
        return 'playlist row not reached'
    press(d, 'a:300', wait=7)
    press(d, 'a:300', wait=4)       # the lobby opens on Start Game
    return 'matchmaking'


def main():
    cmd, out, copies = sys.argv[1], sys.argv[2], sys.argv[3:]
    for c in copies:
        d = os.path.join(winpath(out), c)
        if cmd == 'state':
            im = shot(d)
            print(c, classify(im) if im else 'no frame')
        elif cmd.startswith('optimatch'):      # optimatch<row>, e.g. optimatch1 = Team Slayer
            print(c, optimatch(d, int(cmd[9:] or 1)))
        elif cmd == 'tolive':
            print(c, 'on the Live menu' if tolive(d) else 'NOT on the Live menu')


if __name__ == '__main__':
    main()

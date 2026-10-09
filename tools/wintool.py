"""wintool.py: drive and inspect the game window from outside the process.

  wintool.py info                     window rect, client size, style, foreground?
  wintool.py cap <out.png>            the client area as the desktop compositor
                                      has it (PrintWindow, full content)
  wintool.py focus                    bring the window to the foreground
  wintool.py key <name> [hold ms]     press and release a key in the focused
                                      window (SendInput, scan codes); names:
                                      a-z 0-9 enter esc space tab back up down
                                      left right ctrl shift f1 f2 f11 altenter
  wintool.py click                    left click in the middle of the client area
  wintool.py mouse <dx> <dy> <ms>     move the mouse by (dx, dy) counts spread
                                      over ms milliseconds (relative moves)
  wintool.py lbutton <hold ms>        hold the left button
  wintool.py size <w> <h>             resize the window's outer rectangle
  wintool.py close                    post WM_CLOSE

The window is found by its title (default "Halo 2"; WINTITLE overrides).
"""
import ctypes, os, sys, time
from ctypes import wintypes as W

u32 = ctypes.WinDLL('user32', use_last_error=True)
g32 = ctypes.WinDLL('gdi32', use_last_error=True)
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
try:
    u32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass
u32.FindWindowW.restype = W.HWND
u32.GetDC.restype = W.HDC
u32.GetDC.argtypes = [W.HWND]
u32.GetForegroundWindow.restype = W.HWND
g32.CreateCompatibleDC.restype = W.HDC
g32.CreateCompatibleDC.argtypes = [W.HDC]
g32.CreateCompatibleBitmap.restype = W.HBITMAP
g32.CreateCompatibleBitmap.argtypes = [W.HDC, ctypes.c_int, ctypes.c_int]
g32.SelectObject.argtypes = [W.HDC, W.HGDIOBJ]
g32.SelectObject.restype = W.HGDIOBJ
g32.GetDIBits.argtypes = [W.HDC, W.HBITMAP, ctypes.c_uint, ctypes.c_uint,
                          ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint]
g32.DeleteObject.argtypes = [W.HGDIOBJ]
g32.DeleteDC.argtypes = [W.HDC]
u32.PrintWindow.argtypes = [W.HWND, W.HDC, ctypes.c_uint]
u32.ReleaseDC.argtypes = [W.HWND, W.HDC]
u32.GetWindowLongW.argtypes = [W.HWND, ctypes.c_int]
u32.GetWindowLongW.restype = ctypes.c_long

TITLE = os.environ.get('WINTITLE', 'Halo 2')


def hwnd():
    h = u32.FindWindowW('xboxrecomp_game_window', None) or u32.FindWindowW(None, TITLE)
    if not h:
        sys.exit('no window')
    return h


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [('dx', W.LONG), ('dy', W.LONG), ('mouseData', W.DWORD),
                ('dwFlags', W.DWORD), ('time', W.DWORD),
                ('dwExtraInfo', ctypes.c_void_p)]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [('wVk', W.WORD), ('wScan', W.WORD), ('dwFlags', W.DWORD),
                ('time', W.DWORD), ('dwExtraInfo', ctypes.c_void_p)]


class _U(ctypes.Union):
    _fields_ = [('mi', MOUSEINPUT), ('ki', KEYBDINPUT)]


class INPUT(ctypes.Structure):
    _fields_ = [('type', W.DWORD), ('u', _U)]


def send(inp):
    n = u32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
    if n != 1:
        print('SendInput failed', ctypes.get_last_error())


def key_event(vk, up, ext=False):
    i = INPUT(); i.type = 1
    sc = u32.MapVirtualKeyW(vk, 0)
    i.u.ki = KEYBDINPUT(vk, sc, (2 if up else 0) | (1 if ext else 0), 0, None)
    send(i)


def mouse_event(flags, dx=0, dy=0):
    i = INPUT(); i.type = 0
    i.u.mi = MOUSEINPUT(dx, dy, 0, flags, 0, None)
    send(i)


VK = {'enter': 0x0D, 'esc': 0x1B, 'space': 0x20, 'tab': 0x09, 'back': 0x08,
      'up': 0x26, 'down': 0x28, 'left': 0x25, 'right': 0x27, 'ctrl': 0x11,
      'shift': 0x10, 'f1': 0x70, 'f2': 0x71, 'f11': 0x7A}
EXT = {'up', 'down', 'left', 'right'}


def focus():
    h = hwnd()
    # A background process may not take the foreground; a tap of Alt lifts
    # that restriction for the next call.
    key_event(0x12, False); key_event(0x12, True)
    u32.ShowWindow(h, 9)
    u32.SetForegroundWindow(h)
    time.sleep(0.3)
    return u32.GetForegroundWindow() == h


def client(h):
    r = W.RECT(); u32.GetClientRect(h, ctypes.byref(r))
    return r.right, r.bottom


def main():
    cmd = sys.argv[1]
    h = hwnd()
    if cmd == 'info':
        r = W.RECT(); u32.GetWindowRect(h, ctypes.byref(r))
        cw, ch = client(h)
        buf = ctypes.create_unicode_buffer(128); u32.GetWindowTextW(h, buf, 128)
        print('title "%s" rect %d,%d %dx%d client %dx%d style %08X foreground %d' % (
            buf.value, r.left, r.top, r.right - r.left, r.bottom - r.top, cw, ch,
            u32.GetWindowLongW(h, -16) & 0xFFFFFFFF, u32.GetForegroundWindow() == h))
    elif cmd == 'cap':
        from PIL import Image
        cw, ch = client(h)
        dc = u32.GetDC(h); mdc = g32.CreateCompatibleDC(dc)
        bmp = g32.CreateCompatibleBitmap(dc, cw, ch)
        g32.SelectObject(mdc, bmp)
        ok = u32.PrintWindow(h, mdc, 3)        # client only | full content
        bi = (ctypes.c_uint32 * 10)(40, cw, (-ch) & 0xFFFFFFFF, 1 | (32 << 16), 0, 0, 0, 0, 0, 0)
        px = ctypes.create_string_buffer(cw * ch * 4)
        g32.GetDIBits(mdc, bmp, 0, ch, px, bi, 0)
        g32.DeleteObject(bmp); g32.DeleteDC(mdc); u32.ReleaseDC(h, dc)
        im = Image.frombuffer('RGBA', (cw, ch), px.raw, 'raw', 'BGRA', 0, 1).convert('RGB')
        im.save(sys.argv[2])
        ex = im.getextrema()
        print('PrintWindow %d, %dx%d, extrema %s -> %s' % (ok, cw, ch, ex, sys.argv[2]))
    elif cmd == 'focus':
        print('foreground', focus())
    elif cmd == 'key':
        name = sys.argv[2].lower(); hold = int(sys.argv[3]) if len(sys.argv) > 3 else 120
        if u32.GetForegroundWindow() != h and not focus():
            print('warning: window is not in the foreground')
        if name == 'altenter':
            key_event(0x12, False); key_event(0x0D, False); time.sleep(hold / 1000)
            key_event(0x0D, True); key_event(0x12, True)
        else:
            vk = VK.get(name) or ord(name.upper())
            key_event(vk, False, name in EXT); time.sleep(hold / 1000)
            key_event(vk, True, name in EXT)
        print('key', name, hold)
    elif cmd == 'click':
        if u32.GetForegroundWindow() != h:
            focus()
        cw, ch = client(h)
        p = W.POINT(cw // 2, ch // 2); u32.ClientToScreen(h, ctypes.byref(p))
        u32.SetCursorPos(p.x, p.y); time.sleep(0.05)
        mouse_event(2); time.sleep(0.08); mouse_event(4)
        print('click at', p.x, p.y)
    elif cmd == 'lbutton':
        mouse_event(2); time.sleep(int(sys.argv[2]) / 1000); mouse_event(4)
        print('lbutton', sys.argv[2])
    elif cmd == 'mouse':
        dx, dy, ms = int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
        steps = max(1, ms // 8)
        for k in range(steps):
            mouse_event(1, (dx * (k + 1)) // steps - (dx * k) // steps,
                        (dy * (k + 1)) // steps - (dy * k) // steps)
            time.sleep(0.008)
        print('mouse', dx, dy, ms)
    elif cmd == 'size':
        u32.SetWindowPos(h, None, 0, 0, int(sys.argv[2]), int(sys.argv[3]), 0x0002 | 0x0004)
        time.sleep(0.3)
        print('client now %dx%d' % client(h))
    elif cmd == 'close':
        u32.PostMessageW(h, 0x0010, 0, 0)
        print('WM_CLOSE posted')


main()

"""ol_wdrive.py <dir> --url URL [--profile DIR] [--port N] [--max SECS] [--webgl]

One headless Chrome page driven while it runs, for Open Live rigs (one
browser copy beside native copies). Follows <dir>/cmd.txt, one command per
appended line (a leading tab name "a" is accepted and ignored, so the lines
of halo2-cleanroom/ports/wasm/cdp_pair.py work too):

    key Space 300        hold a key (KeyboardEvent.code) for 300 ms
    shot label           the game picture -> label_a.png
    eval <javascript>    evaluate; the value goes to console_a.txt
    quit                 close the browser

The picture comes from the page's own copy of the next frame (dev.html:
Module.snapWanted / Module.snapURL), since a canvas that receives
ImageBitmaps reads back empty; the canvas itself is the fallback. Console
output goes to console_a.txt. Nothing here times out on a quiet page.
"""
import argparse
import base64
import json
import os
import subprocess
import sys
import threading
import time
import urllib.request

import websocket

sys.path.insert(0, 'D:/n64work/halo2-cleanroom/ports/wasm')
from cdp_shot import BROWSERS, key_info  # noqa: E402


class Tab:
    def __init__(self, ws_url, out):
        self.ws = websocket.create_connection(ws_url, timeout=60, suppress_origin=True)
        self.ws.settimeout(None)
        self.n = 0
        self.pending = {}
        self.lock = threading.Lock()
        self.log = open(os.path.join(out, 'console_a.txt'), 'w', encoding='utf-8', buffering=1)
        self.alive = True
        threading.Thread(target=self.reader, daemon=True).start()
        for m in ('Runtime.enable', 'Log.enable', 'Page.enable'):
            self.send(m)

    def send(self, method, cb=None, **params):
        with self.lock:
            self.n += 1
            if cb:
                self.pending[self.n] = cb
            self.ws.send(json.dumps({'id': self.n, 'method': method, 'params': params}))

    def reader(self):
        while True:
            try:
                m = json.loads(self.ws.recv())
            except Exception as e:  # noqa: BLE001
                self.log.write(f'[driver] reader stopped: {e}\n')
                self.alive = False
                return
            try:
                meth = m.get('method')
                if meth == 'Runtime.consoleAPICalled':
                    text = ' '.join(str(x.get('value', x.get('description', ''))) for x in m['params']['args'])
                    self.log.write(f"[{m['params']['type']}] {text[:2000]}\n")
                elif meth == 'Runtime.exceptionThrown':
                    ed = m['params']['exceptionDetails']
                    self.log.write('EXCEPTION: ' + (ed.get('exception', {}).get('description') or ed.get('text', ''))[:2000] + '\n')
                elif meth == 'Log.entryAdded':
                    e = m['params']['entry']
                    self.log.write(f"[log.{e['level']}] {e['text'][:2000]}\n")
                elif 'id' in m:
                    cb = self.pending.pop(m['id'], None)
                    if cb:
                        cb(m)
            except Exception as e:  # noqa: BLE001
                self.log.write(f'[driver] {e}\n')

    def key(self, code, ms):
        key, vk = key_info(code)
        self.send('Input.dispatchKeyEvent', type='keyDown', code=code, key=key, windowsVirtualKeyCode=vk)
        threading.Timer(ms / 1000.0, lambda: self.send(
            'Input.dispatchKeyEvent', type='keyUp', code=code, key=key, windowsVirtualKeyCode=vk)).start()

    def evaluate(self, expr, timeout=10.0):
        box, ev = {}, threading.Event()

        def done(m):
            box['v'] = m.get('result', {}).get('result', {}).get('value')
            ev.set()
        self.send('Runtime.evaluate', cb=done, expression=expr, returnByValue=True, awaitPromise=True)
        ev.wait(timeout)
        return box.get('v')

    def shot(self, path):
        v = self.evaluate('''new Promise(res => {
            if (!Module.nv2aView) { res((document.getElementById('canvas')).toDataURL('image/png')); return; }
            Module.snapURL = null; Module.snapWanted = true;
            let n = 0; const t = setInterval(() => {
              if (Module.snapURL || ++n > 40) { clearInterval(t);
                res(Module.snapURL || document.getElementById('glcanvas').toDataURL('image/png')); } }, 50);
          })''', timeout=15)
        if isinstance(v, str) and v.startswith('data:image/png;base64,'):
            with open(path, 'wb') as f:
                f.write(base64.b64decode(v.split(',', 1)[1]))
        else:
            self.log.write(f'[driver] no picture for {path}\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--url', required=True)
    ap.add_argument('--profile', required=True)
    ap.add_argument('--port', type=int, default=9478)
    ap.add_argument('--max', type=float, default=14400.0)
    ap.add_argument('--webgl', action='store_true')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    cmd_path = os.path.join(a.out, 'cmd.txt')
    if not os.path.exists(cmd_path):
        open(cmd_path, 'w').close()
    exe = next(b for b in BROWSERS if os.path.exists(b))
    args = [exe, '--headless=new', f'--user-data-dir={a.profile}', f'--remote-debugging-port={a.port}',
            '--remote-allow-origins=*', '--no-first-run', '--autoplay-policy=no-user-gesture-required',
            '--window-size=700,560', '--ignore-gpu-blocklist', '--enable-gpu-rasterization',
            '--js-flags=--max-old-space-size=8192', '--mute-audio',
            '--disable-background-timer-throttling', '--disable-renderer-backgrounding',
            '--disable-backgrounding-occluded-windows', '--enable-logging=stderr', '--log-level=1']
    if a.webgl:
        args += ['--enable-unsafe-swiftshader', '--use-angle=swiftshader']
    blog = open(os.path.join(a.out, 'browser.log'), 'wb')
    p = subprocess.Popen(args + ['about:blank'], stdout=subprocess.DEVNULL, stderr=blog)
    with open(os.path.join(a.out, 'browser.pid'), 'w') as f:
        f.write(str(p.pid))
    tab = None
    try:
        for _ in range(150):
            try:
                listing = json.load(urllib.request.urlopen(f'http://127.0.0.1:{a.port}/json'))
                break
            except OSError:
                time.sleep(0.2)
        first = next(t for t in listing if t.get('type') == 'page')
        tab = Tab(first['webSocketDebuggerUrl'], a.out)
        tab.send('Page.navigate', url=a.url)
        print('page open', flush=True)
        pos = os.path.getsize(cmd_path)
        t0 = time.time()
        # the first process may hand over to another and exit: follow the page
        while time.time() - t0 < a.max and tab.alive:
            time.sleep(0.2)
            with open(cmd_path, 'r', encoding='utf-8') as f:
                f.seek(pos)
                chunk = f.read()
            if not chunk.endswith('\n'):
                continue
            pos += len(chunk.encode('utf-8'))
            for line in chunk.splitlines():
                w = line.split()
                if w and w[0] in ('a', 'b'):
                    if w[0] == 'b':
                        continue
                    w = w[1:]
                    line = line.split(None, 1)[1] if len(line.split(None, 1)) > 1 else ''
                if not w:
                    continue
                if w[0] == 'quit':
                    return
                if w[0] == 'shot' and len(w) > 1:
                    tab.shot(os.path.join(a.out, f'{w[1]}_a.png'))
                elif w[0] == 'key' and len(w) > 1:
                    tab.key(w[1], float(w[2]) if len(w) > 2 else 150.0)
                elif w[0] == 'eval':
                    expr = line.split(None, 1)[1]
                    v = tab.evaluate(expr)
                    tab.log.write(f'[eval] {expr} -> {json.dumps(v)}\n')
    finally:
        try:
            if tab:
                tab.send('Browser.close')
            p.wait(timeout=25)
        except Exception:  # noqa: BLE001
            pass
        p.kill()
        subprocess.run(['taskkill', '/F', '/T', '/PID', str(p.pid)], capture_output=True)
        print('browser closed', flush=True)


if __name__ == '__main__':
    main()

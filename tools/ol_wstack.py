"""ol_wstack.py [port]: where the page's main thread is, when it stops
answering (the Open Live web rig): attaches to the headless browser on the
debugging port, pauses the page and prints its JavaScript/wasm stack."""
import json
import sys
import urllib.request

import websocket

port = int(sys.argv[1]) if len(sys.argv) > 1 else 9478
tabs = json.load(urllib.request.urlopen(f'http://127.0.0.1:{port}/json'))
page = next(t for t in tabs if t['type'] == 'page')
ws = websocket.create_connection(page['webSocketDebuggerUrl'], timeout=20, suppress_origin=True)
n = 0


def send(method, **params):
    global n
    n += 1
    ws.send(json.dumps({'id': n, 'method': method, 'params': params}))
    return n


send('Debugger.enable')
send('Debugger.pause')
try:
    while True:
        m = json.loads(ws.recv())
        if m.get('method') == 'Debugger.paused':
            for f in m['params']['callFrames'][:40]:
                loc = f['location']
                print(f"{f.get('functionName') or '?'}  {f.get('url', '')[-40:]}:{loc['lineNumber']}:{loc.get('columnNumber', 0)}")
            send('Debugger.resume')
            break
except Exception as e:  # noqa: BLE001
    print('no pause:', e)

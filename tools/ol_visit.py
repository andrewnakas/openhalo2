"""ol_visit.py <room> <type> [hex payload]: visits an Open Live relay room the
way net/olive.c does (OLIVE_ONE: the room's oldest member) with one message
(type byte, a 25-byte head from a made-up sender, then the payload) and
prints the welcome and whatever comes back within 4 s. A probe for rigs.

  python tools/ol_visit.py h2u-002et8jz03jqr O         (presence: anyone may ask)
"""
import json
import struct
import sys
import time

import websocket

RELAY = 'wss://skatemods.com/rooms'


def main():
    room, typ = sys.argv[1], sys.argv[2]
    payload = bytes.fromhex(sys.argv[3]) if len(sys.argv) > 3 else b''
    ws = websocket.create_connection(f'{RELAY}/room/{room}', timeout=6)
    welcome = json.loads(ws.recv())
    print('welcome:', welcome)
    host = welcome.get('host')
    if not host or host == welcome.get('self'):
        print('nobody holds the room')
        return
    head = typ.encode() + struct.pack('<Q', 0x0009FFFFFFFF0001) + b'Probe'.ljust(16, b'\0')
    ws.send_binary(struct.pack('<I', host) + head + payload)
    end = time.time() + 4
    while time.time() < end:
        ws.settimeout(max(0.1, end - time.time()))
        try:
            m = ws.recv()
        except Exception:
            break
        if isinstance(m, bytes):
            print('reply from', struct.unpack_from('<I', m)[0], ':', m[4:5], m[5:80].hex())
        else:
            print('notice:', m)
    ws.close()


if __name__ == '__main__':
    main()

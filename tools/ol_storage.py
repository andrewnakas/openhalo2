"""Open Live title storage: writes the files the title downloads after
signing in (docs/OPEN_LIVE_DESIGN.md 1.4), with our own content.

Formats (all read from the title's code; see port/xr/src/openlive_api.c):

  every file      16-byte header {crc, version, size, 0}; crc = CRC-32
                  (reflected, polynomial 0xEDB88320, start 0xFFFFFFFF, no
                  final xor: crc.cpp 0x163BA0) of bytes 4..size; version per
                  file (request definitions, tools/xo_storage_defs.py)
  compressed body big-endian u32 uncompressed size, then a zlib stream
                  (unknown_199560.cpp 0x199740)

  message_of_the_day.dat  version 1, body compressed, 0x25224 bytes
                  uncompressed (0x8C790): 9 languages x 0x200 bytes (the
                  message, UTF-16, 256 chars), then 9 x 0x4004 (a list:
                  u32 count <= 32, 32 x 256 UTF-16 chars) (0x8C900, 0x8CA70)
  multiplayer_hopper.dat  version 4, body compressed, 0x162CC0 bytes
                  (0x194260): 16 playlists x 0x614 bytes, then 16 map lists
                  x (0x48 + 100 x 0x37C) (unknown_1932c0.cpp):
                    playlist +0 type (1-5), +8 id, +0xC names[9][16] UTF-8,
                    +0x9C descriptions[9][128] UTF-8, player counts and team
                    settings at +0x5A4..0x614
                    map list +0x44 count, entries of 0x37C: +4 map id, +0x248
                    weight, +0x24C game settings (0x130; +0x44 game type)

The playlists' rules (types, team sizes, game settings, maps) come from the
title's own built-in fallback, which the title writes to its cache drive when
no file is served (save/CacheY/multiplayer_hopper.dat after one signed-in
boot): the title's code, not server data. This tool takes that file as the
base, keeps the rules, and writes our names, descriptions and selection.

usage: python tools/ol_storage.py [--base save/CacheY/multiplayer_hopper.dat]
                                  [--out <run folder>/openlive/storage]
"""
import argparse
import os
import struct
import zlib

TITLE_FOLDER = "9809"            # 0x2651, the folder the title asks for (0xB4120)
LANGS = 9

PLAYLISTS = [
    # (name <= 15 chars, description <= 127 chars, base playlist index,
    #  playlist id, keep only these game types (None = all))
    ("Free For All", "Every player for themselves, up to 8 players. Slayer and its variants.", 0, 1, None),
    ("Team Slayer", "Two teams of up to 4. Slayer only.", 1, 0x20, {2}),
    ("Team Skirmish", "Two teams of up to 4 across every team game type.", 1, 2, None),
    ("Big Team Battle", "Two teams of up to 8 on the large maps. Join alone or as a party.", 3, 4, None),
    ("Duel", "One on one against a single opponent.", 2, 3, None),
    ("Training Ground", "Unranked team games for practice. Guests welcome.", 6, 5, None),
]

MOTD = ("Welcome to Open Live: peer to peer Halo 2 matchmaking, friends and parties, "
        "hosted by the players themselves. No servers, no subscription.")
MOTD_LIST = ["Open Live", "Peer to peer", "Have fun"]

PL_SIZE = 0x614
BLK_SIZE = 0x48 + 100 * 0x37C
MAP_SIZE = 0x37C
HOPPER_SIZE = 16 * PL_SIZE + 16 * BLK_SIZE
MOTD_SIZE = 9 * 0x200 + 9 * 0x4004


def crc_title(b):
    return zlib.crc32(b) ^ 0xFFFFFFFF


def package(version, body, compress):
    if compress:
        body = struct.pack('>I', len(body)) + zlib.compress(body, 9)
    size = 16 + len(body)
    rest = struct.pack('<III', version, size, 0) + body
    return struct.pack('<I', crc_title(rest)) + rest


def put_utf8(buf, off, n, s):
    b = s.encode('utf-8')[:n - 1]
    buf[off:off + n] = b + b'\0' * (n - len(b))


def hopper(base):
    assert len(base) == HOPPER_SIZE, f"base is {len(base):#x} bytes, expected {HOPPER_SIZE:#x}"
    out = bytearray(HOPPER_SIZE)
    for i, (name, desc, src, pid, keep) in enumerate(PLAYLISTS):
        pl = bytearray(base[src * PL_SIZE:(src + 1) * PL_SIZE])
        struct.pack_into('<I', pl, 8, pid)
        for lang in range(LANGS):
            put_utf8(pl, 0xC + 16 * lang, 16, name)
            put_utf8(pl, 0x9C + 128 * lang, 128, desc)
        out[i * PL_SIZE:(i + 1) * PL_SIZE] = pl
        blk = bytearray(base[16 * PL_SIZE + src * BLK_SIZE:16 * PL_SIZE + (src + 1) * BLK_SIZE])
        if keep is not None:
            n = struct.unpack_from('<i', blk, 0x44)[0]
            maps = [blk[0x48 + j * MAP_SIZE:0x48 + (j + 1) * MAP_SIZE] for j in range(n)]
            maps = [m for m in maps if struct.unpack_from('<i', m, 0x24C + 0x44)[0] in keep]
            blk[0x48:] = b'\0' * (BLK_SIZE - 0x48)
            for j, m in enumerate(maps):
                blk[0x48 + j * MAP_SIZE:0x48 + (j + 1) * MAP_SIZE] = m
            struct.pack_into('<i', blk, 0x44, len(maps))
        o = 16 * PL_SIZE + i * BLK_SIZE
        out[o:o + BLK_SIZE] = blk
    return bytes(out)


def motd():
    out = bytearray(MOTD_SIZE)
    msg = MOTD.encode('utf-16-le')[:510]
    lst = bytearray(0x4004)
    struct.pack_into('<I', lst, 0, len(MOTD_LIST))
    for j, s in enumerate(MOTD_LIST):
        b = s.encode('utf-16-le')[:510]
        lst[4 + j * 0x200:4 + j * 0x200 + len(b)] = b
    for lang in range(LANGS):
        out[lang * 0x200:lang * 0x200 + len(msg)] = msg
        out[0x1200 + lang * 0x4004:0x1200 + (lang + 1) * 0x4004] = lst
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', default='D:/n64work/halo2/build/xr/save/CacheY/multiplayer_hopper.dat')
    ap.add_argument('--out', action='append',
                    default=None, help='storage folder(s); default build/xr and build/xr_b')
    a = ap.parse_args()
    outs = a.out or ['D:/n64work/halo2/build/xr/openlive/storage', 'D:/n64work/halo2/build/xr_b/openlive/storage']
    files = {
        'message_of_the_day.dat': package(1, motd(), True),
    }
    # Once our file is served the title caches it in place of its fallback,
    # so the first base seen is kept aside and used from then on.
    keep = 'D:/n64work/halo2/build/openlive_base_hopper.dat'
    if not os.path.exists(keep) and os.path.exists(a.base):
        b = open(a.base, 'rb').read()
        if len(b) == HOPPER_SIZE and struct.unpack_from('<I', b, 8)[0] == 1 \
                and not b[0xC:0x1C].startswith(PLAYLISTS[0][0].encode()):
            open(keep, 'wb').write(b)
    if os.path.exists(keep):
        files['multiplayer_hopper.dat'] = package(4, hopper(open(keep, 'rb').read()), True)
    else:
        print(f"no base playlist file at {a.base}: sign in once without one, the title writes it")
    for o in outs:
        d = os.path.join(o, 'title', TITLE_FOLDER)
        os.makedirs(d, exist_ok=True)
        for n, b in files.items():
            open(os.path.join(d, n), 'wb').write(b)
    print('wrote', ', '.join(f'{n} ({len(b)} bytes)' for n, b in files.items()), 'to', ', '.join(outs))


if __name__ == '__main__':
    main()

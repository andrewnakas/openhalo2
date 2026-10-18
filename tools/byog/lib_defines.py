"""Print the public symbols a COFF static library (.lib) defines, one per line.

Reads the archive's first linker member (the symbol index the linker uses), so
it needs no Microsoft tools. make_release.sh uses it to prove the runtime kit
defines no lifted function.

usage: python lib_defines.py <file.lib>
"""
import struct
import sys


def defines(path):
    data = open(path, "rb").read()
    if data[:8] != b"!<arch>\n":
        raise SystemExit(f"{path}: not an archive")
    hdr = data[8:68]
    if hdr[:16].rstrip() != b"/":
        raise SystemExit(f"{path}: no linker member")
    size = int(hdr[48:58])
    body = data[68:68 + size]
    n = struct.unpack(">I", body[:4])[0]
    names = body[4 + 4 * n:].split(b"\0")[:n]
    return [s.decode("latin-1") for s in names]


if __name__ == "__main__":
    for s in defines(sys.argv[1]):
        print(s)

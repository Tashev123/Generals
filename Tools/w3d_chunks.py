"""w3d_chunks.py <model>...: raw chunk tree of a W3D file (ids, sizes, and any names inside)."""
import re
import struct
import sys

sys.path.insert(0, r"C:\Users\PC\Documents\Unreal Projects\Generals1\Tools")
from gen_vfs import GeneralsVFS  # noqa: E402


def walk(data, lo, hi, depth):
    while lo + 8 <= hi:
        cid, size = struct.unpack_from("<II", data, lo)
        sub = bool(size & 0x80000000)
        size &= 0x7FFFFFFF
        body = data[lo + 8: lo + 8 + size]
        names = "" if sub else " ".join(s.decode("latin-1") for s in re.findall(rb"[A-Za-z0-9_.]{4,}", body)[:6])
        print(f"{'  ' * depth}0x{cid:04X} size={size}{' {' if sub else ''} {names}")
        if sub:
            walk(data, lo + 8, lo + 8 + size, depth + 1)
        lo += 8 + size


vfs = GeneralsVFS()
for name in sys.argv[1:]:
    path = name if "\\" in name else f"Art\\W3D\\{name}.w3d"
    data = vfs.read(path)
    print(f"===== {path} ({len(data)} bytes)")
    walk(data, 0, len(data), 0)

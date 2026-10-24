"""ani_dump.py [out_dir]: what the game's .ani cursors are made of; with out_dir, a contact sheet of every cursor's frames."""
import glob
import io
import os
import struct
import sys

DIR = r"D:\Games\EA Games\Command & Conquer Generals Zero Hour\Data\Cursors"


def chunks(data, lo, hi):
    while lo + 8 <= hi:
        cid = data[lo:lo + 4]
        size = struct.unpack_from("<I", data, lo + 4)[0]
        yield cid, lo + 8, size
        lo += 8 + size + (size & 1)


def parse(path):
    data = open(path, "rb").read()
    assert data[:4] == b"RIFF" and data[8:12] == b"ACON", path
    info = {"frames": [], "rate": None, "seq": None, "anih": None}
    for cid, off, size in chunks(data, 12, len(data)):
        if cid == b"anih":
            info["anih"] = struct.unpack_from("<9I", data, off)
        elif cid == b"rate":
            info["rate"] = list(struct.unpack_from(f"<{size // 4}I", data, off))
        elif cid == b"seq ":
            info["seq"] = list(struct.unpack_from(f"<{size // 4}I", data, off))
        elif cid == b"LIST" and data[off:off + 4] == b"fram":
            for c2, o2, s2 in chunks(data, off + 4, off + size):
                if c2 == b"icon":
                    info["frames"].append(data[o2:o2 + s2])
    return info


def describe(frame):
    reserved, kind, count = struct.unpack_from("<3H", frame, 0)
    w, h, colors, _, hot_x, hot_y, size, offset = struct.unpack_from("<4B2H2I", frame, 6)
    bi_size, bi_w, bi_h, planes, bpp, compression = struct.unpack_from("<IiiHHI", frame, offset)
    return f"type={kind} n={count} {w or 256}x{h or 256} hot=({hot_x},{hot_y}) bpp={bpp} comp={compression} dib={bi_w}x{bi_h}"


out_dir = sys.argv[1] if len(sys.argv) > 1 else None
seen = {}
for path in sorted(glob.glob(os.path.join(DIR, "*.ani")), key=str.lower):
    info = parse(path)
    a = info["anih"]
    d = describe(info["frames"][0]) if info["frames"] else "NO FRAMES"
    kinds = {describe(f) for f in info["frames"]}
    print(f"{os.path.basename(path):22s} frames={len(info['frames']):2d} steps={a[2]:2d} rate={a[7]} jiffies flags={a[8]} seq={'yes' if info['seq'] else 'no'} rates={'yes' if info['rate'] else 'no'} | {d}{' (frames differ)' if len(kinds) > 1 else ''}")
    seen[os.path.basename(path)] = info

if out_dir:
    from PIL import Image
    os.makedirs(out_dir, exist_ok=True)
    for name, info in seen.items():
        imgs = []
        for f in info["frames"]:
            try:
                im = Image.open(io.BytesIO(f))
                imgs.append(im.convert("RGBA"))
            except Exception as e:  # noqa: BLE001
                print(name, "frame not readable:", e)
        if not imgs:
            continue
        w, h = imgs[0].size
        sheet = Image.new("RGBA", (w * len(imgs) * 3, h * 3), (90, 110, 90, 255))
        for i, im in enumerate(imgs):
            big = im.resize((w * 3, h * 3), Image.NEAREST)
            sheet.paste(big, (i * w * 3, 0), big)
        sheet.save(os.path.join(out_dir, os.path.splitext(name)[0] + ".png"))

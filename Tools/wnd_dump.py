"""Prints the window tree of a Generals .wnd file (GameWindowManagerScript.cpp's format): name, type,
rectangle (in the file's creation resolution), status and the first enabled / disabled / hilite images.

    python wnd_dump.py <file.wnd | Window\\Name.wnd> [--all]

A path that doesn't exist on disk is read from the game's archives (gen_vfs). --all prints every image
of the draw data, not just the first.
"""
import os
import re
import sys


def read_text(path):
    if os.path.exists(path):
        return open(path, "rb").read().decode("latin-1")
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from gen_vfs import GeneralsVFS
    vfs = GeneralsVFS()
    data = vfs.read(path)
    if data is None:
        raise SystemExit(f"not found: {path}")
    return data.decode("latin-1")


def statements(text):
    # Statements end with ';' - except the bare keywords WINDOW, CHILD, END, ENDALLCHILDREN.
    text = text.replace("\r", "")
    out = []
    buf = ""
    for line in text.split("\n"):
        s = line.strip()
        if not s:
            continue
        if not buf and s in ("WINDOW", "CHILD", "END", "ENDALLCHILDREN", "STARTLAYOUTBLOCK", "ENDLAYOUTBLOCK"):
            out.append(s)
            continue
        buf += " " + s
        if buf.rstrip().endswith(";"):
            out.append(buf.strip()[:-1])
            buf = ""
    return out


def parse(text):
    root = {"fields": {}, "children": []}
    stack = [root]
    cur = None
    in_layout = False
    for st in statements(text):
        if st == "STARTLAYOUTBLOCK":
            in_layout = True
            continue
        if st == "ENDLAYOUTBLOCK":
            in_layout = False
            continue
        if in_layout:
            continue
        if st == "WINDOW":
            cur = {"fields": {}, "children": []}
            stack[-1]["children"].append(cur)
            stack.append(cur)
        elif st == "CHILD":
            pass
        elif st == "END":
            stack.pop()
            cur = stack[-1]
        elif st == "ENDALLCHILDREN":
            pass
        else:
            k, _, v = st.partition("=")
            stack[-1]["fields"][k.strip()] = v.strip()
    return root


def images(v, all_):
    imgs = re.findall(r"IMAGE:\s*([^,]+)", v or "")
    imgs = [i.strip() for i in imgs]
    if not all_:
        return imgs[:1]
    return imgs


def dump(w, depth, all_):
    f = w["fields"]
    name = f.get("NAME", "?").strip('"')
    rect = re.findall(r"-?\d+", f.get("SCREENRECT", ""))
    r = f"({rect[0]},{rect[1]})-({rect[2]},{rect[3]})" if len(rect) >= 4 else ""
    en = images(f.get("ENABLEDDRAWDATA"), all_)
    di = images(f.get("DISABLEDDRAWDATA"), all_)
    hi = images(f.get("HILITEDRAWDATA"), all_)
    imgs = []
    for tag, lst in (("E", en), ("D", di), ("H", hi)):
        lst = [i for i in lst if i != "NoImage"]
        if lst:
            imgs.append(f"{tag}:{','.join(lst)}")
    extra = ""
    if "TEXT" in f:
        extra += f" text={f['TEXT']}"
    if "TOOLTIPTEXT" in f:
        extra += f" tip={f['TOOLTIPTEXT']}"
    print(f"{'  ' * depth}{name} [{f.get('WINDOWTYPE', '')}] {r} {f.get('STATUS', '')} {' '.join(imgs)}{extra}")
    for c in w["children"]:
        dump(c, depth + 1, all_)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        print(__doc__)
        return
    root = parse(read_text(args[0]))
    for w in root["children"]:
        dump(w, 0, "--all" in sys.argv)


if __name__ == "__main__":
    main()

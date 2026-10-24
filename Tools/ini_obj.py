"""ini_obj.py <ObjectName> [regex of lines to keep context around] [-n maxlines] [-skip REGEX]: prints an Object block of the game's INI data."""
import re
import sys

sys.path.insert(0, r"C:\Users\PC\Documents\Unreal Projects\Generals1\Tools")
from gen_vfs import GeneralsVFS  # noqa: E402

args = sys.argv[1:]
maxn = 400
skip = None
if "-n" in args:
    i = args.index("-n"); maxn = int(args[i + 1]); del args[i:i + 2]
if "-skip" in args:
    i = args.index("-skip"); skip = re.compile(args[i + 1], re.I); del args[i:i + 2]
name = args[0]
vfs = GeneralsVFS()
head = re.compile(r"^\s*(Object|ObjectReskin|ChildObject)\s+" + re.escape(name) + r"\b", re.I)
for g in ("data/ini/object/*.ini",):
    for e in sorted(vfs.glob(g), key=lambda e: e.path.lower()):
        text = vfs.read(e.path).decode("latin-1", "replace").splitlines()
        for start, line in enumerate(text):
            if not head.match(line):
                continue
            n = 0
            skipping_depth = None
            depth = 0
            for i in range(start, len(text)):
                l = text[i].rstrip()
                s = l.split(";")[0].strip()
                if not s:
                    continue
                first = s.split()[0]
                # crude block depth: a line "Key = Value" with a following indented block ends with End
                if skip and skipping_depth is None and skip.search(s) and i != start:
                    skipping_depth = len(l) - len(l.lstrip())
                    continue
                if skipping_depth is not None:
                    ind = len(l) - len(l.lstrip())
                    if ind > skipping_depth:
                        continue
                    if first.lower() == "end" and ind == skipping_depth:
                        skipping_depth = None
                        continue
                    skipping_depth = None
                print(f"{i + 1}: {l[:150]}")
                n += 1
                if n >= maxn or (i != start and re.match(r"^End\s*$", l)):
                    break
            sys.exit(0)
print("not found")

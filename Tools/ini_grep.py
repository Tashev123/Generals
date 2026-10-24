"""ini_grep.py <regex> [path glob, default data\\ini\\*.ini] [-C n] [-m max]: greps the game's INI files (through the VFS)."""
import re
import sys

sys.path.insert(0, r"C:\Users\PC\Documents\Unreal Projects\Generals1\Tools")
from gen_vfs import GeneralsVFS  # noqa: E402

args = sys.argv[1:]
ctx = 0
maxn = 60
if "-C" in args:
    i = args.index("-C"); ctx = int(args[i + 1]); del args[i:i + 2]
if "-m" in args:
    i = args.index("-m"); maxn = int(args[i + 1]); del args[i:i + 2]
pat = re.compile(args[0], re.I)
globs = args[1:] or ["data\\ini\\*.ini", "data\\ini\\object\\*.ini", "data\\ini\\default\\*.ini"]
vfs = GeneralsVFS()
n = 0
for g in globs:
    for e in sorted(vfs.glob(g), key=lambda e: e.path.lower()):
        text = vfs.read(e.path).decode("latin-1", "replace").splitlines()
        for i, line in enumerate(text):
            if pat.search(line):
                lo, hi = max(0, i - ctx), min(len(text), i + ctx + 1)
                for k in range(lo, hi):
                    print(f"{e.path}:{k + 1}:{'>' if k == i else ' '} {text[k].rstrip()}")
                if ctx:
                    print("--")
                n += 1
                if n >= maxn:
                    sys.exit(0)

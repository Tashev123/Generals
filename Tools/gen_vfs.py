"""
Virtual file system over Command & Conquer Generals / Zero Hour .big archives.

Mirrors the original game's lookup order (Win32BIGFileSystem + LocalFileSystem):
  1. Loose files under <ZH>/Data/...          (local file system wins)
  2. *.big in the Zero Hour install dir       (first archive to provide a path wins)
  3. *.big in the base Generals install dir   (fallback for assets ZH doesn't ship)

GenLauncher mod archives (numeric prefix like "340_ControlBarPro.big") and
disabled ones (*.GLR) are skipped so we always read vanilla data.

BIG format (all header ints except archive size are big-endian):
  char[4] "BIGF" | u32le archiveSize | u32be fileCount | u32be headerSize
  fileCount x { u32be offset, u32be size, char name[] (NUL-terminated) }
"""
from __future__ import annotations

import fnmatch
import os
import re
import struct
import sys
from dataclasses import dataclass

ZH_DIR = r"D:\Games\EA Games\Command & Conquer Generals Zero Hour"
GEN_DIR = r"D:\Games\EA Games\Command and Conquer Generals"

_MOD_PREFIX = re.compile(r"^\d+_")


@dataclass
class Entry:
    path: str        # original-case path inside the archive, backslash separated
    source: str      # archive path, or loose file path
    offset: int      # -1 for loose files
    size: int


def read_big_index(archive_path: str) -> list[Entry]:
    with open(archive_path, "rb") as f:
        magic = f.read(4)
        if magic not in (b"BIGF", b"BIG4"):
            raise ValueError(f"{archive_path}: not a BIG archive ({magic!r})")
        _archive_size = struct.unpack("<I", f.read(4))[0]
        count, _header_size = struct.unpack(">II", f.read(8))
        entries = []
        for _ in range(count):
            offset, size = struct.unpack(">II", f.read(8))
            name = bytearray()
            while (c := f.read(1)) not in (b"\0", b""):
                name += c
            entries.append(Entry(name.decode("latin-1"), archive_path, offset, size))
        return entries


def _archives_in(directory: str) -> list[str]:
    names = sorted(
        n for n in os.listdir(directory)
        if n.lower().endswith(".big") and not _MOD_PREFIX.match(n)
    )
    return [os.path.join(directory, n) for n in names]


class GeneralsVFS:
    def __init__(self, zh_dir: str = ZH_DIR, gen_dir: str = GEN_DIR, include_loose: bool = True):
        self.files: dict[str, Entry] = {}
        self.archives: list[str] = []
        if include_loose:
            self._add_loose(zh_dir)
        for d in (zh_dir, gen_dir):
            if d and os.path.isdir(d):
                for arc in _archives_in(d):
                    self.archives.append(arc)
                    for e in read_big_index(arc):
                        self.files.setdefault(_key(e.path), e)

    def _add_loose(self, root: str) -> None:
        data = os.path.join(root, "Data")
        if not os.path.isdir(data):
            return
        for dirpath, _dirs, names in os.walk(data):
            for n in names:
                full = os.path.join(dirpath, n)
                rel = os.path.relpath(full, root)
                self.files.setdefault(_key(rel), Entry(rel, full, -1, os.path.getsize(full)))

    def exists(self, path: str) -> bool:
        return _key(path) in self.files

    def read(self, path: str) -> bytes:
        e = self.files[_key(path)]
        with open(e.source, "rb") as f:
            if e.offset >= 0:
                f.seek(e.offset)
            return f.read(e.size)

    def glob(self, pattern: str) -> list[Entry]:
        pat = _key(pattern)
        return [e for k, e in self.files.items() if fnmatch.fnmatchcase(k, pat)]

    def extract(self, pattern: str, out_dir: str) -> int:
        n = 0
        for e in self.glob(pattern):
            dst = os.path.join(out_dir, *e.path.replace("/", "\\").split("\\"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as f:
                f.write(self.read(e.path))
            n += 1
        return n


def _key(path: str) -> str:
    return path.replace("/", "\\").lower()


if __name__ == "__main__":
    import argparse

    ap = argparse.ArgumentParser(description="Generals ZH virtual file system")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("archives")
    s = sub.add_parser("ls"); s.add_argument("pattern", nargs="?", default="*")
    s = sub.add_parser("stats")
    s = sub.add_parser("extract"); s.add_argument("pattern"); s.add_argument("out")
    a = ap.parse_args()

    vfs = GeneralsVFS()
    if a.cmd == "archives":
        for arc in vfs.archives:
            print(arc)
    elif a.cmd == "ls":
        for e in sorted(vfs.glob(a.pattern), key=lambda e: e.path.lower()):
            print(f"{e.size:>10}  {e.path}  <{os.path.basename(e.source)}>")
    elif a.cmd == "stats":
        from collections import Counter
        ext = Counter(os.path.splitext(k)[1] for k in vfs.files)
        print(f"{len(vfs.files)} files from {len(vfs.archives)} archives")
        for k, v in ext.most_common():
            print(f"  {k or '(none)':8} {v}")
    elif a.cmd == "extract":
        print(f"extracted {vfs.extract(a.pattern, a.out)} files", file=sys.stderr)

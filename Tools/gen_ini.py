"""
Generic tree parser for Generals / Zero Hour INI files.

Mirrors GeneralsMD/Code/GameEngine/Source/Common/INI/INI.cpp:
  * ';' starts a comment, control chars become spaces, tokens split on " \\t=".
  * Every top-level statement is "<BlockType> [args]" closed by "End"
    (except single-line statements such as "ReallyLowMHz = 400").
  * Inside a block, a field opens a nested block only if its parser calls
    INI::initFromINI. That set was extracted from the source (see
    BLOCK_FIELDS / CONTEXT_BLOCK_FIELDS below). Typed interpretation of
    the values happens later, per template type.
"""
from __future__ import annotations

from dataclasses import dataclass, field

TOP_LEVEL_SINGLE_LINE = {"reallylowmhz", "benchprofile", "lodpreset"}

# Fields that open a nested block in any context.
BLOCK_FIELDS = {
    # ThingTemplate
    "draw", "body", "behavior", "clientupdate", "weaponset", "armorset", "prerequisites",
    "unitspecificsounds", "unitspecificfx", "addmodule", "replacemodule", "inheritablemodule",
    "overrideablebylikekind",
    # W3DModelDraw (AliasConditionState does NOT open a block)
    "defaultconditionstate", "conditionstate", "transitionstate",
    # RadiusDecalTemplate::parseRadiusDecalTemplate users inside modules / nuggets.
    # (InGameUI's "*RadiusCursor" blocks are matched by suffix, see _opens_block.)
    "attackareadecal", "deliverydecal", "griddecaltemplate", "targetingreticledecal",
}

# Fields that open a nested block only under a given parent block keyword.
CONTEXT_BLOCK_FIELDS = {
    "fxlist": {"sound", "rayeffect", "tracer", "lightpulse", "viewshake", "terrainscorch",
               "particlesystem", "fxlistatbonepos"},
    "objectcreationlist": {"createobject", "createdebris", "applyrandomforce", "deliverpayload",
                           "fireweapon", "attack"},
    "aidata": {"sideinfo", "skirmishbuildlist"},
    "sideinfo": {"skillset1", "skillset2", "skillset3", "skillset4", "skillset5"},
    "skirmishbuildlist": {"structure"},
    "evaevent": {"sidesounds"},
    "campaign": {"mission"},
    "controlbarscheme": {"imagepart", "animatingpart"},
    "animatingpart": {"imagepart"},
    "shellmenuscheme": {"imagepart", "linepart"},
    "windowtransition": {"window"},
    "challengegenerals": {f"generalpersona{i}" for i in range(12)},
}

# Fields that open a block only when written without a value (e.g. AIUpdate's "Turret"
# block vs. W3DModelDraw's "Turret = TurretBone").
VALUELESS_BLOCK_FIELDS = {"turret", "altturret"}

SEPS = " \t="


@dataclass
class Entry:
    key: str
    values: list[str]
    raw: str                      # text after the key, comment stripped
    line: int
    block: "Block | None" = None


@dataclass
class Block:
    keyword: str
    args: list[str]
    file: str
    line: int
    entries: list[Entry] = field(default_factory=list)

    def get(self, key: str) -> Entry | None:
        k = key.lower()
        for e in self.entries:
            if e.key.lower() == k:
                return e
        return None

    def get_all(self, key: str) -> list[Entry]:
        k = key.lower()
        return [e for e in self.entries if e.key.lower() == k]

    @property
    def name(self) -> str:
        return self.args[0] if self.args else ""


class IniError(Exception):
    pass


def _tokens(text: str) -> list[str]:
    out, cur = [], []
    for ch in text:
        if ch in SEPS:
            if cur:
                out.append("".join(cur)); cur = []
        else:
            cur.append(ch)
    if cur:
        out.append("".join(cur))
    return out


def _clean(line: str) -> str:
    i = line.find(";")
    if i >= 0:
        line = line[:i]
    return "".join(" " if 0 < ord(c) < 32 else c for c in line)


def _split_key(line: str) -> tuple[str, str]:
    s = line.lstrip(SEPS)
    i = 0
    while i < len(s) and s[i] not in SEPS:
        i += 1
    key = s[:i]
    rest = s[i:].lstrip(" \t")
    if rest.startswith("="):
        rest = rest[1:]
    return key, rest.strip()


def _opens_block(parent_kw: str, key: str, values: list[str]) -> bool:
    k = key.lower()
    if k in BLOCK_FIELDS:
        return True
    if k in VALUELESS_BLOCK_FIELDS and not values:
        return True
    if k.endswith("radiuscursor") and not values:
        return True
    ctx = CONTEXT_BLOCK_FIELDS.get(parent_kw.lower())
    return bool(ctx and k in ctx)


def parse(text: str, filename: str = "") -> list[Block]:
    lines = text.splitlines()
    blocks: list[Block] = []
    stack: list[Block] = []
    for ln, rawline in enumerate(lines, 1):
        line = _clean(rawline)
        key, rest = _split_key(line)
        if not key:
            continue
        values = _tokens(rest)
        if key.lower() == "end":
            if not stack:
                raise IniError(f"{filename}:{ln}: unmatched End")
            stack.pop()
            continue
        if not stack:
            if key.lower() in TOP_LEVEL_SINGLE_LINE:
                blocks.append(Block(key, values, filename, ln))
                continue
            b = Block(key, values, filename, ln)
            blocks.append(b)
            stack.append(b)
            continue
        parent = stack[-1]
        e = Entry(key, values, rest, ln)
        parent.entries.append(e)
        if _opens_block(parent.keyword, key, values):
            e.block = Block(key, values, filename, ln)
            stack.append(e.block)
    if stack:
        raise IniError(f"{filename}: unterminated block {stack[-1].keyword} {stack[-1].args} from line {stack[-1].line}")
    return blocks


# ---- load order, as GameEngine::init / INI::loadDirectory ----------------------
def ini_files_in(vfs, directory: str) -> list[str]:
    """INI::loadDirectory: files directly in dir first (sorted), then subdirectories."""
    prefix = directory.lower().rstrip("\\") + "\\"
    names = sorted(e.path for e in vfs.files.values()
                   if e.path.lower().startswith(prefix) and e.path.lower().endswith(".ini"))
    top = [n for n in names if "\\" not in n[len(prefix):]]
    sub = [n for n in names if "\\" in n[len(prefix):]]
    return top + sub


if __name__ == "__main__":
    import sys, collections
    from gen_vfs import GeneralsVFS

    vfs = GeneralsVFS()
    files = [e.path for e in vfs.files.values() if e.path.lower().endswith(".ini") and e.path.lower().startswith("data\\ini")]
    kinds = collections.Counter()
    errors = 0
    for path in sorted(files):
        try:
            for b in parse(vfs.read(path).decode("latin-1"), path):
                kinds[b.keyword] += 1
        except IniError as ex:
            errors += 1
            print("ERROR", ex)
    print(f"{len(files)} files, {sum(kinds.values())} top-level blocks, {errors} errors")
    for k, v in kinds.most_common():
        print(f"  {k:32} {v}")

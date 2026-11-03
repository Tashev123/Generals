"""compare_modules.py <dump from -GenDumpStats=...>: the rest of what compare_stats.py doesn't cover, read again without the port's code.

 1. Upgrades, sciences, ranks and special powers, value by value.
 2. Every line the port read of every INI file: the merged objects' modules (health, damage, delays, radii, ...) and all other files.
 3. The port's unit conversions against the original's field tables: a key the game parses as a duration, speed, angle or percent
    has to be turned into the same thing here (static: the source of both, line by line).
 4. Keys in the data that no code here ever names: what the port ignores.
"""
import glob
import math
import re
import sys
from collections import Counter, defaultdict

sys.path.insert(0, r"C:\Users\PC\Documents\Unreal Projects\Generals1\Tools")
from gen_ini import parse  # noqa: E402
from gen_vfs import GeneralsVFS  # noqa: E402

SRC = r"C:\Users\PC\Documents\Unreal Projects\Generals1\Source\Generals1"
EA = r"C:\Users\PC\Documents\Unreal Projects\_Reference\CnC_Generals_Zero_Hour\GeneralsMD\Code"
MODULE_KEYS = {"body", "behavior", "draw", "clientupdate"}
FPS = 30.0


def frames(ms):
    return int(math.ceil(float(ms) * FPS / 1000.0 - 1e-6))


def blocks_of(vfs, paths):
    out = []
    for p in paths:
        if vfs.exists(p):
            out += parse(vfs.read(p).decode("latin-1", "replace"), p)
    return out


def merged_objects(vfs):
    files = [e.path for e in vfs.glob("data\\ini\\object\\*.ini")] + [r"Data\INI\Default\Object.ini", r"Data\INI\Crate.ini", r"Data\INI\Default\Crate.ini"]
    objs, later = {}, []
    for b in blocks_of(vfs, files):
        kw = b.keyword.lower()
        if kw == "object":
            objs[b.name.lower()] = list(b.entries)
        elif kw in ("objectreskin", "childobject") and len(b.args) >= 2:
            later.append(b)
    tag = lambda e: e.values[1].lower() if len(e.values) > 1 else ""
    for b in later:
        base = objs.get(b.args[1].lower())
        if base is None:
            continue
        ents, replaced = list(base), set()
        for e in b.entries:
            k = e.key.lower()
            if k in MODULE_KEYS:
                for i, x in enumerate(ents):
                    if x.key.lower() in MODULE_KEYS and tag(x) == tag(e):
                        ents[i] = e
                        break
                else:
                    ents.append(e)
            else:
                if k not in replaced:
                    replaced.add(k)
                    ents = [x for x in ents if x.key.lower() != k]
                ents.append(e)
        objs[b.name.lower()] = ents
    return objs


def tree_lines(at, entries, out):
    for e in entries:
        out[f"T|{at.lower()}|{e.key.lower()}|{' '.join(e.values).lower()}"] += 1
        if e.block:
            tree_lines(at + "/" + e.key + ":" + " ".join(e.block.args), e.block.entries, out)


def main():
    dump = sys.argv[1]
    vfs = GeneralsVFS()
    port_lines = Counter()
    port = defaultdict(lambda: defaultdict(dict))
    for line in open(dump, encoding="utf-8", errors="replace"):
        line = line.rstrip("\r\n").lstrip("\ufeff")
        if line.startswith("T|"):
            port_lines[line] += 1
        else:
            parts = line.split("|", 3)
            if len(parts) == 4:
                port[parts[0]][parts[1]][parts[2]] = parts[3]

    # ---- 1
    bad = 0
    ini = defaultdict(dict)
    for b in blocks_of(vfs, [r"Data\INI\Default\Upgrade.ini", r"Data\INI\Upgrade.ini"]):
        if b.keyword.lower() == "upgrade":
            t = b.get("Type")
            ini["U"][b.name.lower()] = {"BuildCost": int(float(b.get("BuildCost").values[0])) if b.get("BuildCost") else 0,
                                        "BuildTime": float(b.get("BuildTime").values[0]) if b.get("BuildTime") else 0.0,
                                        "Object": 1 if t and t.values[0].upper() == "OBJECT" else 0}
    for b in blocks_of(vfs, [r"Data\INI\Default\Science.ini", r"Data\INI\Science.ini"]):
        if b.keyword.lower() == "science":
            g = b.get("IsGrantable")
            pre = b.get("PrerequisiteSciences")
            ini["S"][b.name.lower()] = {"Cost": int(float(b.get("SciencePurchasePointCost").values[0])) if b.get("SciencePurchasePointCost") else 0,
                                        "Prerequisites": " ".join(sorted(v.lower() for v in pre.values if v.lower() != "none")) if pre else "",
                                        "Grantable": (1 if g.values[0].lower() in ("yes", "1", "true") else 0) if g else 1}
    for b in blocks_of(vfs, [r"Data\INI\Default\Rank.ini", r"Data\INI\Rank.ini"]):
        if b.keyword.lower() == "rank":
            sg = b.get("SciencesGranted")
            ini["R"][b.name.lower()] = {"Skill": int(b.get("SkillPointsNeeded").values[0]) if b.get("SkillPointsNeeded") else 0,
                                        "Points": int(b.get("SciencePurchasePointsGranted").values[0]) if b.get("SciencePurchasePointsGranted") else 0,
                                        "Sciences": " ".join(sorted(v.lower() for v in sg.values if v.lower() != "none")) if sg else ""}
    for b in blocks_of(vfs, [r"Data\INI\Default\SpecialPower.ini", r"Data\INI\SpecialPower.ini"]):
        if b.keyword.lower() == "specialpower":
            yes = lambda k: 1 if b.get(k) and b.get(k).values[0].lower() in ("yes", "1", "true") else 0
            flt = lambda k: float(b.get(k).values[0]) if b.get(k) else 0.0
            rs = b.get("RequiredScience")
            ini["P"][b.name.lower()] = {"Reload": frames(flt("ReloadTime")), "RequiredScience": rs.values[0].lower() if rs and rs.values and rs.values[0].lower() != "none" else "",
                                        "Public": yes("PublicTimer"), "Shared": yes("SharedSyncedTimer"), "Radius": flt("RadiusCursorRadius"),
                                        "ViewFrames": frames(flt("ViewObjectDuration")), "ViewRange": flt("ViewObjectRange")}
    def same(a, b):
        try:
            return abs(float(a) - float(b)) <= max(1e-4, abs(float(b)) * 1e-4)
        except ValueError:
            return str(a) == str(b)
    for kind, title in (("U", "upgrades"), ("S", "sciences"), ("R", "ranks"), ("P", "powers")):
        n = 0
        names = set(ini[kind]) | set(port[kind])
        if kind == "R":
            print(f"-- ranks: port {sorted(port[kind])} game {sorted(ini[kind])}")
        for name in sorted(names):
            if name not in ini[kind] or name not in port[kind]:
                print(f"{title} {name}: only in {'the port' if name in port[kind] else 'the game'}")
                bad += 1
                continue
            for f, v in ini[kind][name].items():
                n += 1
                if not same(port[kind][name].get(f, ""), v):
                    bad += 1
                    print(f"{title} {name}.{f}: game {v!r}, port {port[kind][name].get(f)!r}")
        print(f"-- {title}: {len(names)} entries, {n} values")
    print("differences in 1:", bad)

    # ---- 2
    mine = Counter()
    for name, ents in merged_objects(vfs).items():
        tree_lines("object:" + name, ents, mine)
    seen_files = set()
    for e in vfs.glob("data\\ini\\*.ini") + vfs.glob("data\\ini\\default\\*.ini"):
        low = e.path.lower().replace("/", "\\")
        if "\\object\\" in low or "\\mappedimages\\" in low or low in seen_files:
            continue
        seen_files.add(low)
        short = e.path.replace("/", "\\").split("\\")[-1].rsplit(".", 1)[0]
        for b in parse(vfs.read(e.path).decode("latin-1", "replace"), e.path):
            tree_lines(short + "/" + b.keyword + ":" + " ".join(b.args), b.entries, mine)
    only_port = port_lines - mine
    only_mine = mine - port_lines
    print(f"-- lines as read: port {sum(port_lines.values())}, game {sum(mine.values())}; only in the port {sum(only_port.values())}, only in the game {sum(only_mine.values())}")
    for what, c in (("port", only_port), ("game", only_mine)):
        tally = Counter(k.split("|")[1].split("/")[0].split(":")[0] for k in c)
        print(f"   only in the {what}, by where:", tally.most_common(12))
        for k in list(c)[:25]:
            print("    ", k[:190])

    # ---- 3
    src = {}
    for p in glob.glob(SRC + r"\**\*.cpp", recursive=True):
        src[p] = open(p, encoding="utf-8", errors="replace").read().splitlines()
    ea_types = defaultdict(set)
    pat = re.compile(r'\{\s*"(\w+)"\s*,\s*([\w:]+)')
    for root in ("GameEngine", "GameEngineDevice"):
        for p in glob.glob(EA + "\\" + root + r"\Source\**\*.cpp", recursive=True):
            for k, t in pat.findall(open(p, encoding="latin-1").read()):
                ea_types[k.lower()].add(t.split("::")[-1])
    want = {"parseDurationUnsignedInt": "frames", "parseDurationReal": "frames", "parseVelocityReal": "speed", "parseAccelerationReal": "accel", "parseAngleReal": "angle",
            "parseAngularVelocityReal": "angspeed", "parsePercentToReal": "percent", "parseAngularAccelerationReal": "angaccel"}
    conv = (("frames", ("MsecToFrames", "FramesPerMs", "Msec(", "Ms(")), ("speed", ("VelocityPerFrame", "Speed(")), ("accel", ("AccelPerFrame",)), ("angspeed", ("AngularVelocityPerFrame", "AngularSpeed(")),
            ("angle", ("DegreesToRadians", "Radians(", "Angle(")), ("percent", ("ParsePercent", "UpgPercent", "Percent(", "Percent")))
    mism = []
    seen = Counter()
    for p, lines in src.items():
        for n, line in enumerate(lines):
            for key in re.findall(r'TEXT\("(\w+)"\)', line):
                types = ea_types.get(key.lower())
                if not types:
                    continue
                exp = {want[t] for t in types if t in want}
                if not exp or len(types) > 1 and len({want.get(t, "plain") for t in types}) > 1:
                    continue
                e = exp.pop()
                if not re.search(r"Get(Float|Int)|AsFloat|AsInt|Is\(TEXT|Equals\(TEXT|Find\(TEXT|Percent|Value", line):
                    continue
                seen[e] += 1
                near = line + " " + (lines[n + 1] if n + 1 < len(lines) else "")
                got = {c for c, needles in conv if any(x in near for x in needles)}
                if e not in got:
                    mism.append((p.split("\\")[-1], n + 1, key, e, line.strip()[:120]))
    print("-- conversions looked at:", dict(seen), "not obviously converted:", len(mism))
    for m in mism:
        print("  ", m)

    # ---- 4
    whole = "\n".join("\n".join(l) for l in src.values()) + "\n".join(open(p, encoding="utf-8", errors="replace").read() for p in glob.glob(SRC + r"\**\*.h", recursive=True))
    unread = Counter()
    example = {}
    def walk(ents, owner, obj):
        for e in ents:
            if e.block:
                walk(e.block.entries, e.block.args[0] if e.block.args else e.key, obj)
            elif f'"{e.key}"' not in whole and not re.search(rf'"{e.key}"', whole, re.I):
                if e.values and re.fullmatch(r"-?[\d.]+%?", e.values[0]):
                    unread[(owner, e.key)] += 1
                    example.setdefault((owner, e.key), (obj, e.values[0]))
    for name, ents in merged_objects(vfs).items():
        walk(ents, "Object", name)
    print("-- numeric keys of objects' modules no code names:", len(unread))
    for (owner, key), c in sorted(unread.items(), key=lambda kv: (kv[0][0], kv[0][1])):
        print(f"   {owner}.{key}: {c} objects, e.g. {example[(owner, key)]}")


if __name__ == "__main__":
    main()

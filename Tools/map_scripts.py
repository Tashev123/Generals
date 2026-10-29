"""Dump a map's own scripts, sides, teams and named objects (campaign maps): Maps\\X\\X.map from the archives.

map_scripts.py <map path in the VFS, e.g. maps/md_usa01/md_usa01.map> [out.txt]

Prints statistics (sides, teams, conditions and actions used) and writes the full text dump.
"""
import collections
import struct
import sys

sys.path.insert(0, r"C:\Users\PC\Documents\Unreal Projects\Generals1\Tools")
import gen_vfs
import gen_map

COORD3D = 16
vfs = gen_vfs.GeneralsVFS()
data = gen_map.decompress(vfs.read(sys.argv[1]))
assert data[:4] == b"CkMp", data[:8]
p = 4
count = struct.unpack_from("<i", data, p)[0]; p += 4
names = {}
for _ in range(count):
    n = data[p]; p += 1
    name = data[p:p + n].decode("latin-1"); p += n
    names[struct.unpack_from("<I", data, p)[0]] = name; p += 4
r = gen_map.Reader(data, names)
r.p = p

out = []
cond_hist = collections.Counter()
act_hist = collections.Counter()
stats = collections.Counter()


def read_param(r):
    t = r.i32()
    if t == COORD3D:
        return (t, (r.f32(), r.f32(), r.f32()))
    i, f, s = r.i32(), r.f32(), r.astr()
    return (t, i, round(f, 3), s)


def fmt_parm(pm):
    if pm[0] == COORD3D:
        return f"xyz({pm[1][0]:.0f},{pm[1][1]:.0f},{pm[1][2]:.0f})"
    t, i, f, s = pm
    if s:
        return f"'{s}'"
    if f != 0.0 and i == 0:
        return f"{f}"
    if f != 0.0:
        return f"{i}/{f}"
    return f"{i}"


def read_cond_or_action(r, ver, key_from_ver):
    ctype = r.i32()
    key = names.get(r.i32() >> 8, "?") if ver >= key_from_ver else f"#{ctype}"
    n = r.i32()
    parms = [read_param(r) for _ in range(n)]
    return key, parms


def read_script(r, ver, end, indent):
    name, comment, ccomment, acomment = r.astr(), r.astr(), r.astr(), r.astr()
    active, oneshot, easy, normal, hard, sub = (r.u8() for _ in range(6))
    delay = r.i32() if ver >= 2 else 0
    stats["scripts"] += 1
    flags = ("" if active else " INACTIVE") + (" oneshot" if oneshot else "") + (" SUB" if sub else "") + (f" every {delay}s" if delay else "")
    diff = "".join(c for c, on in zip("ENH", (easy, normal, hard)) if on)
    out.append(f"{indent}SCRIPT '{name}' [{diff}]{flags}")
    if comment:
        out.append(f"{indent}  // {comment!r}")
    first_or = True
    for cname, cver, s, e in r.chunks(end):
        if cname == "OrCondition":
            out.append(f"{indent}  {'IF' if first_or else 'OR'}")
            first_or = False
            for c2, v2, s2, e2 in r.chunks(e):
                if c2 == "Condition":
                    key, parms = read_cond_or_action(r, v2, 4)
                    cond_hist[key] += 1
                    out.append(f"{indent}     {key}({', '.join(fmt_parm(x) for x in parms)})")
        elif cname in ("ScriptAction", "ScriptActionFalse"):
            key, parms = read_cond_or_action(r, cver, 2)
            act_hist[key] += 1
            out.append(f"{indent}  {'THEN' if cname == 'ScriptAction' else 'ELSE'} {key}({', '.join(fmt_parm(x) for x in parms)})")


def read_script_list(r, end, indent):
    for cname, cver, s, e in r.chunks(end):
        if cname == "Script":
            read_script(r, cver, e, indent)
        elif cname == "ScriptGroup":
            gname = r.astr()
            active = r.u8()
            sub = r.u8() if cver >= 2 else 0
            stats["groups"] += 1
            out.append(f"{indent}GROUP '{gname}'{'' if active else ' INACTIVE'}{' SUB' if sub else ''}")
            for c2, v2, s2, e2 in r.chunks(e):
                if c2 == "Script":
                    read_script(r, v2, e2, indent + "    ")


sides, teams = [], []
for cname, cver, s, e in r.chunks(len(data)):
    if cname == "SidesList":
        for _ in range(r.i32()):
            props = r.dict()
            build = []
            for _ in range(r.i32()):
                b = {"building": r.astr(), "template": r.astr(), "x": r.f32(), "y": r.f32(), "z": r.f32(),
                     "angle": r.f32(), "initiallyBuilt": r.u8(), "numRebuilds": r.i32()}
                if cver >= 3:
                    b.update(script=r.astr(), health=r.i32(), whiner=r.u8(), unsellable=r.u8(), repairable=r.u8())
                build.append(b)
            sides.append((props, build))
        if cver >= 2:
            teams = [r.dict() for _ in range(r.i32())]
        for c2, v2, s2, e2 in r.chunks(e):
            if c2 == "PlayerScriptsList":
                idx = 0
                for c3, v3, s3, e3 in r.chunks(e2):
                    if c3 == "ScriptList":
                        pname = sides[idx][0].get("playerName", "?") if idx < len(sides) else "?"
                        out.append(f"===== SCRIPT LIST {idx} ('{pname}')")
                        idx += 1
                        read_script_list(r, e3, "  ")
    elif cname == "ObjectsList":
        for c2, v2, s2, e2 in r.chunks(e):
            if c2 == "Object":
                x, y, z, ang = r.f32(), r.f32(), r.f32(), r.f32()
                flags = r.i32()
                oname = r.astr()
                props = r.dict() if v2 >= 2 else {}
                stats["objects"] += 1
                keep = {k: v for k, v in props.items() if k not in ("objectInitialHealth", "objectEnabled", "objectIndestructible",
                                                                    "objectUnsellable", "objectPowered", "objectRecruitableAI",
                                                                    "objectTargetable", "objectSelectable", "objectAggressiveness",
                                                                    "objectVisualRange", "objectShroudClearingDistance",
                                                                    "objectStoppingDistance", "objectMaxHPs", "objectRadius")}
                if "objectName" in props or "waypointName" in props or props.get("originalOwner", "").find("/") >= 0 or "objectScriptAttachment" in props:
                    out.append(f"OBJECT {oname} at ({x:.0f},{y:.0f},{z:.0f}) ang={ang:.2f} {keep}")
    elif cname == "PolygonTriggers":
        n = r.i32()
        for _ in range(n):
            tname = r.astr()
            if cver >= 4:
                r.astr()  # layer name
            tid = r.i32()
            water = r.u8() if cver >= 2 else 0
            river = r.u8() if cver >= 3 else 0
            rstart = r.i32() if cver >= 3 else 0
            npts = r.i32()
            pts = [(r.i32(), r.i32(), r.i32()) for _ in range(npts)]
            if not water:
                xs = [q[0] for q in pts]; ys = [q[1] for q in pts]
                out.append(f"TRIGGER '{tname}' id={tid} pts={npts} x {min(xs)}..{max(xs)} y {min(ys)}..{max(ys)}")

print("sides:")
for props, build in sides:
    print("  ", {k: v for k, v in props.items()}, f"build list {len(build)}")
    for b in build:
        out.append(f"BUILD {props.get('playerName')} {b}")
print("teams:", len(teams), " scripts:", stats["scripts"], " groups:", stats["groups"], " objects:", stats["objects"])
for t in teams:
    out.append("TEAM " + ", ".join(f"{k}={v}" for k, v in t.items()))
print("--- conditions used:")
for k, v in cond_hist.most_common():
    print(f"   {v:5d} {k}")
print("--- actions used:")
for k, v in act_hist.most_common():
    print(f"   {v:5d} {k}")
dest = sys.argv[2] if len(sys.argv) > 2 else None
if dest:
    with open(dest, "w", encoding="utf-8") as f:
        f.write("\n".join(out))
    print("wrote", dest, len(out), "lines")

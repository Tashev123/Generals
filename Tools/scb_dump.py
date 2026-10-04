"""Decode Data\\Scripts\\SkirmishScripts.scb: players, teams, scripts; write a full text dump and print statistics.

scb_dump.py [out.txt]
"""
import collections
import struct
import sys

sys.path.insert(0, r"C:\Users\PC\Documents\Unreal Projects\Generals1\Tools")
import gen_vfs
import gen_map

COORD3D = 16
vfs = gen_vfs.GeneralsVFS()
data = vfs.read("data/scripts/skirmishscripts.scb")
try:
    data = gen_map.decompress(data)
except Exception:  # noqa
    pass
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
players = []
teams = []


def read_param(r):
    t = r.i32()
    if t == COORD3D:
        return (t, (r.f32(), r.f32(), r.f32()))
    i, f, s = r.i32(), r.f32(), r.astr()
    return (t, i, round(f, 3), s)


def fmt_parm(pm):
    if pm[0] == COORD3D:
        return f"xyz{pm[1]}"
    t, i, f, s = pm
    if s:
        return f"'{s}'"
    if f != 0.0 and i == 0:
        return f"{f}"
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


top = []
for cname, cver, s, e in r.chunks(len(data)):
    top.append((cname, cver, e - s))
    if cname == "PlayerScriptsList":
        idx = 0
        for c2, v2, s2, e2 in r.chunks(e):
            if c2 == "ScriptList":
                out.append(f"===== SCRIPT LIST {idx}")
                idx += 1
                read_script_list(r, e2, "  ")
    elif cname == "ScriptsPlayers":
        read_dicts = r.i32() if cver >= 2 else 0
        n = r.i32()
        for _ in range(n):
            pname = r.astr()
            d = r.dict() if read_dicts else {}
            players.append(pname)
            out.append(f"PLAYER '{pname}' {d}")
    elif cname == "ScriptTeams":
        while r.p < e:
            teams.append(r.dict())
print("top-level chunks:", top)
print("players:", players)
print("teams:", len(teams), " scripts:", stats["scripts"], " groups:", stats["groups"])
by_owner = collections.Counter(t.get("teamOwner", "?") for t in teams)
print("teams per owner:", dict(by_owner))
print("--- conditions used:")
for k, v in cond_hist.most_common():
    print(f"   {v:5d} {k}")
print("--- actions used:")
for k, v in act_hist.most_common():
    print(f"   {v:5d} {k}")
keys = collections.Counter()
for t in teams:
    keys.update(t.keys())
print("--- team dict keys:", dict(keys))

for t in teams:
    out.append("TEAM " + ", ".join(f"{k}={v}" for k, v in t.items()))
dest = sys.argv[1] if len(sys.argv) > 1 else None
if dest:
    with open(dest, "w", encoding="utf-8") as f:
        f.write("\n".join(out))
    print("wrote", dest, len(out), "lines")

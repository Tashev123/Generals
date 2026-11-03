"""compare_stats.py <dump from -GenDumpStats=...> [zh dir] [generals dir]: every number the port holds against the game's own INI.

Reads the data again by itself (gen_vfs + gen_ini, no code shared with the port): weapons, armors, locomotors and objects,
ObjectReskin included, and prints what differs. Values go through the same unit changes as the game's parsers:
msec -> frames (parseDurationUnsignedInt: rounded up), per second -> per frame. Exit code 1 if anything differs.
"""
import math
import sys
from collections import Counter, defaultdict

sys.path.insert(0, r"C:\Users\PC\Documents\Unreal Projects\Generals1\Tools")
from gen_ini import parse  # noqa: E402
from gen_vfs import GeneralsVFS  # noqa: E402

FPS = 30.0
MODULE_KEYS = {"body", "behavior", "draw", "clientupdate", "addmodule", "replacemodule", "inheritablemodule", "overrideablebylikekind"}
AFFECTS = {"SELF": 1, "ALLIES": 2, "ENEMIES": 4, "NEUTRALS": 8, "SUICIDE": 16, "NOT_SIMILAR": 32, "NOT_AIRBORNE": 64}
BODY_SUFFIX = "body"


def frames(ms):
    return int(math.ceil(float(ms) * FPS / 1000.0 - 1e-6))


def blocks_of(vfs, paths):
    out = []
    for p in paths:
        if vfs.exists(p):
            out += parse(vfs.read(p).decode("latin-1", "replace"), p)
    return out


def num(e, default=0.0):
    if not e or not e.values:
        return default
    return float(e.values[0].rstrip("%"))


def last(block, key):
    all_ = block.get_all(key)
    return all_[-1] if all_ else None


def weapons(vfs):
    out = {}
    for b in blocks_of(vfs, [r"Data\INI\Default\Weapon.ini", r"Data\INI\Weapon.ini"]):
        if b.keyword.lower() != "weapon":
            continue
        name = b.name.lower()
        w = out.setdefault(name, {"PrimaryDamage": 0.0, "PrimaryDamageRadius": 0.0, "SecondaryDamage": 0.0, "SecondaryDamageRadius": 0.0, "AttackRange": 0.0,
                                  "MinimumAttackRange": 0.0, "ScatterRadius": 0.0, "ScatterRadiusVsInfantry": 0.0, "DamageType": "EXPLOSION", "DeathType": "NORMAL",
                                  "WeaponSpeed": 999999.0, "ClipSize": 0, "ClipReloadTime": 0, "DelayMin": 0, "DelayMax": 0, "PreAttackDelay": 0, "ProjectileObject": "",
                                  "MinWeaponSpeed": 999999.0, "ScaleWeaponSpeed": 0, "AntiGround": 1, "AntiAirborneVehicle": 0, "AntiAirborneInfantry": 0, "RadiusDamageAffects": 2 | 4 | 8, "Bonuses": []})
        for e in b.entries:
            k = e.key.lower()
            if k in ("primarydamage", "primarydamageradius", "secondarydamage", "secondarydamageradius", "attackrange", "minimumattackrange", "scatterradius", "scatterradiusvsinfantry"):
                w[{x.lower(): x for x in w}[k]] = num(e)
            elif k == "damagetype":
                w["DamageType"] = e.values[0].upper()
            elif k == "deathtype":
                w["DeathType"] = e.values[0].upper()
            elif k == "weaponspeed":
                w["WeaponSpeed"] = num(e) / FPS
            elif k == "minweaponspeed":
                w["MinWeaponSpeed"] = num(e) / FPS
            elif k == "scaleweaponspeed":
                w["ScaleWeaponSpeed"] = 1 if e.values[0].lower() in ("yes", "1", "true") else 0
            elif k == "clipsize":
                w["ClipSize"] = int(num(e))
            elif k == "clipreloadtime":
                w["ClipReloadTime"] = frames(num(e))
            elif k == "preattackdelay":
                w["PreAttackDelay"] = frames(num(e))
            elif k == "delaybetweenshots":
                t = e.raw.replace(":", " ").replace("=", " ").split()
                if len(t) >= 4 and t[0].lower() == "min":
                    w["DelayMin"], w["DelayMax"] = frames(t[1]), frames(t[3])
                else:
                    w["DelayMin"] = w["DelayMax"] = frames(t[0])
            elif k == "projectileobject":
                v = e.values[0] if e.values else ""
                w["ProjectileObject"] = "" if v.lower() == "none" else v.lower()
            elif k in ("antiground", "antiairbornevehicle", "antiairborneinfantry"):
                w[{"antiground": "AntiGround", "antiairbornevehicle": "AntiAirborneVehicle", "antiairborneinfantry": "AntiAirborneInfantry"}[k]] = 1 if e.values[0].lower() in ("yes", "1", "true") else 0
            elif k == "radiusdamageaffects":
                w["RadiusDamageAffects"] = sum(AFFECTS.get(v.upper(), 0) for v in e.values)
            elif k == "weaponbonus" and len(e.values) >= 3:
                f = ["DAMAGE", "RADIUS", "RANGE", "RATE_OF_FIRE", "PRE_ATTACK"]
                if e.values[1].upper() in f:
                    w["Bonuses"].append(f"{e.values[0].upper()}/{f.index(e.values[1].upper())}/{float(e.values[2].rstrip('%')) / 100.0:.4f}")
    for w in out.values():
        w["Bonuses"] = ";".join(sorted(w["Bonuses"]))
    return out


def armors(vfs):
    out = {}
    for b in blocks_of(vfs, [r"Data\INI\Default\Armor.ini", r"Data\INI\Armor.ini"]):
        if b.keyword.lower() != "armor":
            continue
        a = {"DEFAULT": 1.0}
        for e in b.get_all("Armor"):
            if len(e.values) >= 2:
                v = float(e.values[1].rstrip("%")) / 100.0
                if e.values[0].upper() == "DEFAULT":
                    a = {"DEFAULT": v}
                else:
                    a[e.values[0].upper()] = v
        out[b.name.lower()] = a
    return out


def locomotors(vfs):
    out = {}
    for b in blocks_of(vfs, [r"Data\INI\Default\Locomotor.ini", r"Data\INI\Locomotor.ini"]):
        if b.keyword.lower() != "locomotor":
            continue
        l = out.setdefault(b.name.lower(), {"Speed": 0.0, "TurnRate": 0.0, "Acceleration": 0.0, "Braking": 99999.0})
        for k, conv in (("Speed", lambda v: v / FPS), ("TurnRate", lambda v: math.radians(v) / FPS), ("Acceleration", lambda v: v / FPS / FPS), ("Braking", lambda v: v / FPS / FPS)):
            e = last(b, k)
            if e:
                l[k] = conv(num(e))
    return out


def merged_objects(vfs):
    """Object blocks with ObjectReskin worked out: a copy of the parent, its fields replaced, its modules by tag."""
    files = [e.path for e in vfs.glob("data\\ini\\object\\*.ini")] + [r"Data\INI\Crate.ini", r"Data\INI\Default\Crate.ini"]
    objs, reskins = {}, []
    for b in blocks_of(vfs, files):
        kw = b.keyword.lower()
        if kw == "object":
            objs[b.name.lower()] = list(b.entries)
        elif kw in ("objectreskin", "childobject") and len(b.args) >= 2:
            reskins.append(b)
    for b in reskins:
        base = objs.get(b.args[1].lower())
        if base is None:
            continue
        entries = list(base)
        replaced = set()
        for e in b.entries:
            k = e.key.lower()
            if k in MODULE_KEYS:
                tag = e.values[-1].lower() if e.values else ""
                entries = [x for x in entries if not (x.key.lower() in MODULE_KEYS and x.values and x.values[-1].lower() == tag)]
                entries.append(e)
            else:
                if k not in replaced:
                    entries = [x for x in entries if x.key.lower() != k]
                    replaced.add(k)
                entries.append(e)
        objs[b.name.lower()] = entries
    return objs


def object_stats(entries):
    o = {"MaxHealth": 0.0, "InitialHealth": 0.0, "BuildCost": 0.0, "BuildTime": 0.0, "VisionRange": 0.0, "ShroudClearingRange": -1.0, "EnergyProduction": 0, "EnergyBonus": 0,
         "CrusherLevel": 0, "CrushableLevel": 255, "TransportSlotCount": 0, "RefundValue": 0, "SubdualDamageCap": 0.0, "SubdualDamageHealRate": 0, "SubdualDamageHealAmount": 0.0}
    exp_v = [0, 0, 0, 0]
    exp_r = [0, 0, 0, 0]
    armor_sets, weapon_sets = [], []
    for e in entries:
        k = e.key.lower()
        lower = {x.lower(): x for x in o}
        if k in ("buildcost", "buildtime", "visionrange", "shroudclearingrange") and e.values:
            o[lower[k]] = float(e.values[0])
        elif k in ("energyproduction", "energybonus", "crusherlevel", "crushablelevel", "transportslotcount", "refundvalue") and e.values:
            o[lower[k]] = int(float(e.values[0]))
        elif k == "experiencevalue" and e.values:
            exp_v = [int(e.values[min(i, len(e.values) - 1)]) for i in range(4)]
        elif k == "experiencerequired":
            for i, v in enumerate(e.values[:4]):
                exp_r[i] = int(v)
        elif k == "body" and e.block:
            mh = num(e.block.get("MaxHealth"), o["MaxHealth"])
            o["MaxHealth"] = mh
            o["InitialHealth"] = num(e.block.get("InitialHealth"), mh)
            o["SubdualDamageCap"] = num(e.block.get("SubdualDamageCap"), 0.0)
            o["SubdualDamageHealRate"] = frames(num(e.block.get("SubdualDamageHealRate"), 0.0))
            o["SubdualDamageHealAmount"] = num(e.block.get("SubdualDamageHealAmount"), 0.0)
        elif k == "armorset" and e.block:
            c = sorted(v.upper() for ce in e.block.get_all("Conditions") for v in ce.values if v.lower() != "none")
            a = e.block.get("Armor")
            armor_sets.append(f"[{','.join(c)}]{a.values[0].lower() if a and a.values else ''}")
        elif k == "weaponset" and e.block:
            c = sorted(v.upper() for ce in e.block.get_all("Conditions") for v in ce.values if v.lower() != "none")
            w = ["", "", ""]
            for we in e.block.get_all("Weapon"):
                if len(we.values) >= 2 and we.values[0].upper() in ("PRIMARY", "SECONDARY", "TERTIARY"):
                    w[("PRIMARY", "SECONDARY", "TERTIARY").index(we.values[0].upper())] = "" if we.values[1].lower() == "none" else we.values[1].lower()
            weapon_sets.append(f"[{','.join(c)}]{w[0]},{w[1]},{w[2]}")
    o["ExperienceValues"] = " ".join(map(str, exp_v))
    o["ExperienceRequired"] = " ".join(map(str, exp_r))
    o["ArmorSets"] = ";".join(sorted(armor_sets))
    o["WeaponSets"] = ";".join(sorted(weapon_sets))
    return o


def same(a, b):
    if isinstance(a, str) and isinstance(b, str) and ";" in a + b:
        return sorted(a.split(";")) == sorted(b.split(";"))  # (lists: the two sides sort differently)
    try:
        fa, fb = float(a), float(b)
    except (TypeError, ValueError):
        return str(a).strip() == str(b).strip()
    return abs(fa - fb) <= max(1e-4, abs(fb) * 1e-4)


def main():
    dump = sys.argv[1]
    kw = {}
    if len(sys.argv) > 2:
        kw["zh_dir"] = sys.argv[2]
    if len(sys.argv) > 3:
        kw["gen_dir"] = sys.argv[3]
    vfs = GeneralsVFS(**kw)
    port = defaultdict(lambda: defaultdict(dict))
    for line in open(dump, encoding="utf-8", errors="replace"):
        parts = line.rstrip("\r\n").lstrip("\ufeff").split("|", 3)
        if len(parts) == 4:
            port[parts[0]][parts[1]][parts[2]] = parts[3]
    ini = {"W": weapons(vfs), "A": armors(vfs), "L": locomotors(vfs), "O": {k: object_stats(v) for k, v in merged_objects(vfs).items()}}
    bad = 0
    tally = Counter()
    for kind, title in (("W", "weapons"), ("A", "armors"), ("L", "locomotors"), ("O", "objects")):
        names_i, names_p = set(ini[kind]), set(port[kind])
        checked = 0
        for name in sorted(names_i & names_p):
            for field, val in ini[kind][name].items():
                if field not in port[kind][name]:
                    if kind == "A":
                        print(f"{title} {name}.{field}: game {val}, port has none")
                        bad += 1
                    continue
                checked += 1
                if not same(port[kind][name][field], val):
                    bad += 1
                    tally[field] += 1
                    print(f"{title} {name}.{field}: game {val!r}, port {port[kind][name][field]!r}")
            if kind == "A":
                for field in port[kind][name]:
                    if field not in ini[kind][name]:
                        bad += 1
                        print(f"{title} {name}.{field}: port {port[kind][name][field]}, game has none")
        print(f"-- {title}: {len(names_i & names_p)} compared, {checked} values; only in the game: {sorted(names_i - names_p)[:12]} ({len(names_i - names_p)}); only in the port: {sorted(names_p - names_i)[:12]} ({len(names_p - names_i)})")
    print("differences:", bad, dict(tally))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()

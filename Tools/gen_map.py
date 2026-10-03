"""
Generals / Zero Hour .map reader (DataChunk "CkMp" format).

References (EA GPL source, GeneralsMD/Code):
  GameEngine/Source/Common/System/DataChunk.cpp         - TOC + chunk headers + Dict
  GameEngineDevice/.../GameClient/WorldHeightMap.cpp    - HeightMapData, BlendTileData, ObjectsList, GlobalLighting
  GameEngine/Source/GameLogic/Map/SidesList.cpp         - SidesList
  Libraries/Source/Compression/EAC/refdecode.cpp        - RefPack ("EAR\\0")

Chunk header: u32 id (index into TOC), u16 version, i32 size.
"""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass, field

MAP_XY_FACTOR = 10.0
MAP_HEIGHT_SCALE = MAP_XY_FACTOR / 16.0

DICT_BOOL, DICT_INT, DICT_REAL, DICT_ASCIISTRING, DICT_UNICODESTRING = range(5)


# ---- compression -----------------------------------------------------------------
def refpack_decode(src: bytes) -> bytes:
    s = 0
    typ = (src[0] << 8) | src[1]
    s = 2
    if typ & 0x8000:
        if typ & 0x100:
            s += 4
        ulen = int.from_bytes(src[s:s + 4], "big"); s += 4
    else:
        if typ & 0x100:
            s += 3
        ulen = int.from_bytes(src[s:s + 3], "big"); s += 3
    d = bytearray()
    while True:
        first = src[s]; s += 1
        if not first & 0x80:
            second = src[s]; s += 1
            run = first & 3
            d += src[s:s + run]; s += run
            ref = len(d) - 1 - (((first & 0x60) << 3) + second)
            run = ((first & 0x1C) >> 2) + 3
        elif not first & 0x40:
            second, third = src[s], src[s + 1]; s += 2
            run = second >> 6
            d += src[s:s + run]; s += run
            ref = len(d) - 1 - (((second & 0x3F) << 8) + third)
            run = (first & 0x3F) + 4
        elif not first & 0x20:
            second, third, forth = src[s], src[s + 1], src[s + 2]; s += 3
            run = first & 3
            d += src[s:s + run]; s += run
            ref = len(d) - 1 - ((((first & 0x10) >> 4) << 16) + (second << 8) + third)
            run = (((first & 0x0C) >> 2) << 8) + forth + 5
        else:
            run = ((first & 0x1F) << 2) + 4
            if run <= 112:
                d += src[s:s + run]; s += run
                continue
            run = first & 3
            d += src[s:s + run]; s += run
            break
        for _ in range(run):          # overlapping copy, byte by byte
            d.append(d[ref]); ref += 1
    return bytes(d[:ulen]) if ulen else bytes(d)


def decompress(data: bytes) -> bytes:
    tag = data[:4]
    if tag == b"EAR\0":
        return refpack_decode(data[8:])
    if tag[:2] == b"ZL" and tag[3:4] == b"\0":
        return zlib.decompress(data[8:])
    return data


# ---- data model ------------------------------------------------------------------
@dataclass
class TextureClass:
    first_tile: int
    num_tiles: int
    width: int
    name: str


@dataclass
class BlendedTile:
    blend_ndx: int
    horiz: int
    vert: int
    right_diagonal: int
    left_diagonal: int
    inverted: int
    long_diagonal: int
    custom_blend_edge_class: int


@dataclass
class CliffInfo:
    tile_index: int
    uv: tuple
    flip: int
    mutant: int


@dataclass
class MapObject:
    x: float
    y: float
    z: float
    angle: float
    flags: int
    name: str
    props: dict


@dataclass
class Side:
    props: dict
    build_list: list


@dataclass
class GenMap:
    width: int = 0
    height: int = 0
    border: int = 0
    boundaries: list = field(default_factory=list)
    heights: bytes = b""
    tile_ndx: list = field(default_factory=list)
    blend_ndx: list = field(default_factory=list)
    extra_blend_ndx: list = field(default_factory=list)
    cliff_ndx: list = field(default_factory=list)
    num_bitmap_tiles: int = 0
    texture_classes: list = field(default_factory=list)
    edge_texture_classes: list = field(default_factory=list)
    blended_tiles: list = field(default_factory=list)
    cliff_info: list = field(default_factory=list)
    objects: list = field(default_factory=list)
    world: dict = field(default_factory=dict)
    sides: list = field(default_factory=list)
    teams: list = field(default_factory=list)
    time_of_day: int = 0
    lighting: list = field(default_factory=list)
    chunk_versions: dict = field(default_factory=dict)

    def height_at(self, x: int, y: int) -> float:
        return self.heights[y * self.width + x] * MAP_HEIGHT_SCALE

    @property
    def waypoints(self) -> dict[str, MapObject]:
        return {o.props["waypointName"]: o for o in self.objects if "waypointName" in o.props}


class Reader:
    def __init__(self, data: bytes, names: dict[int, str]):
        self.d, self.p, self.names = data, 0, names

    def i32(self): v = struct.unpack_from("<i", self.d, self.p)[0]; self.p += 4; return v
    def u32(self): v = struct.unpack_from("<I", self.d, self.p)[0]; self.p += 4; return v
    def u16(self): v = struct.unpack_from("<H", self.d, self.p)[0]; self.p += 2; return v
    def f32(self): v = struct.unpack_from("<f", self.d, self.p)[0]; self.p += 4; return v
    def u8(self): v = self.d[self.p]; self.p += 1; return v
    def raw(self, n): v = self.d[self.p:self.p + n]; self.p += n; return v

    def astr(self):
        n = self.u16()
        return self.raw(n).decode("latin-1")

    def ustr(self):
        n = self.u16()
        return self.raw(n * 2).decode("utf-16-le")

    def dict(self):
        n = self.u16()
        out = {}
        for _ in range(n):
            kt = self.i32()
            t, key = kt & 0xFF, self.names.get(kt >> 8, f"#{kt >> 8}")
            if t == DICT_BOOL: out[key] = bool(self.u8())
            elif t == DICT_INT: out[key] = self.i32()
            elif t == DICT_REAL: out[key] = self.f32()
            elif t == DICT_ASCIISTRING: out[key] = self.astr()
            elif t == DICT_UNICODESTRING: out[key] = self.ustr()
            else: raise ValueError(f"bad dict type {t}")
        return out

    def chunks(self, end):
        while self.p + 10 <= end:
            cid = self.u32(); ver = self.u16(); size = self.i32()
            start = self.p
            yield self.names.get(cid, f"#{cid}"), ver, start, start + size
            self.p = start + size


def parse(data: bytes) -> GenMap:
    data = decompress(data)
    if data[:4] != b"CkMp":
        raise ValueError("not a CkMp map")
    p = 4
    count = struct.unpack_from("<i", data, p)[0]; p += 4
    names = {}
    for _ in range(count):
        n = data[p]; p += 1
        name = data[p:p + n].decode("latin-1"); p += n
        names[struct.unpack_from("<I", data, p)[0]] = name; p += 4
    r = Reader(data, names)
    r.p = p
    m = GenMap()
    for name, ver, s, e in r.chunks(len(data)):
        m.chunk_versions[name] = ver
        r.p = s
        if name == "HeightMapData":
            _height_map(r, ver, m)
        elif name == "BlendTileData":
            _blend_tiles(r, ver, m)
        elif name == "ObjectsList":
            for cname, cver, cs, ce in r.chunks(e):
                if cname == "Object":
                    r.p = cs
                    x, y, z, ang = r.f32(), r.f32(), r.f32(), r.f32()
                    flags = r.i32()
                    oname = r.astr()
                    props = r.dict() if cver >= 2 else {}
                    if cver <= 2:
                        z = 0.0
                    m.objects.append(MapObject(x, y, z, ang, flags, oname, props))
        elif name == "WorldInfo":
            m.world = r.dict()
        elif name == "SidesList":
            _sides(r, ver, m, e)
        elif name == "GlobalLighting":
            _lighting(r, ver, m, e)
    return m


def _height_map(r: Reader, ver: int, m: GenMap):
    m.width, m.height = r.i32(), r.i32()
    m.border = r.i32() if ver >= 3 else 0
    if ver >= 4:
        n = r.i32()
        m.boundaries = [(r.i32(), r.i32()) for _ in range(n)]
    else:
        m.boundaries = [(m.width - 2 * m.border, m.height - 2 * m.border)]
    size = r.i32()
    assert size == m.width * m.height, "height map size mismatch"
    m.heights = r.raw(size)


def _blend_tiles(r: Reader, ver: int, m: GenMap):
    n = r.i32()
    assert n == m.width * m.height
    shorts = lambda: list(struct.unpack_from(f"<{n}h", r.raw(n * 2)))
    m.tile_ndx = shorts()
    m.blend_ndx = shorts()
    m.extra_blend_ndx = shorts() if ver >= 6 else [0] * n
    m.cliff_ndx = shorts() if ver >= 5 else [0] * n
    if ver >= 7:
        flip_w = (m.width + 7) // 8
        byte_w = (m.width + 1) // 8 if ver == 7 else flip_w
        r.raw(m.height * byte_w)       # passability (cliff) bit flags
    m.num_bitmap_tiles = r.i32()
    num_blended = r.i32()
    num_cliff = r.i32() if ver >= 5 else 1
    for _ in range(r.i32()):
        first, num, width, _legacy = r.i32(), r.i32(), r.i32(), r.i32()
        m.texture_classes.append(TextureClass(first, num, width, r.astr()))
    if ver >= 4:
        _num_edge_tiles = r.i32()
        for _ in range(r.i32()):
            first, num, width = r.i32(), r.i32(), r.i32()
            m.edge_texture_classes.append(TextureClass(first, num, width, r.astr()))
    m.blended_tiles = [None]
    for _ in range(1, num_blended):
        bt = BlendedTile(r.i32(), r.u8(), r.u8(), r.u8(), r.u8(), r.u8(),
                         r.u8() if ver >= 3 else 0, r.i32() if ver >= 4 else -1)
        flag = r.i32()
        assert flag == 0x7ADA0000, f"bad blend flag {flag:x}"
        m.blended_tiles.append(bt)
    m.cliff_info = [None]
    if ver >= 5:
        for _ in range(1, num_cliff):
            ti = r.i32()
            uv = tuple(r.f32() for _ in range(8))
            m.cliff_info.append(CliffInfo(ti, uv, r.u8(), r.u8()))


def _sides(r: Reader, ver: int, m: GenMap, end: int):
    for _ in range(r.i32()):
        props = r.dict()
        build = []
        for _ in range(r.i32()):
            b = {"building": r.astr(), "template": r.astr(), "x": r.f32(), "y": r.f32(), "z": r.f32(),
                 "angle": r.f32(), "initiallyBuilt": r.u8(), "numRebuilds": r.i32()}
            if ver >= 3:
                b.update(script=r.astr(), health=r.i32(), whiner=r.u8(), unsellable=r.u8(), repairable=r.u8())
            build.append(b)
        m.sides.append(Side(props, build))
    if ver >= 2:
        m.teams = [r.dict() for _ in range(r.i32())]
    # remaining: PlayerScriptsList sub-chunks (scripts) - not needed for skirmish rendering


def _lighting(r: Reader, ver: int, m: GenMap, end: int):
    m.time_of_day = r.i32()
    light = lambda: {"ambient": (r.f32(), r.f32(), r.f32()), "diffuse": (r.f32(), r.f32(), r.f32()),
                     "dir": (r.f32(), r.f32(), r.f32())}
    for _ in range(4):
        tod = {"terrain": [light()], "objects": [light()]}
        if ver >= 2:
            tod["objects"] += [light(), light()]
        if ver >= 3:
            tod["terrain"] += [light(), light()]
        m.lighting.append(tod)


if __name__ == "__main__":
    import sys, time, collections
    from gen_vfs import GeneralsVFS

    vfs = GeneralsVFS()
    maps = sorted(vfs.glob(r"maps\*.map"), key=lambda e: e.path.lower())
    t0 = time.time()
    bad = 0
    tex = collections.Counter()
    for e in maps:
        try:
            m = parse(vfs.read(e.path))
        except Exception as ex:
            bad += 1
            print(f"ERR {e.path}: {ex!r}")
            continue
        for tc in m.texture_classes:
            tex[tc.name] += 1
        if len(sys.argv) > 1 and sys.argv[1].lower() in e.path.lower():
            print(e.path, f"{m.width}x{m.height} border={m.border} bounds={m.boundaries} objects={len(m.objects)}"
                  f" sides={len(m.sides)} tod={m.time_of_day} versions={m.chunk_versions}")
            print("  world:", m.world)
            print("  texture classes:", [(t.name, t.first_tile, t.num_tiles, t.width) for t in m.texture_classes])
            print("  waypoints:", sorted(m.waypoints))
            print("  sides:", [s.props.get("playerName") for s in m.sides])
            print("  blended tiles:", len(m.blended_tiles) - 1, "cliff infos:", len(m.cliff_info) - 1)
            obj = collections.Counter(o.name for o in m.objects)
            print("  objects:", obj.most_common(25))
    print(f"{len(maps)} maps, {bad} errors, {time.time()-t0:.1f}s; {len(tex)} distinct terrain classes")

"""
W3D (Westwood 3D) reader for Command & Conquer Generals / Zero Hour.

Format reference: EA's GPL source release,
  GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/w3d_file.h  (chunk ids + structs)
  .../WW3D2/htree.cpp    (pivot base transform = T(translation) * R(rotation))
  .../WW3D2/motchan.cpp  (raw / time-coded / adaptive-delta animation channels)

Every chunk is { u32 type, u32 size } little-endian. The MSB of size marks a
chunk that contains sub-chunks; the low 31 bits are the payload length.

A single .w3d file may hold any mix of: meshes, one hierarchy (HTree),
animations, an HLOD (the "model" that binds meshes to bones), boxes.
"""
from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field

# ---- chunk ids (w3d_file.h) -------------------------------------------------
MESH = 0x000
VERTICES = 0x002
VERTEX_NORMALS = 0x003
MESH_USER_TEXT = 0x00C
VERTEX_INFLUENCES = 0x00E
MESH_HEADER3 = 0x01F
TRIANGLES = 0x020
VERTEX_SHADE_INDICES = 0x022
PRELIT_UNLIT = 0x023
PRELIT_VERTEX = 0x024
PRELIT_LIGHTMAP_MULTI_PASS = 0x025
PRELIT_LIGHTMAP_MULTI_TEXTURE = 0x026
MATERIAL_INFO = 0x028
SHADERS = 0x029
VERTEX_MATERIALS = 0x02A
VERTEX_MATERIAL = 0x02B
VERTEX_MATERIAL_NAME = 0x02C
VERTEX_MATERIAL_INFO = 0x02D
VERTEX_MAPPER_ARGS0 = 0x02E
VERTEX_MAPPER_ARGS1 = 0x02F
TEXTURES = 0x030
TEXTURE = 0x031
TEXTURE_NAME = 0x032
TEXTURE_INFO = 0x033
MATERIAL_PASS = 0x038
VERTEX_MATERIAL_IDS = 0x039
SHADER_IDS = 0x03A
DCG = 0x03B
DIG = 0x03C
SCG = 0x03E
TEXTURE_STAGE = 0x048
TEXTURE_IDS = 0x049
STAGE_TEXCOORDS = 0x04A
PER_FACE_TEXCOORD_IDS = 0x04B
AABTREE = 0x090

HIERARCHY = 0x100
HIERARCHY_HEADER = 0x101
PIVOTS = 0x102
PIVOT_FIXUPS = 0x103

ANIMATION = 0x200
ANIMATION_HEADER = 0x201
ANIMATION_CHANNEL = 0x202
BIT_CHANNEL = 0x203

COMPRESSED_ANIMATION = 0x280
COMPRESSED_ANIMATION_HEADER = 0x281
COMPRESSED_ANIMATION_CHANNEL = 0x282
COMPRESSED_BIT_CHANNEL = 0x283

HLOD = 0x700
HLOD_HEADER = 0x701
HLOD_LOD_ARRAY = 0x702
HLOD_SUB_OBJECT_ARRAY_HEADER = 0x703
HLOD_SUB_OBJECT = 0x704
HLOD_AGGREGATE_ARRAY = 0x705
HLOD_PROXY_ARRAY = 0x706

BOX = 0x740

# mesh attribute flags
MESH_FLAG_HIDDEN = 0x00001000
MESH_FLAG_TWO_SIDED = 0x00002000
MESH_FLAG_CAST_SHADOW = 0x00008000
MESH_FLAG_GEOMETRY_TYPE_MASK = 0x00FF0000
MESH_GEOMETRY_NORMAL = 0x00000000
MESH_GEOMETRY_CAMERA_ALIGNED = 0x00010000
MESH_GEOMETRY_SKIN = 0x00020000
MESH_GEOMETRY_CAMERA_ORIENTED = 0x00060000

# animation channel types
CH_X, CH_Y, CH_Z, CH_XR, CH_YR, CH_ZR, CH_Q = range(7)
CH_TC_X, CH_TC_Y, CH_TC_Z, CH_TC_Q = 7, 8, 9, 10
CH_AD_X, CH_AD_Y, CH_AD_Z, CH_AD_Q = 11, 12, 13, 14

ANIM_FLAVOR_TIMECODED = 0
ANIM_FLAVOR_ADAPTIVE_DELTA = 1

TIMECODED_BINARY_FLAG = 0x80000000


# ---- low level helpers ------------------------------------------------------
def _cstr(b: bytes) -> str:
    i = b.find(b"\0")
    return (b if i < 0 else b[:i]).decode("latin-1")


def iter_chunks(data: bytes, start: int = 0, end: int | None = None):
    """Yields (chunk_type, payload_start, payload_end, has_subchunks)."""
    end = len(data) if end is None else end
    pos = start
    while pos + 8 <= end:
        ctype, csize = struct.unpack_from("<II", data, pos)
        sub = bool(csize & 0x80000000)
        size = csize & 0x7FFFFFFF
        pstart = pos + 8
        pend = pstart + size
        if pend > end:  # truncated / garbage -> stop
            break
        yield ctype, pstart, pend, sub
        pos = pend


# ---- data model -------------------------------------------------------------
@dataclass
class Pivot:
    name: str
    parent: int
    translation: tuple[float, float, float]
    rotation: tuple[float, float, float, float]  # x, y, z, w


@dataclass
class Hierarchy:
    name: str
    center: tuple[float, float, float]
    pivots: list[Pivot] = field(default_factory=list)


@dataclass
class Shader:
    depth_compare: int
    depth_mask: int
    dest_blend: int
    pri_gradient: int
    sec_gradient: int
    src_blend: int
    texturing: int
    detail_color_func: int
    detail_alpha_func: int
    alpha_test: int

    @property
    def blend_mode(self) -> str:
        """Collapse D3D blend factors into the handful of modes Generals actually uses."""
        s, d = self.src_blend, self.dest_blend
        if s == 1 and d == 0:
            return "opaque"
        if s == 2 and d == 5:          # SRC_ALPHA, ONE_MINUS_SRC_ALPHA
            return "alpha"
        if s == 1 and d == 1:          # ONE, ONE
            return "additive"
        if s == 2 and d == 1:          # SRC_ALPHA, ONE
            return "additive_alpha"
        if s == 0 and d == 2:          # ZERO, SRC_COLOR
            return "multiply"
        if s == 1 and d == 5:          # ONE, ONE_MINUS_SRC_ALPHA (premultiplied)
            return "alpha"
        return f"other_{s}_{d}"


@dataclass
class VertexMaterial:
    name: str
    attributes: int = 0
    ambient: tuple = (255, 255, 255)
    diffuse: tuple = (255, 255, 255)
    specular: tuple = (0, 0, 0)
    emissive: tuple = (0, 0, 0)
    shininess: float = 1.0
    opacity: float = 1.0
    translucency: float = 0.0
    mapper_args0: str = ""
    mapper_args1: str = ""


@dataclass
class TextureDef:
    name: str
    attributes: int = 0
    anim_type: int = 0
    frame_count: int = 1
    frame_rate: float = 0.0


@dataclass
class TextureStage:
    texture_ids: list[int] = field(default_factory=list)   # 1 entry (whole mesh) or one per triangle
    texcoords: list[tuple[float, float]] = field(default_factory=list)
    per_face_texcoord_ids: list[tuple[int, int, int]] = field(default_factory=list)


@dataclass
class MaterialPass:
    vertex_material_ids: list[int] = field(default_factory=list)  # 1 or per-vertex
    shader_ids: list[int] = field(default_factory=list)           # 1 or per-triangle
    dcg: list[tuple[int, int, int, int]] = field(default_factory=list)  # per-vertex diffuse color
    stages: list[TextureStage] = field(default_factory=list)


@dataclass
class Mesh:
    name: str                 # mesh name (without container)
    container: str
    attributes: int
    sort_level: int
    vertices: list[tuple[float, float, float]] = field(default_factory=list)
    normals: list[tuple[float, float, float]] = field(default_factory=list)
    triangles: list[tuple[int, int, int]] = field(default_factory=list)
    tri_attributes: list[int] = field(default_factory=list)
    influences: list[int] = field(default_factory=list)  # bone index per vertex (skins)
    shade_indices: list[int] = field(default_factory=list)
    user_text: str = ""
    vertex_materials: list[VertexMaterial] = field(default_factory=list)
    shaders: list[Shader] = field(default_factory=list)
    textures: list[TextureDef] = field(default_factory=list)
    passes: list[MaterialPass] = field(default_factory=list)
    bbox_min: tuple = (0, 0, 0)
    bbox_max: tuple = (0, 0, 0)

    @property
    def full_name(self) -> str:
        return f"{self.container}.{self.name}" if self.container else self.name

    @property
    def geometry_type(self) -> int:
        return self.attributes & MESH_FLAG_GEOMETRY_TYPE_MASK

    @property
    def is_skin(self) -> bool:
        return self.geometry_type == MESH_GEOMETRY_SKIN

    @property
    def is_hidden(self) -> bool:
        return bool(self.attributes & MESH_FLAG_HIDDEN)

    @property
    def two_sided(self) -> bool:
        return bool(self.attributes & MESH_FLAG_TWO_SIDED)


@dataclass
class Box:
    name: str
    attributes: int
    color: tuple
    center: tuple
    extent: tuple


@dataclass
class HLodSubObject:
    bone: int
    name: str     # "CONTAINER.MESH"


@dataclass
class HLod:
    name: str
    hierarchy: str
    lods: list[tuple[float, list[HLodSubObject]]] = field(default_factory=list)  # (max_screen_size, objects), lowest detail first
    aggregates: list[HLodSubObject] = field(default_factory=list)
    proxies: list[HLodSubObject] = field(default_factory=list)


@dataclass
class Channel:
    pivot: int
    kind: str              # "x" | "y" | "z" | "q" | "vis"
    sample: object         # callable(frame:int) -> float | tuple | bool
    first: int = 0
    last: int = 0


@dataclass
class Animation:
    name: str
    hierarchy: str
    num_frames: int
    frame_rate: float
    channels: list[Channel] = field(default_factory=list)


@dataclass
class W3DFile:
    meshes: list[Mesh] = field(default_factory=list)
    hierarchy: Hierarchy | None = None
    animations: list[Animation] = field(default_factory=list)
    hlod: HLod | None = None
    boxes: list[Box] = field(default_factory=list)
    unknown_chunks: dict[int, int] = field(default_factory=dict)


# ---- parsing ----------------------------------------------------------------
def parse(data: bytes) -> W3DFile:
    out = W3DFile()
    for ctype, s, e, _sub in iter_chunks(data):
        if ctype == MESH:
            out.meshes.append(_parse_mesh(data, s, e))
        elif ctype == HIERARCHY:
            out.hierarchy = _parse_hierarchy(data, s, e)
        elif ctype == ANIMATION:
            out.animations.append(_parse_animation(data, s, e))
        elif ctype == COMPRESSED_ANIMATION:
            out.animations.append(_parse_compressed_animation(data, s, e))
        elif ctype == HLOD:
            out.hlod = _parse_hlod(data, s, e)
        elif ctype == BOX:
            out.boxes.append(_parse_box(data, s, e))
        else:
            out.unknown_chunks[ctype] = out.unknown_chunks.get(ctype, 0) + 1
    return out


def _vec3_array(data, s, e):
    n = (e - s) // 12
    f = struct.unpack_from(f"<{n * 3}f", data, s)
    return [f[i:i + 3] for i in range(0, n * 3, 3)]


def _u32_array(data, s, e):
    n = (e - s) // 4
    return list(struct.unpack_from(f"<{n}I", data, s))


def _parse_mesh(data, s, e) -> Mesh:
    mesh = Mesh("", "", 0, 0)
    for ct, cs, ce, _ in iter_chunks(data, s, e):
        if ct == MESH_HEADER3:
            (ver, attrs) = struct.unpack_from("<II", data, cs)
            mesh.attributes = attrs
            mesh.name = _cstr(data[cs + 8:cs + 24])
            mesh.container = _cstr(data[cs + 24:cs + 40])
            (ntri, nvert, nmat, ndmg, sort_level, prelit, _fut, vch, fch) = struct.unpack_from("<IIIIiIIII", data, cs + 40)
            mesh.sort_level = sort_level
            mn = struct.unpack_from("<3f", data, cs + 76)
            mx = struct.unpack_from("<3f", data, cs + 88)
            mesh.bbox_min, mesh.bbox_max = mn, mx
        elif ct == VERTICES:
            mesh.vertices = _vec3_array(data, cs, ce)
        elif ct == VERTEX_NORMALS:
            mesh.normals = _vec3_array(data, cs, ce)
        elif ct == MESH_USER_TEXT:
            mesh.user_text = _cstr(data[cs:ce])
        elif ct == VERTEX_INFLUENCES:
            n = (ce - cs) // 8
            mesh.influences = [struct.unpack_from("<H", data, cs + i * 8)[0] for i in range(n)]
        elif ct == TRIANGLES:
            n = (ce - cs) // 32
            for i in range(n):
                a, b, c, attr = struct.unpack_from("<4I", data, cs + i * 32)
                mesh.triangles.append((a, b, c))
                mesh.tri_attributes.append(attr)
        elif ct == VERTEX_SHADE_INDICES:
            mesh.shade_indices = _u32_array(data, cs, ce)
        elif ct == MATERIAL_INFO:
            pass
        elif ct == SHADERS:
            n = (ce - cs) // 16
            for i in range(n):
                b = data[cs + i * 16: cs + i * 16 + 16]
                mesh.shaders.append(Shader(
                    depth_compare=b[0], depth_mask=b[1], dest_blend=b[3], pri_gradient=b[5],
                    sec_gradient=b[6], src_blend=b[7], texturing=b[8], detail_color_func=b[9],
                    detail_alpha_func=b[10], alpha_test=b[12]))
        elif ct == VERTEX_MATERIALS:
            for vt, vs, ve, _ in iter_chunks(data, cs, ce):
                if vt == VERTEX_MATERIAL:
                    mesh.vertex_materials.append(_parse_vertex_material(data, vs, ve))
        elif ct == TEXTURES:
            for tt, ts, te, _ in iter_chunks(data, cs, ce):
                if tt == TEXTURE:
                    mesh.textures.append(_parse_texture(data, ts, te))
        elif ct == MATERIAL_PASS:
            mesh.passes.append(_parse_material_pass(data, cs, ce))
    return mesh


def _parse_vertex_material(data, s, e) -> VertexMaterial:
    vm = VertexMaterial("")
    for ct, cs, ce, _ in iter_chunks(data, s, e):
        if ct == VERTEX_MATERIAL_NAME:
            vm.name = _cstr(data[cs:ce])
        elif ct == VERTEX_MATERIAL_INFO:
            vm.attributes = struct.unpack_from("<I", data, cs)[0]
            rgb = lambda o: tuple(data[cs + o:cs + o + 3])
            vm.ambient, vm.diffuse, vm.specular, vm.emissive = rgb(4), rgb(8), rgb(12), rgb(16)
            vm.shininess, vm.opacity, vm.translucency = struct.unpack_from("<3f", data, cs + 20)
        elif ct == VERTEX_MAPPER_ARGS0:
            vm.mapper_args0 = _cstr(data[cs:ce])
        elif ct == VERTEX_MAPPER_ARGS1:
            vm.mapper_args1 = _cstr(data[cs:ce])
    return vm


def _parse_texture(data, s, e) -> TextureDef:
    td = TextureDef("")
    for ct, cs, ce, _ in iter_chunks(data, s, e):
        if ct == TEXTURE_NAME:
            td.name = _cstr(data[cs:ce])
        elif ct == TEXTURE_INFO:
            td.attributes, td.anim_type, td.frame_count, td.frame_rate = struct.unpack_from("<HHIf", data, cs)
    return td


def _parse_material_pass(data, s, e) -> MaterialPass:
    mp = MaterialPass()
    for ct, cs, ce, _ in iter_chunks(data, s, e):
        if ct == VERTEX_MATERIAL_IDS:
            mp.vertex_material_ids = _u32_array(data, cs, ce)
        elif ct == SHADER_IDS:
            mp.shader_ids = _u32_array(data, cs, ce)
        elif ct == DCG:
            n = (ce - cs) // 4
            mp.dcg = [tuple(data[cs + i * 4:cs + i * 4 + 4]) for i in range(n)]
        elif ct == TEXTURE_STAGE:
            st = TextureStage()
            for tt, ts, te, _ in iter_chunks(data, cs, ce):
                if tt == TEXTURE_IDS:
                    st.texture_ids = _u32_array(data, ts, te)
                elif tt == STAGE_TEXCOORDS:
                    n = (te - ts) // 8
                    f = struct.unpack_from(f"<{n * 2}f", data, ts)
                    st.texcoords = [f[i:i + 2] for i in range(0, n * 2, 2)]
                elif tt == PER_FACE_TEXCOORD_IDS:
                    n = (te - ts) // 12
                    f = struct.unpack_from(f"<{n * 3}I", data, ts)
                    st.per_face_texcoord_ids = [f[i:i + 3] for i in range(0, n * 3, 3)]
            mp.stages.append(st)
    return mp


def _parse_hierarchy(data, s, e) -> Hierarchy:
    h = None
    for ct, cs, ce, _ in iter_chunks(data, s, e):
        if ct == HIERARCHY_HEADER:
            _ver = struct.unpack_from("<I", data, cs)[0]
            name = _cstr(data[cs + 4:cs + 20])
            _num = struct.unpack_from("<I", data, cs + 20)[0]
            center = struct.unpack_from("<3f", data, cs + 24)
            h = Hierarchy(name, center)
        elif ct == PIVOTS:
            n = (ce - cs) // 60
            for i in range(n):
                o = cs + i * 60
                name = _cstr(data[o:o + 16])
                parent = struct.unpack_from("<I", data, o + 16)[0]
                t = struct.unpack_from("<3f", data, o + 20)
                # o+32: euler angles (unused by the runtime)
                q = struct.unpack_from("<4f", data, o + 44)
                h.pivots.append(Pivot(name, -1 if parent == 0xFFFFFFFF else parent, t, q))
    return h


def _parse_hlod(data, s, e) -> HLod:
    h = None

    def read_array(cs, ce):
        size = 0.0
        objs = []
        for at, as_, ae, _ in iter_chunks(data, cs, ce):
            if at == HLOD_SUB_OBJECT_ARRAY_HEADER:
                _count, size = struct.unpack_from("<If", data, as_)
            elif at == HLOD_SUB_OBJECT:
                bone = struct.unpack_from("<I", data, as_)[0]
                objs.append(HLodSubObject(bone, _cstr(data[as_ + 4:as_ + 36])))
        return size, objs

    for ct, cs, ce, _ in iter_chunks(data, s, e):
        if ct == HLOD_HEADER:
            _ver, _lods = struct.unpack_from("<II", data, cs)
            h = HLod(_cstr(data[cs + 8:cs + 24]), _cstr(data[cs + 24:cs + 40]))
        elif ct == HLOD_LOD_ARRAY:
            h.lods.append(read_array(cs, ce))
        elif ct == HLOD_AGGREGATE_ARRAY:
            h.aggregates.extend(read_array(cs, ce)[1])
        elif ct == HLOD_PROXY_ARRAY:
            h.proxies.extend(read_array(cs, ce)[1])
    return h


def _parse_box(data, s, e) -> Box:
    _ver, attrs = struct.unpack_from("<II", data, s)
    name = _cstr(data[s + 8:s + 40])
    color = tuple(data[s + 40:s + 43])
    center = struct.unpack_from("<3f", data, s + 44)
    extent = struct.unpack_from("<3f", data, s + 56)
    return Box(name, attrs, color, center, extent)


# ---- animations ---------------------------------------------------------------
_KIND = {CH_X: "x", CH_Y: "y", CH_Z: "z", CH_Q: "q",
         CH_TC_X: "x", CH_TC_Y: "y", CH_TC_Z: "z", CH_TC_Q: "q",
         CH_AD_X: "x", CH_AD_Y: "y", CH_AD_Z: "z", CH_AD_Q: "q"}


def _identity(kind):
    return (0.0, 0.0, 0.0, 1.0) if kind == "q" else 0.0


def _parse_animation(data, s, e) -> Animation:
    anim = None
    for ct, cs, ce, _ in iter_chunks(data, s, e):
        if ct == ANIMATION_HEADER:
            _ver = struct.unpack_from("<I", data, cs)[0]
            name = _cstr(data[cs + 4:cs + 20])
            hier = _cstr(data[cs + 20:cs + 36])
            nframes, rate = struct.unpack_from("<II", data, cs + 36)
            anim = Animation(name, hier, nframes, float(rate))
        elif ct == ANIMATION_CHANNEL:
            first, last, vlen, flags, pivot = struct.unpack_from("<5H", data, cs)
            kind = _KIND.get(flags)
            if kind is None:      # euler channels (XR/YR/ZR) are never used by the runtime
                continue
            count = (last - first + 1) * vlen
            vals = struct.unpack_from(f"<{count}f", data, cs + 12)
            anim.channels.append(Channel(pivot, kind, _raw_sampler(vals, first, last, vlen, kind), first, last))
        elif ct == BIT_CHANNEL:
            first, last, flags, pivot = struct.unpack_from("<4H", data, cs)
            default = data[cs + 8]
            bits = data[cs + 9:ce]
            anim.channels.append(Channel(pivot, "vis", _bit_sampler(bits, first, last, default), first, last))
    return anim


def _raw_sampler(vals, first, last, vlen, kind):
    def sample(frame: int):
        if frame < first or frame > last:
            return _identity(kind)
        i = (frame - first) * vlen
        return tuple(vals[i:i + 4]) if vlen == 4 else vals[i]
    return sample


def _bit_sampler(bits, first, last, default):
    def sample(frame: int) -> bool:
        if frame < first or frame > last:
            return bool(default)
        b = frame - first
        return bool(bits[b // 8] & (1 << (b % 8)))
    return sample


def _parse_compressed_animation(data, s, e) -> Animation:
    anim = None
    flavor = ANIM_FLAVOR_TIMECODED
    for ct, cs, ce, _ in iter_chunks(data, s, e):
        if ct == COMPRESSED_ANIMATION_HEADER:
            _ver = struct.unpack_from("<I", data, cs)[0]
            name = _cstr(data[cs + 4:cs + 20])
            hier = _cstr(data[cs + 20:cs + 36])
            nframes, rate, flavor = struct.unpack_from("<IHH", data, cs + 36)
            anim = Animation(name, hier, nframes, float(rate))
        elif ct == COMPRESSED_ANIMATION_CHANNEL:
            if flavor == ANIM_FLAVOR_TIMECODED:
                num_tc, pivot, vlen, flags = struct.unpack_from("<IHBB", data, cs)
                kind = _KIND.get(flags)
                if kind is None:
                    continue
                anim.channels.append(Channel(pivot, kind, _timecoded_sampler(data, cs + 8, num_tc, vlen, kind)))
            else:
                nframes, pivot, vlen, flags, scale = struct.unpack_from("<IHBBf", data, cs)
                kind = _KIND.get(flags)
                if kind is None:
                    continue
                anim.channels.append(Channel(pivot, kind, _adaptive_delta_sampler(data, cs + 12, ce, nframes, vlen, scale, kind)))
        elif ct == COMPRESSED_BIT_CHANNEL:
            num_tc, pivot, flags, default = struct.unpack_from("<IHBB", data, cs)
            codes = struct.unpack_from(f"<{num_tc}I", data, cs + 8)
            anim.channels.append(Channel(pivot, "vis", _timecoded_bit_sampler(codes, default)))
    return anim


def _timecoded_sampler(data, off, num_tc, vlen, kind):
    keys = []
    pk = vlen + 1
    for i in range(num_tc):
        o = off + i * pk * 4
        t = struct.unpack_from("<I", data, o)[0]
        v = struct.unpack_from(f"<{vlen}f", data, o + 4)
        keys.append((t & ~TIMECODED_BINARY_FLAG, bool(t & TIMECODED_BINARY_FLAG), v))

    def sample(frame: int):
        # mirrors TimeCodedMotionChannelClass::Get_Vector at integer frames
        idx = 0
        for i, (t, _b, _v) in enumerate(keys):
            if t <= frame:
                idx = i
            else:
                break
        t0, _, v0 = keys[idx]
        if idx == len(keys) - 1 or frame <= t0:
            v = v0
        else:
            t1, binary, v1 = keys[idx + 1]
            if binary:
                v = v0
            else:
                r = (frame - t0) / (t1 - t0)
                v = tuple(a + (b - a) * r for a, b in zip(v0, v1))
                if kind == "q":
                    v = _slerp(v0, v1, r)
        return tuple(v) if vlen == 4 else v[0]
    return sample


def _timecoded_bit_sampler(codes, default):
    def sample(frame: int) -> bool:
        val = bool(default)
        for c in codes:
            if (c & ~TIMECODED_BINARY_FLAG) <= frame:
                val = bool(c & TIMECODED_BINARY_FLAG)
            else:
                break
        return val
    return sample


_FILTER_TABLE = [1e-8, 1e-7, 1e-6, 1e-5, 1e-4, 1e-3, 1e-2, 1e-1, 1.0, 10.0, 100.0, 1e3, 1e4, 1e5, 1e6, 1e7]
_FILTER_TABLE += [1.0 - math.sin(math.radians(90.0 * (i / 240.0))) for i in range(240)]


def _adaptive_delta_sampler(data, off, end, nframes, vlen, scale, kind):
    """AdaptiveDeltaMotionChannelClass::decompress, fully expanded once."""
    raw = data[off:end]
    base = struct.unpack_from(f"<{vlen}f", raw, 0)
    frames = [list(base)]
    for vi in range(vlen):
        p = vlen * 4 + 9 * vi
        last = base[vi]
        frame = 1
        while frame < nframes:
            filt = _FILTER_TABLE[raw[p]] * scale
            p += 1
            for fi in range(16):
                byte = raw[p + (fi >> 1)]
                nyb = (byte >> 4) if (fi & 1) else (byte & 0xF)
                if nyb & 0x8:
                    nyb -= 16
                last += nyb * filt
                if vi == 0:
                    frames.append([0.0] * vlen)
                if frame < len(frames):
                    frames[frame][vi] = last
                frame += 1
                if frame >= nframes:
                    break
            p += 9 * vlen - 1
    frames = frames[:nframes]

    def sample(frame: int):
        f = frames[min(max(frame, 0), len(frames) - 1)]
        return tuple(f) if vlen == 4 else f[0]
    return sample


def _slerp(q0, q1, t):
    dot = sum(a * b for a, b in zip(q0, q1))
    if dot < 0:
        q1 = tuple(-c for c in q1)
        dot = -dot
    if dot > 0.9995:
        r = tuple(a + (b - a) * t for a, b in zip(q0, q1))
    else:
        th = math.acos(dot)
        s0 = math.sin((1 - t) * th) / math.sin(th)
        s1 = math.sin(t * th) / math.sin(th)
        r = tuple(s0 * a + s1 * b for a, b in zip(q0, q1))
    n = math.sqrt(sum(c * c for c in r)) or 1.0
    return tuple(c / n for c in r)


# ---- debug dump ---------------------------------------------------------------
def describe(f: W3DFile) -> str:
    lines = []
    if f.hierarchy:
        h = f.hierarchy
        lines.append(f"HIERARCHY {h.name}: {len(h.pivots)} pivots")
        for i, p in enumerate(h.pivots):
            lines.append(f"  [{i:2}] {p.name:16} parent={p.parent:3} t=({p.translation[0]:.2f},{p.translation[1]:.2f},{p.translation[2]:.2f})")
    if f.hlod:
        lines.append(f"HLOD {f.hlod.name} hier={f.hlod.hierarchy} lods={len(f.hlod.lods)}")
        for li, (size, objs) in enumerate(f.hlod.lods):
            lines.append(f"  LOD{li} maxScreen={size:g}: " + ", ".join(f"{o.name}@{o.bone}" for o in objs))
        for o in f.hlod.aggregates:
            lines.append(f"  AGG {o.name}@{o.bone}")
        for o in f.hlod.proxies:
            lines.append(f"  PROXY {o.name}@{o.bone}")
    for m in f.meshes:
        tex = ",".join(t.name for t in m.textures)
        shaders = ",".join(s.blend_mode + ("/at" if s.alpha_test else "") for s in m.shaders)
        lines.append(f"MESH {m.full_name}: v={len(m.vertices)} t={len(m.triangles)} attr=0x{m.attributes:08X}"
                     f"{' SKIN' if m.is_skin else ''}{' HIDDEN' if m.is_hidden else ''}{' 2SIDED' if m.two_sided else ''}"
                     f" passes={len(m.passes)} tex=[{tex}] shaders=[{shaders}]")
    for b in f.boxes:
        lines.append(f"BOX {b.name} c={b.center} e={b.extent}")
    for a in f.animations:
        lines.append(f"ANIM {a.name} hier={a.hierarchy} frames={a.num_frames} fps={a.frame_rate} channels={len(a.channels)}")
    if f.unknown_chunks:
        lines.append("UNHANDLED " + ", ".join(f"0x{k:X}x{v}" for k, v in f.unknown_chunks.items()))
    return "\n".join(lines)


if __name__ == "__main__":
    import sys
    from gen_vfs import GeneralsVFS
    vfs = GeneralsVFS()
    for name in sys.argv[1:]:
        path = name if "\\" in name else f"Art\\W3D\\{name}.w3d"
        print(f"===== {path}")
        print(describe(parse(vfs.read(path))))

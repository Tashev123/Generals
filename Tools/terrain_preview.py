"""
Render a top-down preview of a map's terrain texturing (base tiles + blend layers),
following WorldHeightMap::readTiles / getUVForNdx / getAlphaUVData. Used to validate
the tile/UV rules before porting them to C++.

usage: .venv\\Scripts\\python terrain_preview.py "Tournament Desert" out.png [pixels_per_cell]
"""
from __future__ import annotations

import io
import sys

import numpy as np
from PIL import Image

import gen_ini
import gen_map
from gen_vfs import GeneralsVFS

TILE = 64
INVERTED_MASK = 0x1
FLIPPED_MASK = 0x2


def load_terrain_types(vfs) -> dict[str, str]:
    out = {}
    for b in gen_ini.parse(vfs.read(r"Data\INI\Terrain.ini").decode("latin-1")):
        if b.keyword.lower() == "terrain":
            e = b.get("Texture")
            if e and e.values:
                out[b.name.lower()] = e.values[0]
    return out


def tga_file_rows(data: bytes) -> np.ndarray:
    """RGBA pixels in *file row order* (what readTiles iterates), shape (h, w, 4)."""
    img = Image.open(io.BytesIO(data)).convert("RGBA")
    arr = np.asarray(img)
    descriptor = data[17]
    top_down = bool(descriptor & 0x20)
    return arr if top_down else arr[::-1]


def build_tiles(vfs, m: gen_map.GenMap, types: dict[str, str]) -> dict[int, np.ndarray]:
    tiles: dict[int, np.ndarray] = {}
    for tc in m.texture_classes:
        tex = types.get(tc.name.lower())
        if not tex:
            print("missing terrain type", tc.name)
            continue
        path = "Art\\Terrain\\" + tex
        if not vfs.exists(path):
            print("missing texture", path)
            continue
        px = tga_file_rows(vfs.read(path))
        rows = tc.width
        for ty in range(rows):
            for tx in range(rows):
                tile = px[ty * TILE:(ty + 1) * TILE, tx * TILE:(tx + 1) * TILE]
                if tile.shape[:2] == (TILE, TILE):
                    tiles[tc.first_tile + tx + rows * ty] = tile
    return tiles


def cell_image(tiles, tile_ndx: int, ppc: int) -> np.ndarray | None:
    """North-up image of one cell (a quarter tile)."""
    t = tiles.get(tile_ndx >> 2)
    if t is None:
        return None
    qx = tile_ndx & 1
    row0 = 0 if (tile_ndx & 2) else 32       # memory rows [minV, midV) when bit1 set
    quad = t[row0:row0 + 32, qx * 32:qx * 32 + 32]
    # memory row increases from north (V=min) to south (V=max) -> already north-up
    if ppc != 32:
        quad = np.asarray(Image.fromarray(quad).resize((ppc, ppc), Image.BILINEAR))
    return quad


def blend_alpha(bt: gen_map.BlendedTile) -> tuple[list[int], bool]:
    """getAlphaUVData: corner alphas (0:SW,1:SE,2:NE,3:NW) and diagonal flip."""
    a = [0, 0, 0, 0]
    flip = False
    inv = bt.inverted & INVERTED_MASK
    if bt.horiz:
        flip = bool(bt.inverted & FLIPPED_MASK)
        if inv: a[0] = a[3] = 255
        else: a[1] = a[2] = 255
    if bt.vert:
        flip = bool(bt.inverted & FLIPPED_MASK)
        if inv: a[0] = a[1] = 255
        else: a[2] = a[3] = 255
    if bt.right_diagonal:
        if inv:
            a[1] = 255
            if bt.long_diagonal: a[0] = a[2] = 255
        else:
            flip = True
            a[2] = 255
            if bt.long_diagonal: a[1] = a[3] = 255
    if bt.left_diagonal:
        if inv:
            flip = True
            a[0] = 255
            if bt.long_diagonal: a[1] = a[3] = 255
        else:
            a[3] = 255
            if bt.long_diagonal: a[0] = a[2] = 255
    if bt.custom_blend_edge_class >= 0:
        a = [0, 0, 0, 0]
        flip = False
    return a, flip


def alpha_mask(a: list[int], flip: bool, ppc: int) -> np.ndarray:
    """Interpolate 4 corner alphas across two triangles (north-up image)."""
    ys, xs = np.mgrid[0:ppc, 0:ppc].astype(np.float32)
    u = (xs + 0.5) / ppc              # 0 west .. 1 east
    v = 1.0 - (ys + 0.5) / ppc        # 0 south .. 1 north
    a0, a1, a2, a3 = (c / 255.0 for c in a)   # SW, SE, NE, NW
    if not flip:   # diagonal SW-NE: triangles (0,1,2) and (0,2,3)
        lower = u >= v
        r1 = a0 + (a1 - a0) * u + (a2 - a1) * v
        r2 = a0 + (a2 - a3) * u + (a3 - a0) * v
        return np.where(lower, r1, r2)
    # diagonal SE-NW: triangles (0,1,3) and (1,2,3)
    lower = (u + v) <= 1.0
    r1 = a0 + (a1 - a0) * u + (a3 - a0) * v
    r2 = a2 + (a3 - a2) * (1 - u) + (a1 - a2) * (1 - v)
    return np.where(lower, r1, r2)


def render(map_name: str, out: str, ppc: int = 8):
    vfs = GeneralsVFS()
    m = gen_map.parse(vfs.read(f"Maps\\{map_name}\\{map_name}.map"))
    types = load_terrain_types(vfs)
    tiles = build_tiles(vfs, m, types)
    w, h = m.width - 1, m.height - 1
    img = np.zeros((h * ppc, w * ppc, 3), np.float32)
    for y in range(h):
        py = (h - 1 - y) * ppc          # north up
        for x in range(w):
            i = y * m.width + x
            base = cell_image(tiles, m.tile_ndx[i], ppc)
            if base is None:
                continue
            c = base[..., :3].astype(np.float32)
            for layer in (m.blend_ndx[i], m.extra_blend_ndx[i]):
                if layer:
                    bt = m.blended_tiles[layer]
                    over = cell_image(tiles, bt.blend_ndx, ppc)
                    if over is not None:
                        a, flip = blend_alpha(bt)
                        k = alpha_mask(a, flip, ppc)[..., None]
                        c = c * (1 - k) + over[..., :3].astype(np.float32) * k
            img[py:py + ppc, x * ppc:(x + 1) * ppc] = c
    # shade by height for readability
    hm = np.frombuffer(m.heights, np.uint8).reshape(m.height, m.width).astype(np.float32)
    gy, gx = np.gradient(hm)
    shade = np.clip(1.0 - (gx - gy) * 0.04, 0.6, 1.3)[:h, :w][::-1]
    shade = np.kron(shade, np.ones((ppc, ppc)))[..., None]
    Image.fromarray(np.clip(img * shade, 0, 255).astype(np.uint8)).save(out)
    print(f"wrote {out}: {w}x{h} cells, {len(tiles)} tiles, {len(m.blended_tiles) - 1} blends")


if __name__ == "__main__":
    render(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 8)

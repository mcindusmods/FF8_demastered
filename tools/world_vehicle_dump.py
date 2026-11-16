#!/usr/bin/env python3
"""Write the world map textures with the file names WORLD_VEHICLES looks for.

Usage:
    python world_vehicle_dump.py "<...>\\DEMASTER_EXP\\data" out_folder [--test] [--extract]

Reads world.fi/.fl/.fs from the given folder (DEMASTER_EXP\\data after AUTOUNPACK, or any folder holding the three
files) and writes:
    out_folder/world/dat/wmset/section42/texture0.png     vehicles: Balamb Garden, Galbadia Garden, cars, trains, ...
    out_folder/world/dat/wmset/atlas/page13.png          effects, moon, water, sea, clouds, the select map, roads and
                                                          rails (wmset sections 38 and 39): one atlas per VRAM page
    out_folder/optional/world/dat/wmset/section38/texture0.png  far terrain (section 38 texture0-7, section 40): one file
                                                          each, kept apart because the wmset_016-019 pages cover it
    out_folder/world/esk/chara_one/model1.png            Ragnarok, Squall, chocobos, Zell, Selphie: one strip per model
plus out_folder/world_textures.csv: what each texture is, its size, its palettes, the VRAM spot written in the file and
which other textures share that spot. Upscale the PNGs, keep the names, put them in DEMASTER_EXP\\textures\\.

One file per texture, used for every palette of it. Vehicles (section 42) with several palettes are split into regions,
each only ever drawn with one of them (from the polygons of the vehicle's own model, wmset section 16), so the PNG
shows the object the way it looks in game. Page atlases are laid out like the VRAM page the
textures are meant for (256x256 texels: page12 select map, page13 moon and effects, page14 clouds, page28 sea,
page28_8bit water and shores, page29 roads and rails), each texture in its own colours. Where palettes recolour the
same texels (the sea, the select map's grey version, sparkles), palette 0 is in the atlas and palette N in
<atlas>_p<N>.png, which holds only the textures that have that palette. Water with a colour animation (section 41)
gets <atlas>_f1 .. _f5 the same way (frame 0 is in the atlas). These are optional and go to out_folder/optional:
copy the ones you make different next to the atlas; without them the atlas is used for every palette and frame.
A file named after one texture (world/dat/wmset/section38/texture9.png, also with _p<N> or _f<N>) wins over the
atlas for that texture.
chara.one: one strip per model, its textures left to right in file order (Ragnarok: 12+84+92+68 texels wide). The game
may put the pieces in different places; DEMASTER cuts the strip by their widths.
Exact copies get one file, the lower number: section 42 texture0/1, 15/16, 29/32 (same VRAM spot, drawn together:
Garden + ring; the file has both models' regions in it) and chara.one model2/model3 (chocobo).
--test writes 4x test versions instead: each palette region tinted its own colour, with a white border, so a replaced
texture is obvious in game and you can see the regions.
--extract also writes wmsetus.obj and chara.one (for tests/world_vehicles_test.cpp).
Pure Python, no packages needed.
"""
import argparse
import csv
import os
import re
import struct
import sys
import zlib

LABELS_42 = {
    0: "Balamb Garden", 1: "Balamb Garden (same pixels as texture0)", 2: "Train: blue locomotive", 3: "Train: blue wagon",
    4: "Train: green locomotive", 5: "Train: green wagon", 6: "Train: old locomotive", 7: "Train: old wagon",
    8: "Train: grey freight wagon", 9: "Car: yellow", 10: "Galbadian army car", 11: "Galbadian army car (second)",
    12: "unknown (16x16)", 13: "unknown (4 flat colours)", 14: "Vessel (boat to Dollet; Remastered's own wmset_014_0 has this number)",
    15: "Galbadia Garden", 16: "Galbadia Garden (same pixels as texture15)", 17: "Car: green", 18: "Car: brown",
    19: "Car: blue", 20: "Car: mint", 21: "Car: pink", 22: "Car: Esthar yellow", 23: "Car: Esthar blue",
    24: "White SeeD ship", 25: "Lunatic Pandora", 26: "Lunatic Pandora", 27: "Lunatic Pandora", 28: "Disc 4 doors",
    29: "City barrier (disc 4)", 30: "Jumbo Cactuar", 31: "Rocks", 32: "City barrier (disc 4, same pixels as texture29)",
}
LABELS_38 = {
    **{n: "far terrain, low-res copy (wmset_016-019 page files cover these)" for n in range(8)},
    8: "sea and shore (16 colour variants)", 9: "moon", 10: "clouds", 11: "select map / minimap (palette 1: grey)",
    12: "shadow", 13: "square", 14: "smoke puffs", 15: "sparkles (6 colour variants)",
    16: "open sea (colour animation)", 17: "sea (colour animation)", 18: "sea (colour animation)",
    19: "river / waterfall (colour animation)", 20: "water", 21: "shore", 22: "shore", 23: "water",
    24: "minimap view cone", 25: "minimap marker", 26: "dots", 27: "ring", 28: "spikes", 29: "spikes", 30: "dust",
    31: "ring", 32: "circles", 33: "bars", 34: "dark circle", 35: "horizon",
}
LABELS_39 = {**{n: "road / rail" for n in range(11)}, 11: "bridge", 12: "texture (64x64)"}
LABELS_40 = {0: "far terrain variant of section38 texture2"}
LABELS_CHARA = {0: "Squall", 1: "Ragnarok", 2: "Chocobo", 3: "Chocobo", 4: "Squall (student)", 5: "Zell", 6: "Selphie"}


def read_archive(folder):
    """Returns {file name: bytes} for wmset*.obj and chara.one from world.fi/.fl/.fs, or loose files."""
    files = {}
    fi, fl, fs = (os.path.join(folder, "world" + ext) for ext in (".fi", ".fl", ".fs"))
    if os.path.exists(fi) and os.path.exists(fl) and os.path.exists(fs):
        index = open(fi, "rb").read()
        names = [n.strip(b"\r\0 ").decode("latin-1") for n in open(fl, "rb").read().split(b"\n")]
        names = [n for n in names if n]
        with open(fs, "rb") as f:
            for i, name in enumerate(names[:len(index) // 12]):
                base = name.replace("\\", "/").rsplit("/", 1)[-1].lower()
                if not (base == "chara.one" or (base.startswith("wmset") and base.endswith(".obj"))):
                    continue
                size, offset, compression = struct.unpack_from("<III", index, i * 12)
                f.seek(offset)
                if compression == 0:
                    files[base] = f.read(size)
                elif compression == 1:
                    length = struct.unpack("<I", f.read(4))[0]
                    files[base] = lzss(f.read(length), size)
                elif compression == 2:
                    header = f.read(12)
                    declared = struct.unpack_from("<I", header)[0]
                    files[base] = lz4(f.read(declared - 8 if declared >= 8 else -1), size)
    for name in os.listdir(folder):
        lower = name.lower()
        if lower not in files and (lower == "chara.one" or (lower.startswith("wmset") and lower.endswith(".obj"))):
            files[lower] = open(os.path.join(folder, name), "rb").read()
    return files


def lzss(data, expected):
    window, r, flags, ip, out = bytearray(4096), 4078, 0, 0, bytearray()
    while ip < len(data) and len(out) < expected:
        flags >>= 1
        if not flags & 256:
            flags, ip = data[ip] | 0xFF00, ip + 1
            if ip >= len(data):
                break
        if flags & 1:
            out.append(data[ip])
            window[r], r, ip = data[ip], (r + 1) & 4095, ip + 1
        else:
            if ip + 1 >= len(data):
                break
            i, j = data[ip] | ((data[ip + 1] & 0xF0) << 4), (data[ip + 1] & 15) + 2
            ip += 2
            for k in range(j + 1):
                c = window[(i + k) & 4095]
                out.append(c)
                window[r], r = c, (r + 1) & 4095
    return bytes(out[:expected])


def lz4(data, expected):
    out, ip = bytearray(), 0
    while ip < len(data) and len(out) < expected:
        token = data[ip]
        ip += 1
        length = token >> 4
        if length == 15:
            while True:
                b = data[ip]
                ip += 1
                length += b
                if b != 255:
                    break
        out += data[ip:ip + length]
        ip += length
        if len(out) >= expected or ip >= len(data):
            break
        offset = data[ip] | (data[ip + 1] << 8)
        ip += 2
        match = token & 15
        if match == 15:
            while True:
                b = data[ip]
                ip += 1
                match += b
                if b != 255:
                    break
        for _ in range(match + 4):
            out.append(out[-offset])
    return bytes(out[:expected])


class Tim:
    def __init__(self, data, offset):
        magic, flags = struct.unpack_from("<II", data, offset)
        if magic != 0x10 or flags & 3 > 1:
            raise ValueError("not a 4/8-bit TIM")
        self.bpp, pos = flags & 3, offset + 8
        self.clut, self.clut_w, self.clut_h, self.clut_x, self.clut_y = [], 0, 0, 0, 0
        if flags & 8:
            block, self.clut_x, self.clut_y, w, h = struct.unpack_from("<IHHHH", data, pos)
            colors, per = (block - 12) // 2, (16 if self.bpp == 0 else 256)
            if w * h != colors:
                w, h = per, colors // per
            self.clut_w, self.clut_h = w, h
            self.clut = list(struct.unpack_from(f"<{w * h}H", data, pos + 12))
            pos += block
        _, self.x, self.y, self.units, self.h = struct.unpack_from("<IHHHH", data, pos)
        self.image = data[pos + 12:pos + 12 + self.units * 2 * self.h]

    @property
    def width(self):
        return self.units * (4 if self.bpp == 0 else 2)

    def palettes(self):
        per = 16 if self.bpp == 0 else 256
        return max(1, self.clut_w // per) * self.clut_h if self.clut else 0

    def palette_of_clut(self, clut_word):
        """Palette number for a polygon's CLUT field ((y << 6) | (x >> 4)), or None if it is not one of ours."""
        per = 16 if self.bpp == 0 else 256
        x, y = (clut_word & 63) * 16, (clut_word >> 6) & 511
        line = max(1, self.clut_w // per)
        col, row = (x - self.clut_x) // per, y - self.clut_y
        if x < self.clut_x or (x - self.clut_x) % per or not 0 <= col < line or not 0 <= row < self.clut_h:
            return None
        return row * line + col

    def rgba(self, palette_of_texel, table=None):
        """palette_of_texel: one palette number per texel (row by row), or a single number for all.
        table: a colour table to use instead of the TIM's own (colour animation frame)."""
        per = 16 if self.bpp == 0 else 256
        line = max(1, self.clut_w // per)
        clut = table if table is not None else self.clut
        out, row_bytes = bytearray(), self.units * 2
        for y in range(self.h):
            row = self.image[y * row_bytes:(y + 1) * row_bytes]
            for x in range(self.width):
                palette = palette_of_texel if isinstance(palette_of_texel, int) else palette_of_texel[y * self.width + x]
                base = (palette // line) * self.clut_w + (palette % line) * per
                index = ((row[x >> 1] >> 4) if x & 1 else (row[x >> 1] & 15)) if self.bpp == 0 else row[x]
                c = clut[base + index] if base + index < len(clut) else 0
                r, g, b = c & 31, (c >> 5) & 31, (c >> 10) & 31
                out += bytes(((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 0 if c == 0 else 255))
        return bytes(out)


def section42(wmset):
    pointers = struct.unpack_from("<48I", wmset, 0)
    start = pointers[41]
    end = min([p for p in pointers if start < p < len(wmset)] + [len(wmset)])
    tims, toc = [], start
    while toc + 4 <= end:
        rel = struct.unpack_from("<I", wmset, toc)[0]
        if rel == 0 or start + rel >= end:
            break
        tims.append(Tim(wmset, start + rel))
        toc += 4
    return tims


def section_tims(wmset, index):
    """TIMs of a section that starts with a table of offsets (index = section number - 1)."""
    pointers = struct.unpack_from("<48I", wmset, 0)
    start = pointers[index]
    end = min([p for p in pointers if start < p < len(wmset)] + [len(wmset)])
    tims, toc = [], start
    while toc + 4 <= end:
        rel = struct.unpack_from("<I", wmset, toc)[0]
        if rel == 0 or start + rel >= end:
            break
        try:
            tims.append(Tim(wmset, start + rel))
        except ValueError:
            break
        toc += 4
    return tims


def colour_animations(wmset):
    """Section 41: {(clut x, clut y): [colour table per frame]}."""
    pointers = struct.unpack_from("<48I", wmset, 0)
    start = pointers[40]
    end = min([p for p in pointers if start < p < len(wmset)] + [len(wmset)])
    out, toc = {}, start
    while toc + 4 <= end:
        rel = struct.unpack_from("<I", wmset, toc)[0]
        if rel == 0 or start + rel >= end:
            break
        entry = start + rel
        frames, clut_x, clut_y = wmset[entry + 2], *struct.unpack_from("<HH", wmset, entry + 4)
        tables = []
        for f in range(frames):
            frame = entry + 12 + struct.unpack_from("<I", wmset, entry + 12 + 4 * f)[0]
            w, h = struct.unpack_from("<HH", wmset, frame + 16)
            tables.append(list(struct.unpack_from(f"<{w * h}H", wmset, frame + 20)))
        out[(clut_x, clut_y)] = tables
        toc += 4
    return out


def atlas_of(tim):
    """(atlas page name, texel x, texel y) the way DEMASTER's AtlasOf() works, or None."""
    row = tim.y // 256
    if tim.units == 0 or tim.h == 0 or tim.y % 256 + tim.h > 256:
        return None
    if tim.bpp == 0:
        if tim.x % 64 + tim.units > 64:
            return None
        return f"page{tim.x // 64 + 16 * row}", (tim.x % 64) * 4, tim.y % 256
    first = (tim.x // 128) * 2  # 8-bit pages are 128 units wide
    if tim.x + tim.units > first * 64 + 128:
        first = tim.x // 64
    if tim.x + tim.units > first * 64 + 128 or first > 15:
        return None
    return f"page{first + 16 * row}_8bit", (tim.x - first * 64) * 2, tim.y % 256


def section16_models(wmset):
    """Model number -> list of (three (u, v) texture coordinates, CLUT word, texture page word) per triangle."""
    pointers = struct.unpack_from("<48I", wmset, 0)
    start = pointers[15]
    end = min([p for p in pointers if start < p < len(wmset)] + [len(wmset)])
    count = struct.unpack_from("<I", wmset, start)[0] // 4
    models = {}
    for i, rel in enumerate(struct.unpack_from(f"<{count}I", wmset, start)):
        if rel == 0 or start + rel + 8 > end:
            continue  # Remastered left model 14 (the Dollet vessel) pointing outside the section
        base = start + rel
        triangles, quads, tpage, _ = struct.unpack_from("<4H", wmset, base)
        pos, polys = base + 8, []
        for _ in range(triangles):
            *_, ua, va, ub, vb, uc, vc, clut = struct.unpack_from("<4B6BH", wmset, pos)
            polys.append((((ua, va), (ub, vb), (uc, vc)), clut, tpage))
            pos += 12
        for _ in range(quads):
            _, _, _, _, ua, va, ub, vb, uc, vc, ud, vd, clut, _, _ = struct.unpack_from("<4B8BHBB", wmset, pos)
            polys.append((((ua, va), (ub, vb), (uc, vc)), clut, tpage))
            polys.append((((ub, vb), (uc, vc), (ud, vd)), clut, tpage))
            pos += 16
        models[i] = polys
    return models


def palette_regions(tim, polygons):
    """Palette number per texel from the polygons that draw it (None where no polygon does), and conflicting texels."""
    owner = [None] * (tim.width * tim.h)
    conflicts = 0
    for (a, b, c), clut, tpage in polygons:
        palette = tim.palette_of_clut(clut)
        if palette is None:
            continue
        ox = (tim.x - (tpage & 15) * 64) * (4 if tim.bpp == 0 else 2)
        oy = tim.y - ((tpage >> 4) & 1) * 256
        pts = [(u - ox, v - oy) for u, v in (a, b, c)]

        def edge(p, q, x, y):
            return (q[0] - p[0]) * (y - p[1]) - (q[1] - p[1]) * (x - p[0])
        if edge(pts[0], pts[1], *pts[2]) == 0:
            continue
        for y in range(max(0, min(p[1] for p in pts)), min(tim.h, max(p[1] for p in pts) + 1)):
            for x in range(max(0, min(p[0] for p in pts)), min(tim.width, max(p[0] for p in pts) + 1)):
                w = [edge(pts[0], pts[1], x + 0.5, y + 0.5), edge(pts[1], pts[2], x + 0.5, y + 0.5), edge(pts[2], pts[0], x + 0.5, y + 0.5)]
                if min(w) >= 0 or max(w) <= 0:
                    k = y * tim.width + x
                    if owner[k] is not None and owner[k] != palette:
                        conflicts += 1
                    elif owner[k] is None:
                        owner[k] = palette
    return owner, conflicts


TWINS = {1: 0, 16: 15, 32: 29}
CHARA_TWINS = {3: 2}  # the two chocobo models are exact copies


def chara_one(data):
    result, cur = [], len(data) - 4
    count = struct.unpack_from("<I", data, cur)[0]
    for model in range(count):
        texture = 0
        while cur >= 8:
            cur -= 4
            value = struct.unpack_from("<I", data, cur)[0]
            if value & 0x80000000:
                break
            result.append((model, texture, Tim(data, value & 0x0FFFFFFF)))
            texture += 1
        if cur < 8:
            break
        cur -= 4
    return result


def write_png(path, width, height, rgba):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    raw = b"".join(b"\0" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))

    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


TEST_TINTS = [(255, 0, 255), (0, 255, 255), (255, 255, 0), (0, 255, 0), (255, 128, 0), (0, 128, 255)]


def test_pattern(width, height, rgba, palette_of_texel, scale=4):
    out_w, out_h = width * scale, height * scale
    out = bytearray(out_w * out_h * 4)
    for y in range(out_h):
        for x in range(out_w):
            k = (y // scale) * width + x // scale
            s = k * 4
            o = (y * out_w + x) * 4
            palette = palette_of_texel if isinstance(palette_of_texel, int) else palette_of_texel[k]
            tint = TEST_TINTS[palette % len(TEST_TINTS)]
            if x < 2 or y < 2 or x >= out_w - 2 or y >= out_h - 2:
                out[o:o + 4] = b"\xff\xff\xff\xff"
            elif rgba[s + 3]:
                out[o:o + 4] = bytes(((rgba[s] + tint[0]) // 2, (rgba[s + 1] + tint[1]) // 2, (rgba[s + 2] + tint[2]) // 2, 255))
    return out_w, out_h, bytes(out)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("folder", help="folder with world.fi/.fl/.fs (DEMASTER_EXP\\data)")
    parser.add_argument("out_folder")
    parser.add_argument("--extract", action="store_true", help="also write wmsetus.obj and chara.one")
    parser.add_argument("--test", action="store_true", help="write 4x test textures, palette regions tinted")
    args = parser.parse_args(argv)

    files = read_archive(args.folder)
    wmset_name = next((n for n in ("wmsetus.obj", "wmset.obj") if n in files), next((n for n in files if n.startswith("wmset")), None))
    if wmset_name is None or "chara.one" not in files:
        print("world.fi/.fl/.fs (or wmset*.obj and chara.one) not found in", args.folder)
        return 1
    os.makedirs(args.out_folder, exist_ok=True)
    if args.extract:
        for name in (wmset_name, "chara.one"):
            with open(os.path.join(args.out_folder, "wmsetus.obj" if name.startswith("wmset") else name), "wb") as f:
                f.write(files[name])

    wmset = files[wmset_name]
    tims42 = section42(wmset)
    models = section16_models(wmset)
    entries = []  # (name, label, tim, palette per texel or one palette, note, colour table or None)
    for i, tim in enumerate(tims42):
        if i in TWINS:
            continue
        group = [i] + [t for t, base in TWINS.items() if base == i]
        polygons = [p for m in group for p in models.get(m, [])]
        owner, conflicts = palette_regions(tim, polygons)
        used = sorted({p for p in owner if p is not None})
        if tim.palettes() <= 1:
            note = ""
        elif not polygons:
            note = "no model in this world.fs, all shown with palette 0"
        else:
            note = f"palette regions {' '.join(map(str, used))}" + (f", {conflicts} texels drawn with two palettes" if conflicts else "")
        regions = [p if p is not None else 0 for p in owner]
        twin_note = ", ".join(f"also used as texture{t}" for t, base in TWINS.items() if base == i)
        entries.append((f"world/dat/wmset/section42/texture{i}", LABELS_42.get(i, ""), tim, regions,
                        "; ".join(x for x in (note, twin_note) if x), None))
    animations = colour_animations(wmset)
    atlases = {}  # atlas file -> [(texture name, label, tim, palette, colour table, x, y)]
    for section, labels in ((38, LABELS_38), (39, LABELS_39), (40, LABELS_40)):
        for i, tim in enumerate(section_tims(wmset, section - 1)):
            name = f"world/dat/wmset/section{section}/texture{i}"
            label = labels.get(i, "")
            frames = animations.get((tim.clut_x, tim.clut_y)) if tim.bpp == 1 else None
            spot = atlas_of(tim) if (section == 38 and i >= 8) or section == 39 else None
            if spot is None:
                # far terrain (section 38 texture0-7, section 40): one file each, like the vehicles
                entries.append((name, label, tim, 0, f"{tim.palettes()} palettes (regions), shown with palette 0"
                                if tim.palettes() > 1 else "", None))
                continue
            atlas, x, y = spot
            if frames:  # colour animation: frame 0 in the atlas, the others in atlas_f<N>
                atlases.setdefault(atlas, []).append((name, label, tim, 0, frames[0], x, y))
                for f in range(1, len(frames)):
                    atlases.setdefault(f"{atlas}_f{f}", []).append((name, label, tim, f, frames[f], x, y))
            elif tim.palettes() > 1:  # palettes recolouring the same texels: palette 0 in the atlas, the others in atlas_p<N>
                atlases.setdefault(atlas, []).append((name, label, tim, 0, None, x, y))
                for p in range(1, tim.palettes()):
                    atlases.setdefault(f"{atlas}_p{p}", []).append((name, label, tim, p, None, x, y))
            else:
                atlases.setdefault(atlas, []).append((name, label, tim, 0, None, x, y))
    for atlas, members in sorted(atlases.items()):
        entries.append((f"world/dat/wmset/atlas/{atlas}", "page atlas", members, 0, "", None))
    by_model = {}
    for m, k, t in chara_one(files["chara.one"]):
        by_model.setdefault(m, []).append(t)
    for m, tims in sorted(by_model.items()):
        if m in CHARA_TWINS:
            continue
        twin_note = ", ".join(f"also used as model{a}" for a, base in CHARA_TWINS.items() if base == m)
        widths = "+".join(str(t.width) for t in tims)
        entries.append((f"world/esk/chara_one/model{m}", LABELS_CHARA.get(m, ""), tims, 0,
                        "; ".join(x for x in (f"strip of {len(tims)} texture(s), {widths} texels wide", twin_note) if x), None))
    rows, written = [], 0
    for name, label, tim, regions, note, table in entries:
        if isinstance(tim, list) and tim and isinstance(tim[0], tuple):  # page atlas: 256x256 texels, textures at their spot
            w = h = 256
            rgba, tint = bytearray(w * h * 4), [0] * (w * h)
            for member, member_label, t, palette, colours, x0, y0 in tim:
                part = t.rgba(palette if colours is None else 0, colours)
                for y in range(t.h):
                    rgba[((y0 + y) * w + x0) * 4:((y0 + y) * w + x0 + t.width) * 4] = part[y * t.width * 4:(y + 1) * t.width * 4]
                    tint[(y0 + y) * w + x0:(y0 + y) * w + x0 + t.width] = [palette] * t.width
                rows.append((name.replace("/", "\\"), member_label, t.width, t.h, 4 if t.bpp == 0 else 8, t.palettes(),
                             f"{member.rsplit('/', 2)[-2]}\\{member.rsplit('/', 1)[-1]} at {x0},{y0} in the atlas"
                             + (f" (palette {palette})" if colours is None and palette else f" (frame {palette})" if palette else ""),
                             f"{t.x},{t.y}", ""))
            pixels = bytes(rgba)
            if args.test:
                w, h, pixels = test_pattern(w, h, pixels, tint)
            # palette and frame atlases are optional: written apart, copy the ones you make different
            folder = os.path.join(args.out_folder, "optional") if re.search(r"_[pf]\d+$", name) else args.out_folder
            write_png(os.path.join(folder, *f"{name}.png".split("/")), w, h, pixels)
            written += 1
            continue
        if isinstance(tim, list):  # chara.one model strip: textures side by side, top aligned
            w, h = sum(t.width for t in tim), max(t.h for t in tim)
            rgba = bytearray(w * h * 4)
            x0 = 0
            for t in tim:
                part = t.rgba(0)
                for y in range(t.h):
                    rgba[(y * w + x0) * 4:(y * w + x0 + t.width) * 4] = part[y * t.width * 4:(y + 1) * t.width * 4]
                x0 += t.width
            pixels, bits, palettes, spot, shared = bytes(rgba), 4 if tim[0].bpp == 0 else 8, 1, "placed by the game when the map loads", ""
        else:
            w, h, pixels = tim.width, tim.h, tim.rgba(regions, table)
            bits, palettes = 4 if tim.bpp == 0 else 8, tim.palettes()
            spot = f"{tim.x},{tim.y}"
            base_name = name.split("_")[0]
            shared = " ".join("\\".join(n.split("/")[-2:]) for n, _, t, _, _, _ in entries
                              if not isinstance(t, list) and n.split("_")[0] != base_name and "_" not in n.rsplit("/", 1)[-1]
                              and t.x < tim.x + tim.units and tim.x < t.x + t.units and tim.y < t.y + t.h and t.y < tim.y + tim.h)
        if args.test:
            w, h, pixels = test_pattern(w, h, pixels, regions if not isinstance(tim, list) else 0)
        # far terrain: the wmset_016-019 page files already cover it, so these would only cover them up again
        far = name.startswith("world/dat/wmset/section38/") or name.startswith("world/dat/wmset/section40/")
        write_png(os.path.join(os.path.join(args.out_folder, "optional") if far else args.out_folder, *f"{name}.png".split("/")),
                  w, h, pixels)
        written += 1
        rows.append((name.replace("/", "\\"), label, w if not args.test else w // 4, h if not args.test else h // 4, bits,
                     palettes, note, spot, shared))
    with open(os.path.join(args.out_folder, "world_textures.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["file (.png or .dds)", "what", "width", "height", "bits", "palettes", "notes", "VRAM x,y in the file",
                    "shares that VRAM spot with"])
        w.writerows(rows)
    print(f"{written} files from {wmset_name} and chara.one written to {args.out_folder}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

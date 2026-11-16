#!/usr/bin/env python3
"""Join per-piece chara.one textures (model1-0.png .. model1-3.png) into the one-strip-per-model files DEMASTER reads.

Usage:
    python join_chara_strips.py "<...>\\DEMASTER_EXP\\data" "<...>\\DEMASTER_EXP\\textures\\world\\esk\\chara_one"

The first folder holds world.fi/.fl/.fs (to know each model's pieces and their widths), the second your
model<M>-<T>.png files. For every model with at least one piece there, it writes model<M>.png next to them: the pieces
left to right in file order (Ragnarok: 12+84+92+68 texels wide), top aligned, all at the scale of your files. A piece
you do not have comes from an identical piece of another model if you have that (student Squall's head is Squall's),
otherwise from the game's own texture, enlarged with nearest neighbour, so the strip is complete.
Your per-piece files are left as they are (DEMASTER no longer reads them; delete them when you are happy).
model3 is the same as model2 (the two chocobo models), so it is not written.
Pure Python, no packages needed. Reads 8-bit PNGs (RGB, RGBA, grey, palette); for .dds pieces convert them first.
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from world_vehicle_dump import read_archive, chara_one, write_png  # noqa: E402

CHARA_TWINS = {3: 2}


def read_png(path):
    """Returns (width, height, RGBA bytes) for a non-interlaced 8-bit PNG."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, palette, alpha = 8, b"", None, None
    width = height = depth = colour = interlace = None
    while pos < len(data):
        length, kind = struct.unpack_from(">I4s", data, pos)
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = body
        elif kind == b"tRNS":
            alpha = body
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    if depth != 8 or interlace:
        raise ValueError("only 8-bit, non-interlaced PNGs")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour]
    raw = zlib.decompress(idat)
    stride = width * channels
    rows, prev, pos = [], bytearray(stride), 0
    for _ in range(height):
        kind, line = raw[pos], bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if kind == 1:
                line[i] = (line[i] + a) & 255
            elif kind == 2:
                line[i] = (line[i] + b) & 255
            elif kind == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    out = bytearray()
    for line in rows:
        for x in range(width):
            px = line[x * channels:(x + 1) * channels]
            if colour == 6:
                out += px
            elif colour == 2:
                out += px + b"\xff"
            elif colour == 0:
                out += bytes((px[0], px[0], px[0], 255))
            elif colour == 4:
                out += bytes((px[0], px[0], px[0], px[1]))
            else:
                k = px[0]
                out += palette[k * 3:k * 3 + 3] + bytes((alpha[k] if alpha and k < len(alpha) else 255,))
    return width, height, bytes(out)


def resize_nearest(width, height, rgba, new_width, new_height):
    out = bytearray(new_width * new_height * 4)
    for y in range(new_height):
        sy = y * height // new_height
        for x in range(new_width):
            sx = x * width // new_width
            out[(y * new_width + x) * 4:(y * new_width + x + 1) * 4] = rgba[(sy * width + sx) * 4:(sy * width + sx + 1) * 4]
    return bytes(out)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 2:
        print(__doc__)
        return 2
    files = read_archive(argv[0])
    if "chara.one" not in files:
        print("chara.one not found (world.fi/.fl/.fs) in", argv[0])
        return 1
    folder = argv[1]
    models = {}
    for m, k, tim in chara_one(files["chara.one"]):
        models.setdefault(m, []).append((k, tim))
    written = 0
    for m, pieces in sorted(models.items()):
        if m in CHARA_TWINS:
            continue
        found, names = {}, {}
        for k, tim in pieces:
            # this piece's file, or the file of an identical piece of another model (student Squall's head = Squall's)
            candidates = [(m, k)] + [(om, ok) for om, others in models.items() for ok, other in others
                                     if (om, ok) != (m, k) and other.image == tim.image and other.clut == tim.clut]
            for cm, ck in candidates:
                path = os.path.join(folder, f"model{cm}-{ck}.png")
                if os.path.exists(path):
                    found[k], names[k] = read_png(path), f"model{cm}-{ck}.png"
                    break
        if not found:
            continue
        # scale of your files: the largest one found (pieces at a smaller scale are enlarged to it)
        scale = max(w / tim.width for k, tim in pieces if k in found for w, _, _ in [found[k]])
        strip_w = round(sum(tim.width for _, tim in pieces) * scale)
        strip_h = round(max(tim.h for _, tim in pieces) * scale)
        strip = bytearray(strip_w * strip_h * 4)
        x0 = 0
        for k, tim in pieces:
            w, h = round(tim.width * scale), round(tim.h * scale)
            if k in found:
                fw, fh, rgba = found[k]
                piece = rgba if (fw, fh) == (w, h) else resize_nearest(fw, fh, rgba, w, h)
                source = names[k]
            else:
                piece = resize_nearest(tim.width, tim.h, tim.rgba(0), w, h)
                source = "game texture"
            for y in range(min(h, strip_h)):
                strip[(y * strip_w + x0) * 4:(y * strip_w + x0 + w) * 4] = piece[y * w * 4:(y + 1) * w * 4]
            print(f"  model{m}: piece {k} ({tim.width}x{tim.h} texels) at x={x0} from {source}")
            x0 += w
        write_png(os.path.join(folder, f"model{m}.png"), strip_w, strip_h, bytes(strip))
        print(f"model{m}.png {strip_w}x{strip_h}")
        written += 1
    print(f"{written} strip(s) written to {folder}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

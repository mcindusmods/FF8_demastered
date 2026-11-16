#!/usr/bin/env python3
"""Build starter far-terrain pages (wmset_016_0 .. wmset_019_0) from your texl HD textures.

The world map draws distant land from texture pages 16-19: half-size copies of the 16 texl textures, laid out
like this on each 256x256 page (at 1x):

    page 16:  texl_001 | texl_002        page 17: texl_005 | texl_006
              ---------+---------                 ---------+---------
              texl_003 | texl_004                 texl_007 | texl_008
    page 18: texl_009..012, page 19: texl_013..016, same pattern. Each cell is 128x128 at 1x.

Usage:
    python build_far_pages.py "<...>\\DEMASTER_EXP\\textures\\world\\dat\\texl" out_folder [--scale 4]

Reads texl_XXX_0.png (PNG; DDS only if your Pillow can decode it) and writes wmset_016_0.png .. wmset_019_0.png at
256 * scale pixels. Put them in DEMASTER_EXP\\textures\\world\\dat\\wmset\\ next to Maki's other wmset_XXX pages and
touch them up like any other page. Missing texl files leave their cell transparent (the game's own pixels are not
available here). Needs Pillow (pip install pillow).
"""
import argparse
import os
import sys

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    sys.exit("This tool needs Pillow: pip install pillow")


def cells(page):
    """(texl file number, x, y) for the four 128x128 cells of a far page at 1x."""
    first = (page - 16) * 4 + 1
    return [(first, 0, 0), (first + 1, 128, 0), (first + 2, 0, 128), (first + 3, 128, 128)]


def find_texl(folder, number):
    for ext in (".png", ".dds"):
        path = os.path.join(folder, f"texl_{number:03d}_0{ext}")
        if os.path.exists(path):
            return path
    return None


def build(texl_folder, out_folder, scale):
    os.makedirs(out_folder, exist_ok=True)
    size, cell = 256 * scale, 128 * scale
    written = []
    for page in range(16, 20):
        canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        for number, x, y in cells(page):
            path = find_texl(texl_folder, number)
            if path is None:
                print(f"  wmset_{page:03d}: texl_{number:03d}_0 not found, cell left empty")
                continue
            image = Image.open(path).convert("RGBA").resize((cell, cell), Image.LANCZOS)
            canvas.paste(image, (x * scale, y * scale))
        out = os.path.join(out_folder, f"wmset_{page:03d}_0.png")
        canvas.save(out)
        written.append(out)
        print(f"wrote {out}")
    return written


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("texl_folder")
    parser.add_argument("out_folder")
    parser.add_argument("--scale", type=int, default=4, help="page size = 256 * scale (default 4 = 1024px)")
    args = parser.parse_args(argv)
    build(args.texl_folder, args.out_folder, max(1, args.scale))
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Makes DEMASTER's colour grades (LUT=1 in demaster.conf): the LUTs in DEMASTER_EXP/lut/, from the looks described below.

  python3 tools/make_luts.py [output folder]                     the eight LUTs (default: DEMASTER_EXP/lut)
  python3 tools/make_luts.py --preview day.png [night.png] [sheet.png]   a field by day (and by night) through them

Each LUT is a strip of 32 squares of 32 x 32 (1024 x 32), as ReShade's and Unreal's: in a square red goes right and green
down, the squares are blue's steps. Any LUT tool that writes that layout (or a .cube file) can make them instead; this
script is only one way. Needs numpy and Pillow.

A look is worked out on the picture's colours in this order (in linear light unless it says otherwise):
  exposure      times this
  balance       white balance: times this colour (made to keep the brightness)
  shadows       a tint the dark parts take (colour, how much), from the darkest down to the middle
  highlights    a tint the bright parts take (colour, how much), from the middle up
  keep          how much the brightest parts (lamps, glows, the sky's light) keep their own colour (0-1)
  saturation    0 grey, 1 as it is; dark_saturation: for the darkest parts (the night's eyes see less colour)
  cool_hues     how much of the colour greens, cyans and blues lose (the Lunar Cry's red world)
  contrast      around the middle grey, on the picture as you see it (1: as it is)
  lift          added to black (the night's air, the morning's haze), linear
The time's own darkening is not in the LUTs: PBR fields dim to their night and battles and the world map take the night's
colour (SCENE_LIGHT) before the grade, so the night LUT only cools and quietens what is already dark.
"""
import os
import sys

import numpy as np
from PIL import Image

N = 32

LOOKS = {
    # the day as painted
    "day": dict(),
    # golden hour into sunset: amber light, violet shade (the sky's), more colour and depth
    "dusk": dict(exposure=0.97, balance=(1.30, 0.97, 0.62), shadows=((0.52, 0.42, 1.0), 0.50),
                 highlights=((1.0, 0.54, 0.24), 0.68), keep=0.30, saturation=1.22, contrast=1.10),
    # first light: rose and peach in the light, lavender shade, a soft haze, quieter than the day
    "dawn": dict(exposure=1.0, balance=(1.16, 0.95, 0.96), shadows=((0.70, 0.60, 1.0), 0.50),
                 highlights=((1.0, 0.66, 0.60), 0.66), keep=0.30, saturation=1.0, contrast=0.94,
                 lift=(0.0080, 0.0060, 0.0110)),
    # moonlight on what is already dark: blue shade, the colour going out of the dark parts, the lamps kept warm
    "night": dict(balance=(0.94, 1.0, 1.10), shadows=((0.48, 0.66, 1.0), 0.52), highlights=((0.88, 0.94, 1.0), 0.08),
                  keep=0.75, saturation=0.92, dark_saturation=0.55, contrast=1.04, lift=(0.0006, 0.0010, 0.0022)),
    # the Lunar Cry: the red moon over everything
    "lunar_day": dict(exposure=0.94, balance=(1.36, 0.80, 0.74), shadows=((1.0, 0.22, 0.26), 0.55),
                      highlights=((1.0, 0.44, 0.30), 0.55), keep=0.20, saturation=1.06, cool_hues=0.70, contrast=1.12),
    "lunar_dusk": dict(exposure=0.90, balance=(1.42, 0.78, 0.60), shadows=((0.95, 0.16, 0.30), 0.62),
                       highlights=((1.0, 0.36, 0.16), 0.70), keep=0.20, saturation=1.12, cool_hues=0.75, contrast=1.14),
    "lunar_night": dict(balance=(1.18, 0.86, 0.90), shadows=((1.0, 0.18, 0.28), 0.62), highlights=((1.0, 0.62, 0.56), 0.18),
                        keep=0.6, saturation=0.98, dark_saturation=0.72, cool_hues=0.65, contrast=1.06,
                        lift=(0.0030, 0.0006, 0.0010)),
    "lunar_dawn": dict(exposure=0.96, balance=(1.30, 0.84, 0.82), shadows=((0.95, 0.30, 0.45), 0.52),
                       highlights=((1.0, 0.48, 0.40), 0.60), keep=0.25, saturation=1.02, cool_hues=0.65, contrast=1.06,
                       lift=(0.0050, 0.0020, 0.0028)),
}
ORDER = ["day", "dusk", "night", "dawn", "lunar_day", "lunar_dusk", "lunar_night", "lunar_dawn"]
LUMA = np.array([0.2126, 0.7152, 0.0722])


def to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1.0 / 2.4) - 0.055)


def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def unit_brightness(colour):
    c = np.array(colour, dtype=np.float64)
    return c / float(np.dot(c, LUMA))


def grade(srgb, look):
    """srgb: (..., 3) in 0-1. Returns the graded colours (..., 3) in 0-1."""
    if not look:
        return srgb.copy()
    lin = to_linear(srgb)
    keep = look.get("keep", 0.0)
    original = lin.copy()
    lin = lin * look.get("exposure", 1.0)
    if "balance" in look:
        lin = lin * unit_brightness(look["balance"])
    shown = to_srgb(np.clip(lin, 0, 1)) @ LUMA  # how bright it looks (0-1), for where the tints go
    if "shadows" in look:
        colour, amount = look["shadows"]
        w = (1.0 - smooth(0.0, 0.55, shown))[..., None] * amount
        lin = lin * (1.0 - w + w * unit_brightness(colour))
    if "highlights" in look:
        colour, amount = look["highlights"]
        w = smooth(0.35, 1.0, shown)[..., None] * amount
        lin = lin * (1.0 - w + w * unit_brightness(colour))
    # saturation around the brightness, less in the dark parts if asked
    y = (lin @ LUMA)[..., None]
    s = look.get("saturation", 1.0)
    if "dark_saturation" in look:
        s = look["dark_saturation"] + (s - look["dark_saturation"]) * smooth(0.05, 0.6, shown)[..., None]
    lin = y + (lin - y) * s
    if look.get("cool_hues", 0.0) > 0.0:
        # green to violet (hue 75-310 degrees) loses colour: the world under the red moon
        disp = to_srgb(np.clip(lin, 0, 1))
        mx, mn = disp.max(axis=-1), disp.min(axis=-1)
        r, g, b = disp[..., 0], disp[..., 1], disp[..., 2]
        delta = np.maximum(mx - mn, 1e-6)
        hue = np.where(mx == r, ((g - b) / delta) % 6.0, np.where(mx == g, (b - r) / delta + 2.0, (r - g) / delta + 4.0)) * 60.0
        cool = smooth(55, 95, hue) * (1.0 - smooth(290, 330, hue))
        chroma = smooth(0.02, 0.15, mx - mn)
        lose = (look["cool_hues"] * cool * chroma)[..., None]
        y = (lin @ LUMA)[..., None]
        lin = lin + (y - lin) * lose
    # the brightest parts keep their own colour (lamps stay lamps)
    if keep > 0.0:
        bright = smooth(0.62, 0.95, to_srgb(original) @ LUMA)[..., None] * keep
        lin = lin * (1.0 - bright) + original * bright
    lin = np.maximum(lin, 0.0)
    out = to_srgb(lin)
    c = look.get("contrast", 1.0)
    if c != 1.0:
        # an S around the middle grey that keeps black and white where they are
        x = np.clip(out, 0.0, 1.0)
        pivot = 0.46
        lo = pivot * np.power(x / pivot, c)
        hi = 1.0 - (1.0 - pivot) * np.power((1.0 - x) / (1.0 - pivot), c)
        out = np.where(x < pivot, lo, hi)
    if "lift" in look:
        lift = np.array(look["lift"])
        lin = to_linear(np.clip(out, 0, 1))
        lin = lift + lin * (1.0 - lift)
        out = to_srgb(lin)
    return np.clip(out, 0.0, 1.0)


def strip(look):
    i = np.arange(N) / (N - 1.0)
    b, g, r = np.meshgrid(i, i, i, indexing="ij")  # [blue][green][red]
    cube = np.stack([r, g, b], axis=-1)  # (blue, green, red, 3)
    graded = grade(cube, look)
    # the strip: row = green, column = red + N * blue
    picture = np.transpose(graded, (1, 0, 2, 3)).reshape(N, N * N, 3)
    return np.round(picture * 255.0).astype(np.uint8)


def apply_strip(picture, lut):
    """the shaders' arithmetic (lut_grade.h Apply) on a picture (H, W, 3, 0-1) with a strip (N, N*N, 3 uint8)"""
    n = lut.shape[0]
    table = lut.astype(np.float64) / 255.0
    r, g, b = (np.clip(picture[..., k], 0, 1) * (n - 1) for k in range(3))
    slice0 = np.minimum(np.floor(b), n - 1).astype(int)
    slice1 = np.minimum(slice0 + 1, n - 1)
    fb = (b - slice0)[..., None]

    def square(s):
        r0 = np.clip(np.floor(r).astype(int), 0, n - 1)
        g0 = np.clip(np.floor(g).astype(int), 0, n - 1)
        r1, g1 = np.minimum(r0 + 1, n - 1), np.minimum(g0 + 1, n - 1)
        fr, fg = (r - r0)[..., None], (g - g0)[..., None]
        at = lambda rr, gg: table[gg, s * n + rr]
        top = at(r0, g0) * (1 - fr) + at(r1, g0) * fr
        bottom = at(r0, g1) * (1 - fr) + at(r1, g1) * fr
        return top * (1 - fg) + bottom * fg

    return square(slice0) * (1 - fb) + square(slice1) * fb


def main():
    args = sys.argv[1:]
    here = os.path.dirname(os.path.abspath(__file__))
    if args and args[0] == "--preview":
        # --preview day.png [night.png] [sheet.png]: before the grade (top), with it (middle), the Lunar Cry's (bottom); the
        # dusk and the dawn on the day picture half way to the night (as SCENE_LIGHT's default night: 0.3 x a blue tint),
        # the way the game has it in the middle of a fade; the night on the night picture
        rest = args[1:]
        sheet_path = "lut_preview.png"
        if len(rest) == 3 or (len(rest) == 2 and not os.path.exists(rest[1])):
            sheet_path = rest.pop()  # the last one is the sheet to write
        day_path = rest[0]
        night_path = rest[1] if len(rest) > 1 else None

        def load(path):
            im = Image.open(path).convert("RGB")
            im = im.resize((480, int(im.size[1] * 480.0 / im.size[0])), Image.LANCZOS)
            return np.asarray(im, dtype=np.float64) / 255.0

        day = load(day_path)
        night = load(night_path) if night_path else None
        if night is not None and night.shape != day.shape:
            night = np.asarray(Image.fromarray(np.round(night * 255).astype(np.uint8)).resize((day.shape[1], day.shape[0])),
                               dtype=np.float64) / 255.0
        tint = 0.3 * np.array([0.55, 0.65, 1.0])
        half = to_srgb(to_linear(day) * (0.5 + 0.5 * tint))
        full = night if night is not None else to_srgb(to_linear(day) * tint)
        bases = {"day": day, "dusk": half, "night": full, "dawn": half}
        sh, sw = day.shape[:2]
        sheet = Image.new("RGB", (sw * 4, sh * 3))
        for column, name in enumerate(["day", "dusk", "night", "dawn"]):
            base = bases[name]
            for row, picture in enumerate([base, apply_strip(base, strip(LOOKS[name])),
                                           apply_strip(base, strip(LOOKS["lunar_" + name]))]):
                sheet.paste(Image.fromarray(np.round(np.clip(picture, 0, 1) * 255).astype(np.uint8)), (column * sw, row * sh))
        sheet.save(sheet_path)
        print("wrote", sheet_path, "(columns: day, dusk, night, dawn; rows: before the grade, graded, the Lunar Cry's)")
        return
    folder = args[0] if args else os.path.join(here, "..", "DEMASTER_EXP", "lut")
    os.makedirs(folder, exist_ok=True)
    for name in ORDER:
        path = os.path.join(folder, name + ".png")
        Image.fromarray(strip(LOOKS[name])).save(path)
        print("wrote", path)


if __name__ == "__main__":
    main()

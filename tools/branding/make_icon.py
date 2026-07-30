#!/usr/bin/env python3
"""Generate castlemist's application icon and logo.

A keep standing in mist: three towers on a gradient night sky, cloud banks
rolling across the base, a moon and stars behind. The name, drawn.

The hard constraint is 16x16 in a taskbar. Detail is therefore *composed per
size* rather than scaled from one master -- see `detail_for`. A 256px render
shrunk to 16px turns into grey mush, so at 16px the moon, the stars, the flags,
the roofs and two of the three cloud banks are simply not drawn, and what
survives is the silhouette: three crenellated towers standing in one bank of
mist. Every size in between drops one thing at a time.

    python tools/branding/make_icon.py

Writes:
    src/app/castlemist.ico      16,20,24,32,48,64,128,256 -- linked into the exe
    docs/assets/logo.png        512px, for the README
    docs/assets/logo-wide.png   1280x640, for the GitHub social preview
"""

from __future__ import annotations

import math
import pathlib
import sys

try:
    from PIL import Image, ImageDraw, ImageFilter, ImageFont
except ImportError:
    sys.exit("needs Pillow:  pip install Pillow")

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent

# ---- palette ---------------------------------------------------------------
# The accent blue is the one in src/ui/theme.cpp, so the icon and the running
# program agree.
SKY_TOP  = (0x0E, 0x18, 0x2E)  # night, top of the gradient
SKY_BOT  = (0x24, 0x46, 0x86)  # indigo, bottom
GLOW     = (0x4C, 0x82, 0xD8)  # halo behind the keep

STONE_LIT = (0xDA, 0xE4, 0xF2)  # tower face, lit side
STONE_DIM = (0x9A, 0xAF, 0xCC)  # tower face, shadow side
ROOF      = (0x3E, 0x74, 0xC8)  # conical roofs
ROOF_DARK = (0x2B, 0x55, 0x9B)
OPENING   = (0x16, 0x24, 0x40)  # gate and window slits
FLAG      = (0x6F, 0xB2, 0xFF)

MIST_NEAR = (0xFF, 0xFF, 0xFF, 0xE6)
MIST_MID  = (0xC7, 0xDD, 0xFA, 0xC4)
MIST_FAR  = (0x92, 0xB6, 0xE6, 0x8E)

MOON      = (0xE8, 0xEF, 0xFA, 0xF2)
STAR      = (0xD8, 0xE6, 0xFA)

SS = 8  # supersample factor
ICON_SIZES = [16, 20, 24, 32, 48, 64, 128, 256]


class Detail:
    """What gets drawn at a given icon size."""

    def __init__(self, size: int):
        self.mist_banks = 1 if size <= 16 else 2 if size <= 24 else 3
        self.roofs      = size >= 24
        self.gate       = size >= 32
        self.windows    = size >= 48
        self.flags      = size >= 48
        self.sky        = size >= 32   # gradient; flat fill below that
        self.glow       = size >= 64
        self.stars      = size >= 64
        self.moon       = size >= 64
        self.soft_mist  = size >= 48   # blurred cloud shapes vs plain bands

        # Small sizes get a compact layout: a taller castle sitting on a low,
        # thin mist band. Reusing the 256px proportions here buries the towers
        # in cloud and the icon reads as a white mound.
        self.compact = size <= 32


def detail_for(size: int) -> Detail:
    return Detail(size)


# ---- drawing helpers -------------------------------------------------------

def sky(n: int, flat: bool) -> Image.Image:
    """Vertical night-sky gradient, or a flat mid tone at tiny sizes."""
    img = Image.new("RGB", (n, n), SKY_TOP)
    if flat:
        mid = tuple((a + b) // 2 for a, b in zip(SKY_TOP, SKY_BOT))
        return Image.new("RGB", (n, n), mid)
    d = ImageDraw.Draw(img)
    for y in range(n):
        t = y / max(1, n - 1)
        d.line([(0, y), (n, y)],
               fill=tuple(round(a + (b - a) * t) for a, b in zip(SKY_TOP, SKY_BOT)))
    return img


def tower(d: ImageDraw.ImageDraw, px, x0: float, x1: float, top: float,
          bottom: float, det: Detail, *, merlons: int = 3) -> None:
    """A crenellated tower body with a lit and a shadowed face."""
    mid = x0 + (x1 - x0) * 0.58
    d.rectangle((px(x0), px(top), px(mid), px(bottom)), fill=STONE_LIT)
    d.rectangle((px(mid), px(top), px(x1), px(bottom)), fill=STONE_DIM)

    # Crenellations: merlons and the gaps between them are the same width, which
    # is what keeps them legible when a merlon is barely a pixel across.
    span = x1 - x0
    unit = span / (merlons * 2 - 1)
    cren_h = span * 0.30
    for i in range(merlons):
        mx = x0 + i * 2 * unit
        face = STONE_LIT if mx + unit <= mid else STONE_DIM
        d.rectangle((px(mx), px(top - cren_h), px(mx + unit), px(top)), fill=face)


def roof(d: ImageDraw.ImageDraw, px, x0: float, x1: float, apex_y: float,
         base_y: float) -> None:
    """A conical roof, split down the middle for the same light direction."""
    cx = (x0 + x1) / 2
    d.polygon([(px(x0), px(base_y)), (px(cx), px(apex_y)), (px(cx), px(base_y))], fill=ROOF)
    d.polygon([(px(cx), px(base_y)), (px(cx), px(apex_y)), (px(x1), px(base_y))], fill=ROOF_DARK)


def flag(d: ImageDraw.ImageDraw, px, x: float, y: float, h: float, w: float) -> None:
    """Pole plus pennant, pointing right."""
    d.rectangle((px(x - w * 0.05), px(y - h), px(x + w * 0.05), px(y)), fill=STONE_LIT)
    d.polygon([(px(x), px(y - h)), (px(x + w), px(y - h + h * 0.22)),
               (px(x), px(y - h + h * 0.44))], fill=FLAG)


def cloud(size: int, px, cx: float, cy: float, w: float, h: float,
          colour, soft: bool) -> Image.Image:
    """A mist bank: overlapping lobes on a slab, blurred at larger sizes."""
    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    d.rounded_rectangle((px(cx - w / 2), px(cy - h / 2), px(cx + w / 2), px(cy + h / 2)),
                        radius=px(h / 2), fill=colour)
    if soft:
        # Lobes along the top edge turn the slab into something cloud-shaped.
        for frac, lobe in ((0.20, 0.62), (0.44, 1.00), (0.68, 0.78), (0.86, 0.48)):
            r = h * lobe
            lx = cx - w / 2 + w * frac
            ly = cy - h * 0.18
            d.ellipse((px(lx - r), px(ly - r), px(lx + r), px(ly + r)), fill=colour)
        layer = layer.filter(ImageFilter.GaussianBlur(radius=size * 0.004))
    return layer


def render(size: int, *, backdrop: bool = True) -> Image.Image:
    """Compose the mark at `size` px."""
    det = detail_for(size)
    n = size * SS
    px = lambda v: v * n  # noqa: E731  -- normalised (0..1) coords to pixels

    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))

    if backdrop:
        mask = Image.new("L", (n, n), 0)
        ImageDraw.Draw(mask).rounded_rectangle(
            (px(0.015), px(0.015), px(0.985), px(0.985)), radius=px(0.22), fill=255)
        img.paste(sky(n, flat=not det.sky).convert("RGBA"), (0, 0), mask)

    # ---- sky furniture ------------------------------------------------------
    if backdrop and det.glow:
        halo = Image.new("RGBA", (n, n), (0, 0, 0, 0))
        ImageDraw.Draw(halo).ellipse((px(0.16), px(0.20), px(0.84), px(0.88)),
                                     fill=(*GLOW, 0x3A))
        img.alpha_composite(halo.filter(ImageFilter.GaussianBlur(radius=n * 0.045)))

    d = ImageDraw.Draw(img)

    if backdrop and det.moon:
        # Crescent: a disc with a second disc bitten out of it.
        moon = Image.new("RGBA", (n, n), (0, 0, 0, 0))
        md = ImageDraw.Draw(moon)
        md.ellipse((px(0.700), px(0.105), px(0.855), px(0.260)), fill=MOON)
        md.ellipse((px(0.660), px(0.080), px(0.828), px(0.248)), fill=(0, 0, 0, 0))
        img.alpha_composite(moon)

    if backdrop and det.stars:
        for sx, sy, r in ((0.175, 0.150, 0.011), (0.275, 0.235, 0.008),
                          (0.135, 0.290, 0.007), (0.610, 0.115, 0.007),
                          (0.860, 0.330, 0.008)):
            d.ellipse((px(sx - r), px(sy - r), px(sx + r), px(sy + r)), fill=(*STAR, 0xE0))

    # ---- castle -------------------------------------------------------------
    # Central keep flanked by two shorter towers. Everything is symmetric about
    # x = 0.5 so the mark still looks deliberate when the detail falls away.
    if det.compact:
        keep_x0, keep_x1 = 0.395, 0.605
        keep_top, base_y = 0.250, 0.775
        side_top = 0.390
        left_x0, left_x1 = 0.205, 0.389
        right_x0, right_x1 = 0.611, 0.795
    else:
        keep_x0, keep_x1 = 0.410, 0.590
        keep_top, base_y = 0.330, 0.720
        side_top = 0.430
        left_x0, left_x1 = 0.248, 0.404
        right_x0, right_x1 = 0.596, 0.752

    if det.roofs:
        roof(d, px, left_x0 - 0.012, left_x1 + 0.012, side_top - 0.105, side_top)
        roof(d, px, right_x0 - 0.012, right_x1 + 0.012, side_top - 0.105, side_top)
        tower(d, px, left_x0, left_x1, side_top, base_y, det, merlons=2)
        tower(d, px, right_x0, right_x1, side_top, base_y, det, merlons=2)
        roof(d, px, keep_x0 - 0.020, keep_x1 + 0.020, keep_top - 0.135, keep_top)
        tower(d, px, keep_x0, keep_x1, keep_top, base_y, det, merlons=2)
    else:
        # No room for roofs: crenellations alone have to say "castle".
        tower(d, px, left_x0, left_x1, side_top, base_y, det, merlons=2)
        tower(d, px, right_x0, right_x1, side_top, base_y, det, merlons=2)
        tower(d, px, keep_x0, keep_x1, keep_top, base_y, det, merlons=3)

    if det.flags:
        flag(d, px, 0.500, keep_top - 0.135, 0.115, 0.085)
        flag(d, px, left_x0 + (left_x1 - left_x0) / 2, side_top - 0.105, 0.075, 0.058)
        flag(d, px, right_x0 + (right_x1 - right_x0) / 2, side_top - 0.105, 0.075, 0.058)

    if det.gate:
        gw, gh = 0.072, 0.135
        gx0, gx1 = 0.5 - gw, 0.5 + gw
        gy0 = base_y - gh
        # Drawn as a lit arch surround with the opening punched inside it, so
        # the gate reads on a tower face that is already light.
        b = gw * 0.30
        d.rounded_rectangle((px(gx0 - b), px(gy0 - b), px(gx1 + b), px(base_y)),
                            radius=px(gw + b), fill=STONE_LIT)
        d.rectangle((px(gx0 - b), px(gy0 + gh * 0.4), px(gx1 + b), px(base_y)), fill=STONE_LIT)
        d.rounded_rectangle((px(gx0), px(gy0), px(gx1), px(base_y)),
                            radius=px(gw), fill=OPENING)
        d.rectangle((px(gx0), px(gy0 + gh * 0.45), px(gx1), px(base_y)), fill=OPENING)

    if det.windows:
        ww, wh = 0.016, 0.040
        for wx, wy in ((0.462, 0.400), (0.538, 0.400),
                       (0.310, 0.505), (0.690, 0.505)):
            d.rounded_rectangle((px(wx - ww), px(wy), px(wx + ww), px(wy + wh)),
                                radius=px(ww), fill=OPENING)

    # ---- mist ---------------------------------------------------------------
    # The near bank crosses the towers' feet: the keep stands *in* the mist
    # rather than on a shelf above it.
    banks = [
        (0.500, 0.812, 0.86, 0.080, MIST_NEAR),
        (0.470, 0.895, 0.66, 0.062, MIST_MID),
    ] if det.compact else [
        (0.500, 0.700, 0.90, 0.115, MIST_NEAR),
        (0.470, 0.790, 0.74, 0.090, MIST_MID),
        (0.535, 0.862, 0.56, 0.070, MIST_FAR),
    ]
    for cx, cy, w, h, colour in banks[:det.mist_banks]:
        img.alpha_composite(cloud(n, px, cx, cy, w, h, colour, det.soft_mist))

    if backdrop:
        # Re-clip: glow and mist are drawn full-bleed, so trim to the rounded square.
        clip = Image.new("L", (n, n), 0)
        ImageDraw.Draw(clip).rounded_rectangle(
            (px(0.015), px(0.015), px(0.985), px(0.985)), radius=px(0.22), fill=255)
        out = Image.new("RGBA", (n, n), (0, 0, 0, 0))
        out.paste(img, (0, 0), clip)
        img = out

    return img.resize((size, size), Image.LANCZOS)


# ---- outputs ---------------------------------------------------------------

def write_ico(path: pathlib.Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    frames = [render(s) for s in ICON_SIZES]
    frames[-1].save(path, format="ICO",
                    sizes=[(s, s) for s in ICON_SIZES],
                    append_images=frames[:-1])
    print(f"  {path.relative_to(ROOT)}  ({len(ICON_SIZES)} sizes)")


def write_logo(path: pathlib.Path, size: int = 512) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    render(size).save(path, format="PNG")
    print(f"  {path.relative_to(ROOT)}  ({size}x{size})")


def load_font(size: int):
    for name in ("segoeuisb.ttf", "segoeui.ttf", "arialbd.ttf", "arial.ttf"):
        candidate = pathlib.Path("C:/Windows/Fonts") / name
        if candidate.exists():
            try:
                return ImageFont.truetype(str(candidate), size)
            except OSError:
                pass
    return ImageFont.load_default()


def write_wide(path: pathlib.Path, w: int = 1280, h: int = 640) -> None:
    """GitHub's social preview card: mark on the left, wordmark on the right.

    The wordmark lives here and *not* in the .ico: text in an application icon
    is unreadable the moment it is a taskbar button.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    img = Image.new("RGB", (w, h), SKY_TOP)
    d = ImageDraw.Draw(img)
    for y in range(h):  # same gradient as the icon, run horizontally-ish
        t = y / max(1, h - 1)
        d.line([(0, y), (w, y)],
               fill=tuple(round(a + (b - a) * t) for a, b in zip(SKY_TOP, SKY_BOT)))

    mark = render(380)
    img.paste(mark, (130, (h - 380) // 2), mark)

    d.text((590, h // 2 - 104), "castlemist", font=load_font(108), fill=STONE_LIT)
    d.text((596, h // 2 + 26), "a Guild Wars 2 .dat explorer",
           font=load_font(40), fill=(0xA9, 0xC6, 0xEE))
    d.text((596, h // 2 + 90), "Win32  \u00b7  Direct3D 11  \u00b7  C++20",
           font=load_font(28), fill=(0x7C, 0x9A, 0xC6))

    img.save(path, format="PNG")
    print(f"  {path.relative_to(ROOT)}  ({w}x{h})")


def main() -> None:
    print("castlemist branding:")
    write_ico(ROOT / "src" / "app" / "castlemist.ico")
    write_logo(ROOT / "docs" / "assets" / "logo.png")
    write_wide(ROOT / "docs" / "assets" / "logo-wide.png")


if __name__ == "__main__":
    main()

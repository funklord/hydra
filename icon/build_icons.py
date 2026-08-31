#!/usr/bin/env python3
"""Build the icon set from the master artwork.

    python3 icon/build_icons.py

One drawing, downscaled, with the small sizes retouched afterwards.

**48px and up** come from `hydra-master.png` by downscaling. The master is the
artwork with its white plate and cream halo flood-filled away and then cropped
hard to the ink: the original render was 1024x1024 but only 774x853 of it was
drawing, so a quarter of every icon would otherwise have been spent on nothing.
The artwork is taller than it is wide, and an icon slot is square, so it is
squashed the last 9% rather than letterboxed — at these sizes the distortion is
invisible and the recovered margin is not.

**32px and below are downscales too, then retouched.** The alpha curve is
steepened so the silhouette keeps an edge rather than fading out through a
skirt of half-transparent pixels, and colour and contrast are pushed back up,
because averaging thousands of source pixels into one greys it out. How hard
depends on the size and is in `TOUCH`: at 32 a firm hand helps, and at 16 the
same treatment rings and invents cyan that is nowhere in the drawing.

A hand-drawn 16 was tried and thrown away. Redrawing at that size loses the
artwork rather than compressing it — the first attempt read as a strawberry —
and a retouched downscale keeps the palette, the proportions and the silhouette
that were approved at full size.

**Android's launcher icon is generated here too**, because it is the same
drawing and a second generator is a second thing to forget. See `ANDROID` and
`FOREGROUND_DP` below for the geometry, and the comment on `build_android` for
why a transparent PNG on its own came out small and on a white plate.
"""
import os
from PIL import Image, ImageEnhance, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
MASTER = os.path.join(HERE, "hydra-master.png")
ANDROID_RES = os.path.join(os.path.dirname(HERE), "android", "res")

# Android's five density buckets, as multiples of mdpi. A launcher icon is
# 48dp and an adaptive icon's canvas is 108dp, so every file below is one of
# those two numbers times one of these.
ANDROID = {"mdpi": 1.0, "hdpi": 1.5, "xhdpi": 2.0, "xxhdpi": 3.0, "xxxhdpi": 4.0}

# **How much of the 108dp adaptive canvas the drawing gets.**
#
# The canvas is 108dp and the outer 18dp on every edge is always cropped, so
# what anybody sees is the central 72dp, and the launcher's mask is applied
# inside that. 72 is therefore the arithmetic maximum and is the wrong number
# here: the drawing is a rounded blob rather than a true circle -- the master
# is the artwork squashed 9% to fit a square -- so at 72dp a circular mask cuts
# the tops of the three heads and the outer edge of the wave. Measured by
# rendering the mask over it at 62, 64, 66, 68 and 70: clipping starts at 68.
#
# 66 is the largest that survives the circular mask, which is the harshest of
# the shapes a launcher may choose, and it still leaves a hairline of
# background rather than sitting on the edge. On the squircle and rounded
# square that Samsung and others use it reads as generously filled.
FOREGROUND_DP = 66

# **The plate behind it, and the whole reason this is not left transparent.**
# Taken from the drawing's own darkest ink rather than invented: the linework
# is around #300c37, and this sits just under it in the same family, so the
# heavy outline stops reading as an outline and reads as the edge of the
# artwork. A lighter or contrasting colour puts a ring around every stroke.
BACKGROUND = "#1B0A28"

# How hard to push, by size. Uniform settings do not work: at 32 each output
# pixel averages a few hundred source pixels and takes a firm hand well, while
# at 16 it averages a few thousand and the same treatment rings -- an unsharp
# pass at 150% on a 16px image invents cyan and magenta that are nowhere in the
# drawing. Measured by looking at it, not by taste.
TOUCH = {
    16: dict(colour=1.18, contrast=1.08, sharp=None),
    24: dict(colour=1.28, contrast=1.14, sharp=(0.5, 80)),
    32: dict(colour=1.40, contrast=1.20, sharp=(0.7, 140)),
}

def retouch(im, how):
    """Firm up a downscale so it survives being 16 or 24 pixels wide.

    A plain LANCZOS shrink of a detailed drawing comes out soft and desaturated:
    every output pixel is an average of dozens of input pixels, and averaging
    pulls colour toward grey and edges toward mush. Three passes put back what
    the averaging took, in the order that matters.

    The alpha gamma comes first. The shrink leaves a wide skirt of part-transparent
    pixels around the edge, which at 16px is a grey fringe two pixels thick on a
    dark ground; steepening the curve pushes the nearly-there pixels to solid and
    the nearly-gone ones to nothing, so the silhouette has an edge again.
    """
    r, g, b, al = im.split()
    al = al.point(lambda v: 0 if v < 34 else min(255, int(255*(v/255)**0.55)))
    im = Image.merge("RGBA", (r, g, b, al))
    rgb = im.convert("RGB")
    rgb = ImageEnhance.Color(rgb).enhance(how["colour"])    # averaging greys it out
    rgb = ImageEnhance.Contrast(rgb).enhance(how["contrast"])  # and flattens it
    if how["sharp"]:
        radius, percent = how["sharp"]
        rgb = rgb.filter(ImageFilter.UnsharpMask(radius=radius, percent=percent,
                                                  threshold=0))
    out = rgb.convert("RGBA")
    out.putalpha(al)
    return out

def build_android(master):
    """Write the launcher icon Android actually uses, which is not a PNG.

    **The symptom this fixes: the icon came up small, outlined and on a white
    background, next to a Firefox that filled its slot.** All three are one
    cause. Before this there was only `mipmap-*/ic_launcher.png`, a legacy
    icon, and every Android from 8 onwards puts a legacy icon through its
    "legacy treatment" -- shrink the bitmap, drop it on a white plate, mask
    the plate to the launcher's shape. So the drawing was scaled down inside
    something else's white circle, and its own dark linework had a white
    ground to contrast against, which is what reads as an outline.

    The app's minSdk is 26, which is the exact release adaptive icons arrived
    in, so `mipmap-anydpi-v26/ic_launcher.xml` covers every device this app
    can be installed on. The PNGs stay as the default-configuration fallback;
    nothing that can run Hydra will pick them.

    Two layers, per the adaptive icon contract: a background, which is a flat
    colour here because the artwork is already a complete composition and a
    second illustration behind it would fight it, and a foreground holding the
    drawing at `FOREGROUND_DP` of the 108dp canvas.
    """
    def px(dp, scale):
        return int(round(dp * scale))

    for bucket, scale in ANDROID.items():
        d = os.path.join(ANDROID_RES, f"mipmap-{bucket}")
        os.makedirs(d, exist_ok=True)

        # The legacy icon: 48dp, the drawing edge to edge, transparent around
        # it. Unreachable at minSdk 26 and written anyway, because a resource
        # with no default configuration is a resource one aapt version
        # complains about and the cost of keeping it is five small files.
        n = px(48, scale)
        master.resize((n, n), Image.LANCZOS).save(
          os.path.join(d, "ic_launcher.png"))

        # The adaptive foreground: a 108dp canvas, the drawing centred at
        # FOREGROUND_DP, everything else transparent so the background layer
        # shows through. The margin is not padding -- it is the crop the
        # system takes plus the room the mask needs.
        canvas_n = px(108, scale)
        art_n    = px(FOREGROUND_DP, scale)
        fg = Image.new("RGBA", (canvas_n, canvas_n), (0, 0, 0, 0))
        off = (canvas_n - art_n) // 2
        fg.alpha_composite(master.resize((art_n, art_n), Image.LANCZOS), (off, off))
        fg.save(os.path.join(d, "ic_launcher_foreground.png"))

    vals = os.path.join(ANDROID_RES, "values")
    os.makedirs(vals, exist_ok=True)
    with open(os.path.join(vals, "ic_launcher_background.xml"), "w") as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n'
                 '<!-- Generated by icon/build_icons.py. The reasoning for the\n'
                 '     colour is on BACKGROUND there; edit it, not this file. -->\n'
                 '<resources>\n'
                 f'    <color name="ic_launcher_background">{BACKGROUND}</color>\n'
                 '</resources>\n')

    anydpi = os.path.join(ANDROID_RES, "mipmap-anydpi-v26")
    os.makedirs(anydpi, exist_ok=True)
    with open(os.path.join(anydpi, "ic_launcher.xml"), "w") as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n'
                 '<!-- Generated by icon/build_icons.py.\n'
                 '\n'
                 '     This file is why the icon is not small and white. A\n'
                 '     launcher on API 26 or above prefers it over the PNG and\n'
                 '     masks these two layers to its own shape; without it the\n'
                 '     PNG gets the legacy treatment instead, which shrinks the\n'
                 '     drawing onto a white plate. minSdk is 26, so this is the\n'
                 '     icon on every device the app supports.\n'
                 '\n'
                 '     No <monochrome>: the themed icons of API 33 want a flat\n'
                 '     single-colour silhouette, and reducing this drawing to\n'
                 '     one is redrawing it rather than generating it. Absent,\n'
                 '     a themed launcher falls back to these two layers. -->\n'
                 '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
                 '    <background android:drawable="@color/ic_launcher_background"/>\n'
                 '    <foreground android:drawable="@mipmap/ic_launcher_foreground"/>\n'
                 '</adaptive-icon>\n')

    print(f"wrote the Android launcher icon: adaptive, art at {FOREGROUND_DP}dp "
           f"of 108, on {BACKGROUND}")


def main():
    master = Image.open(MASTER).convert("RGBA")
    for n in (16, 24, 32, 48, 64, 128, 256, 512):
        im = master.resize((n, n), Image.LANCZOS)
        # Retouched where the shrink is violent enough to need it, left alone
        # where the drawing still has room to speak for itself.
        if n in TOUCH:
            im = retouch(im, TOUCH[n])
        im.save(os.path.join(HERE, f"hydra-{n}.png"))

    print("wrote hydra-16 … hydra-512")
    build_android(master)

if __name__ == "__main__":
    main()

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
# inside that.
#
# **The number that settles this is not a geometric limit, it is what the
# icons beside it do.** Pulled from the test handset and measured, the drawn
# extent of each foreground within its own 108dp canvas:
#
#     Chrome              52dp
#     Samsung Internet    48dp
#     this, first attempt 66dp
#
# The first attempt was *larger than both* and was reported from the phone as
# smaller than both, which is the whole lesson: on an adaptive icon the thing
# a person sees the size of is the **plate**, not the drawing. Chrome's white
# and Samsung's blue fill the mask edge to edge, so the icon reads as a full
# disc with a logo inside it, and the logo being 48dp costs nothing.
#
# So the drawing is sized to sit *within* a plate rather than to reach the
# mask, and 62 is that: a clear ring of colour all round on every mask shape,
# no clipping under any of them, and the artwork still a fifth larger than
# Chrome's. The old reasoning -- push the drawing out until a circular mask
# starts cutting it, which happens at 68 -- was answering a question nobody
# had asked.
FOREGROUND_DP = 62

# **The plate, and it is the reason the first attempt failed.**
#
# It was #1B0A28, chosen from the drawing's own darkest ink so that the heavy
# outline would stop reading as an outline. It did, and it took the icon's
# edge with it: the artwork's outer rim *is* dark linework, so against a dark
# wallpaper a near-black plate has no visible boundary at all, and what reads
# as the icon shrinks to wherever the bright ink starts. The white plate it
# replaced was ugly and did bound the icon, which is why removing it made
# things worse rather than better.
#
# A plate has to be visible against the wallpaper or it is not a plate. This
# is a violet from the artwork's own mid-tones -- the drawing is full of
# #7161a7 and #655fa7 -- dark enough to be the "darker, not white" that was
# asked for, and light enough to hold an edge on a dark home screen. Checked
# by rendering the candidates on both a dark and a light backdrop, which is
# the comparison the first attempt never made.
BACKGROUND = "#4B2E83"

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

    The app's minSdk is 28 -- read from a built package with `aapt2 dump
    badging`, which is the only source that cannot be stale -- and adaptive
    icons arrived at 26. So `mipmap-anydpi-v26/ic_launcher.xml` covers every
    device this app can be installed on, with two releases to spare. The PNGs
    stay as the default-configuration fallback; nothing that can run Hydra
    will pick them.

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
        # it. Unreachable at this minSdk and written anyway, because a resource
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
                 '     drawing onto a white plate. minSdk is 28 and adaptive\n'
                 '     icons arrived at 26, so this is the icon on every device\n'
                 '     the app supports.\n'
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

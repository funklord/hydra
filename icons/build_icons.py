#!/usr/bin/env python3
"""Build the icon set from the master artwork.

    python3 icons/build_icons.py

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
"""
import os
from PIL import Image, ImageEnhance, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
MASTER = os.path.join(HERE, "hydra-master.png")

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

if __name__ == "__main__":
    main()

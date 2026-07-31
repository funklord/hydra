#!/usr/bin/env python3
"""Build the icon set from the master artwork.

    python3 icons/build_icons.py

Two cuts, because one drawing cannot serve the whole range.

**48px and up** come from `hydra-master.png` by downscaling. The master is the
artwork with its white plate and cream halo flood-filled away and then cropped
hard to the ink: the original render was 1024x1024 but only 774x853 of it was
drawing, so a quarter of every icon would otherwise have been spent on nothing.
The artwork is taller than it is wide, and an icon slot is square, so it is
squashed the last 9% rather than letterboxed — at these sizes the distortion is
invisible and the recovered margin is not.

**32px** is the same downscale plus a light unsharp pass. Downscaling loses
local contrast and 32 is where that starts to matter; the pass is deliberately
not applied below that, where it only adds confetti.

**16px is drawn here, pixel by pixel.** No resampling. At that size a downscale
spends most of its budget on antialiased grey belonging to no shape, and the
result is a speck: the render's three heads, four eyes and flame swirl have to
land on 256 pixels, and they cannot. What is drawn instead keeps only what
survives — three green heads, lit eyes, a fire body, water curling up the left —
in full-strength colour with one-pixel features and outline only where two
fills meet.
"""
import os
from PIL import Image, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
MASTER = os.path.join(HERE, "hydra-master.png")

# Sampled from the master rather than invented, so the two cuts sit together.
C = {
    '.': None,
    'K': (55, 17, 65, 255),     # plum outline
    'G': (127, 203, 155, 255),  # scale, lit
    'Y': (255, 216, 74, 255),   # eye
    'A': (255, 194, 74, 255),   # fire, bright
    'O': (255, 119, 47, 255),   # fire, mid
    'E': (233, 84, 31, 255),    # fire, deep
    'B': (76, 140, 195, 255),   # water
    'b': (47, 106, 168, 255),   # water, deep
}

ICON16 = [
    "......KGGK......",
    ".KK..KGGGGK..KK.",
    "KGGK.KGGGGK.KGGK",
    "KGYGKKGYYGKKGYGK",
    "KGGGGKGGGGKGGGGK",
    ".KGGGKKGGKKGGGK.",
    "..KKKOOAAOOKKK..",
    ".KOOOAAAAAAOOOK.",
    "KOOAAAAAAAAAAOOK",
    "KBOAAAAAAAAAAOOK",
    "KBBOAAAAAAAAOOEK",
    ".KBBOOAAAAOOOEK.",
    ".KbBBOOOOOOOEK..",
    "..KbBBOOOOOEKK..",
    "...KKbBBOOEKK...",
    ".....KKKKKK.....",
]

def draw16():
    im = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    px = im.load()
    for y, row in enumerate(ICON16):
        for x, ch in enumerate(row):
            c = C.get(ch)
            if c:
                px[x, y] = c
    return im

def main():
    master = Image.open(MASTER).convert("RGBA")
    for n in (24, 32, 48, 64, 128, 256, 512):
        im = master.resize((n, n), Image.LANCZOS)
        if n in (24, 32):
            im = im.filter(ImageFilter.UnsharpMask(radius=0.6, percent=120,
                                                    threshold=0))
        im.save(os.path.join(HERE, f"hydra-{n}.png"))
    draw16().save(os.path.join(HERE, "hydra-16.png"))
    print("wrote hydra-16 … hydra-512")

if __name__ == "__main__":
    main()

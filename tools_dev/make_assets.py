"""Builds the HUD art the overlay loads at runtime from your own extracted game
files, so no game art has to live in the repo.

    python make_assets.py <extracted recom folder> <output folder>

Writes face.rgba: 256x256 RGBA, Sora's portrait cut out of the player gauge
texture with a round mask.
"""
import os
import sys

from PIL import Image, ImageDraw

SIZE = 256
# portrait disc inside pl_gauge_tm2.dds (1024x512)
CX, CY, R = 233, 204, 106


def main():
    src, out = sys.argv[1], sys.argv[2]
    tex = Image.open(os.path.join(src, "remastered", "CARD", "9004", "CA9004.RTM", "pl_gauge_tm2.dds")).convert("RGBA")
    face = tex.crop((CX - R, CY - R, CX + R, CY + R)).resize((SIZE, SIZE), Image.LANCZOS)

    # round mask, supersampled so the edge is smooth
    big = Image.new("L", (SIZE * 4, SIZE * 4), 0)
    ImageDraw.Draw(big).ellipse((4, 4, SIZE * 4 - 5, SIZE * 4 - 5), fill=255)
    mask = big.resize((SIZE, SIZE), Image.LANCZOS)
    r, g, b, a = face.split()
    face = Image.merge("RGBA", (r, g, b, Image.composite(a, Image.new("L", a.size, 0), mask)))

    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "face.rgba"), "wb") as f:
        f.write(face.tobytes())
    face.save(os.path.join(out, "face_preview.png"))
    print("wrote", os.path.join(out, "face.rgba"))


if __name__ == "__main__":
    main()

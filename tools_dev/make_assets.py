"""Builds the HUD art the overlay loads at runtime from your own extracted game
files, so no game art has to live in the repo.

    python make_assets.py <extracted recom> <output folder> [<extracted kh2> [<openkh Apps> [<extracted kh1>]]]

Writes face.rgba: 256x256 RGBA, Sora's head cut out of the player gauge
texture, hair included.

With a KH2 folder it also writes, from field2d/us:

    kh2_gauge.rgba     the gauge sheet (tt0field.2dd), for the HP and MP gauges
    kh2_face.rgba      KH2's HUD portrait of Sora in his KH1 clothes
    cmd_<skin>.rgba    a command menu sheet (<skin>command.2dd), one a world
    cmd_<skin>.seq     that menu's layout and animation data, flattened
    kh2_font.rgba      the system font sheet (msg/us/fontimage.bar)
    kh2_font.bin       its advance widths (msg/us/fontinfo.bar)

The .seq needs OpenKH's command line tools to unpack the .2dd: pass the folder
holding OpenKh.Command.Bar.exe and OpenKh.Command.Layout.exe as a fourth
argument. Without the KH2 files the overlay draws plain shapes instead.

A fifth argument, an extracted KH1 folder, adds kh1_command.rgba: KH1's
command menu pieces, header words and icons (command2/uitex.bin).
"""
import os
import struct
import subprocess
import sys
import tempfile

from PIL import Image, ImageFilter

# KH2 command menu skins the overlay knows how to pick: the standard one and
# the worlds Re:CoM shares with KH2. Keep in step with skin_for_world().
SKINS = ["zz0", "al0", "he0", "nm0", "lm0", "hb0", "po0", "tt0", "di0"]

SIZE = 256
# portrait disc inside pl_gauge_tm2.dds (1024x512), and how much around it to take
CX, CY, R = 233, 204, 106
SPAN = 150


def cut_face(tex):
    """Sora's head out of the player gauge texture, hair and all. KH2 draws the
    portrait over the gauge with the hair spilling past the ring, so the disc
    it sits in here has to go: inside the disc everything but its black
    background is kept, outside it only hair and skin coloured pixels are (the
    rest out there is the green gauge and the white rim)."""
    import numpy as np
    px = np.asarray(tex.convert("RGBA")).astype(np.float32)
    crop = px[CY - SPAN:CY + SPAN, CX - SPAN:CX + SPAN]
    yy, xx = np.mgrid[-SPAN:SPAN, -SPAN:SPAN]
    inside = np.sqrt(xx ** 2 + yy ** 2) <= R - 1
    r, g, b, a = crop[..., 0], crop[..., 1], crop[..., 2], crop[..., 3]
    warm = (r > g * 1.12) & (r > b * 1.25) & (r > 70) & (a > 128)
    dark = (r < 40) & (g < 40) & (b < 40)
    alpha = np.where(inside, np.where(dark, 0, 255), np.where(warm, 255, 0)).astype(np.uint8)
    face = Image.fromarray(np.dstack([r, g, b, alpha]).astype(np.uint8), "RGBA")
    # soften the cut edge a little, then down to the size the overlay loads
    r_, g_, b_, a_ = face.split()
    face = Image.merge("RGBA", (r_, g_, b_, a_.filter(ImageFilter.GaussianBlur(0.8))))
    return face.resize((SIZE, SIZE), Image.LANCZOS)


def colour(c):
    """KH2 packs colours as ABGR bytes with 0x80 meaning 1.0."""
    return [((c >> s) & 0xFF) / 128.0 for s in (0, 8, 16, 24)]


def write_seq(yml_path, out_path, sheet_size):
    """Flattens an `unsequence` YAML into the binary the overlay reads. Keep in
    step with the Seq structs in src/overlay.cpp."""
    import yaml
    d = yaml.safe_load(open(yml_path))
    sprites = b""
    for s in d["Sprites"]:
        sprites += struct.pack("<4f", s["Left"], s["Top"], s["Right"], s["Bottom"])
        # how far the texture slides a frame (smoke and the like)
        sprites += struct.pack("<2f", s["UTranslation"], s["VTranslation"])
        # the four colours are the corners: top-left, top-right, bottom-left, bottom-right
        for key in ("ColorLeft", "ColorTop", "ColorRight", "ColorBottom"):
            sprites += struct.pack("<4f", *colour(s[key]))
    parts = b""
    groups = b""
    n_parts = 0
    for g in d["SpriteGroups"]:
        groups += struct.pack("<2i", n_parts, len(g))
        for part in g:
            parts += struct.pack("<4fi", part["Left"], part["Top"], part["Right"], part["Bottom"], part["SpriteIndex"])
        n_parts += len(g)
    anims = b""
    anim_groups = b""
    n_anims = 0
    for g in d["AnimationGroups"]:
        a_list = g["Animations"]
        last = max([a["FrameEnd"] for a in a_list] or [0])
        anim_groups += struct.pack("<6i", n_anims, len(a_list), 1 if g["DoNotLoop"] else 0,
                                   g["LoopStart"], g["LoopEnd"], last)
        for a in a_list:
            anims += struct.pack("<5i", a["Flags"], a["SpriteGroupIndex"], a["FrameStart"], a["FrameEnd"], a["ColorBlend"])
            anims += struct.pack("<10f", a["TranslateXStart"], a["TranslateXEnd"], a["TranslateYStart"], a["TranslateYEnd"],
                                 a["PivotXStart"], a["PivotXEnd"], a["PivotYStart"], a["PivotYEnd"],
                                 a["RotationZStart"], a["RotationZEnd"])
            anims += struct.pack("<12f", a["ScaleStart"], a["ScaleEnd"], a["ScaleXStart"], a["ScaleXEnd"],
                                 a["ScaleYStart"], a["ScaleYEnd"], a["BounceXStart"], a["BounceXEnd"],
                                 a["BounceYStart"], a["BounceYEnd"], a["BounceXCount"], a["BounceYCount"])
            anims += struct.pack("<8f", *(colour(a["ColorStart"]) + colour(a["ColorEnd"])))
        n_anims += len(a_list)
    # Sprite coordinates are for the original sheet, a power of two just big
    # enough for them; the remastered one is two or four times that.
    need = max(max(s["Left"], s["Right"]) for s in d["Sprites"])
    native = 256
    while native < need:
        native *= 2
    texel = sheet_size[0] / native
    with open(out_path, "wb") as f:
        f.write(struct.pack("<9I", 0x33514553, len(d["Sprites"]), n_parts, len(d["SpriteGroups"]), n_anims,
                            len(d["AnimationGroups"]), sheet_size[0], sheet_size[1], int(texel * 256)))
        f.write(sprites + parts + groups + anims + anim_groups)
    print("wrote", out_path)


def main():
    src, out = sys.argv[1], sys.argv[2]
    tex = Image.open(os.path.join(src, "remastered", "CARD", "9004", "CA9004.RTM", "pl_gauge_tm2.dds"))
    face = cut_face(tex)

    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "face.rgba"), "wb") as f:
        f.write(face.tobytes())
    face.save(os.path.join(out, "face_preview.png"))
    print("wrote", os.path.join(out, "face.rgba"))

    if len(sys.argv) > 3:
        sheet = Image.open(os.path.join(sys.argv[3], "remastered", "field2d", "us", "tt0field.2dd",
                                        "US_tt0field_2dd1.png")).convert("RGBA")
        assert sheet.size == (1024, 512), sheet.size
        with open(os.path.join(out, "kh2_gauge.rgba"), "wb") as f:
            f.write(sheet.tobytes())
        print("wrote", os.path.join(out, "kh2_gauge.rgba"))

        # KH2's HUD portrait of Sora in his KH1 clothes (the one he wears at the
        # start of KH2, and Limit Form's): normal, hurt and down, side by side
        faces = Image.open(os.path.join(sys.argv[3], "remastered", "obj", "P_EX100_KH1F.a.us", "-9.dds")).convert("RGBA")
        assert faces.size == (1024, 512), faces.size
        with open(os.path.join(out, "kh2_face.rgba"), "wb") as f:
            f.write(faces.tobytes())
        print("wrote", os.path.join(out, "kh2_face.rgba"))

        # KH2's system font: a grid of 28 glyphs a row, plus one advance width a glyph
        font = Image.open(os.path.join(sys.argv[3], "remastered", "msg", "us", "fontimage.bar",
                                       "US_fontimage_bar0.png")).convert("RGBA")
        assert font.size == (1024, 512), font.size
        with open(os.path.join(out, "kh2_font.rgba"), "wb") as f:
            f.write(font.tobytes())
        print("wrote", os.path.join(out, "kh2_font.rgba"))

        us = os.path.join(sys.argv[3], "remastered", "field2d", "us")
        sheet_sizes = {}
        for skin in SKINS:
            folder = os.path.join(us, skin + "command.2dd")
            sheet = Image.open(os.path.join(folder, os.listdir(folder)[0])).convert("RGBA")
            sheet_sizes[skin] = sheet.size
            with open(os.path.join(out, "cmd_%s.rgba" % skin), "wb") as f:
                f.write(sheet.tobytes())
            print("wrote", os.path.join(out, "cmd_%s.rgba" % skin))

    if len(sys.argv) > 4:
        tools = sys.argv[4]
        for skin in SKINS:
            with tempfile.TemporaryDirectory() as tmp:
                src2dd = os.path.join(sys.argv[3], "original", "field2d", "us", skin + "command.2dd")
                subprocess.run([os.path.join(tools, "OpenKh.Command.Bar.exe"), "unpack", src2dd, "-o", tmp],
                               check=True, stdout=subprocess.DEVNULL)
                yml = os.path.join(tmp, "comw.yml")
                subprocess.run([os.path.join(tools, "OpenKh.Command.Layout.exe"), "unsequence",
                                os.path.join(tmp, "comw.sqd"), "-o", yml], check=True, stdout=subprocess.DEVNULL)
                write_seq(yml, os.path.join(out, "cmd_%s.seq" % skin), sheet_sizes[skin])

        with tempfile.TemporaryDirectory() as tmp:
            info = os.path.join(sys.argv[3], "original", "msg", "us", "fontinfo.bar")
            subprocess.run([os.path.join(tools, "OpenKh.Command.Bar.exe"), "unpack", info, "-o", tmp],
                           check=True, stdout=subprocess.DEVNULL)
            widths = open(os.path.join(tmp, "sys.list"), "rb").read()
            with open(os.path.join(out, "kh2_font.bin"), "wb") as f:
                f.write(widths)
            print("wrote", os.path.join(out, "kh2_font.bin"), len(widths), "glyphs")

    if len(sys.argv) > 5:
        # KH1's command menu: the grey pieces the game tints and stacks, with
        # the sheet of header words and icons under them
        ui = os.path.join(sys.argv[5], "remastered", "command2", "uitex.bin")
        pieces = Image.open(os.path.join(ui, "-4140.dds")).convert("RGBA")
        words = Image.open(os.path.join(ui, "-20c0.dds")).convert("RGBA")
        assert pieces.size == (256, 384) and words.size == (256, 256), (pieces.size, words.size)
        # The sheets are small for a screen this size. Enlarge both four times;
        # the words and icons are flat shapes in the alpha channel, so their
        # edges can be tightened back up afterwards instead of left soft.
        k = 4
        # (premultiplied while resizing, so clear texels do not darken the edges)
        pieces = pieces.convert("RGBa").resize((256 * k, 384 * k), Image.LANCZOS).convert("RGBA")
        words = words.resize((256 * k, 256 * k), Image.BICUBIC)
        r, g, b, alpha = words.split()
        alpha = alpha.point(lambda v: max(0, min(255, int((v - 128) * 4 + 128))))
        grey = Image.new("L", words.size, 128)
        words = Image.merge("RGBA", (grey, grey, grey, alpha))
        sheet = Image.new("RGBA", (256 * k, 640 * k))
        sheet.paste(pieces, (0, 0))
        sheet.paste(words, (0, 384 * k))
        with open(os.path.join(out, "kh1_command.rgba"), "wb") as f:
            f.write(sheet.tobytes())
        print("wrote", os.path.join(out, "kh1_command.rgba"))


if __name__ == "__main__":
    main()

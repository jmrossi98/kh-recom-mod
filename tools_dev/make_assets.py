"""Builds the HUD art the overlay loads at runtime from your own extracted game
files, so no game art has to live in the repo.

    python make_assets.py <extracted recom folder> <output folder> [<extracted kh2 folder>]

Writes face.rgba: 256x256 RGBA, Sora's portrait cut out of the player gauge
texture with a round mask.

With a KH2 folder it also writes, from field2d/us:

    kh2_gauge.rgba     the gauge sheet (tt0field.2dd), for the HP and MP gauges
    kh2_command.rgba   the command menu sheet (tt0command.2dd)
    kh2_command.seq    that menu's layout and animation data, flattened

The .seq needs OpenKH's command line tools to unpack the .2dd: pass the folder
holding OpenKh.Command.Bar.exe and OpenKh.Command.Layout.exe as a fourth
argument. Without the KH2 files the overlay draws plain shapes instead.
"""
import os
import struct
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw

SIZE = 256
# portrait disc inside pl_gauge_tm2.dds (1024x512)
CX, CY, R = 233, 204, 106


def colour(c):
    """KH2 packs colours as ABGR bytes with 0x80 meaning 1.0."""
    return [((c >> s) & 0xFF) / 128.0 for s in (0, 8, 16, 24)]


def write_seq(yml_path, out_path):
    """Flattens an `unsequence` YAML into the binary the overlay reads. Keep in
    step with the Seq structs in src/overlay.cpp."""
    import yaml
    d = yaml.safe_load(open(yml_path))
    sprites = b""
    for s in d["Sprites"]:
        sprites += struct.pack("<4f", s["Left"], s["Top"], s["Right"], s["Bottom"])
        # the four colours are the corners: top-left, top-right, bottom-left, bottom-right
        for key in ("ColorLeft", "ColorTop", "ColorRight", "ColorBottom"):
            c = colour(s[key])
            # Scrolling sprites (smoke and the like) wrap their texture inside the
            # sprite's rectangle as they move, which the overlay does not do. Left
            # in they smear across the sheet, so they are made invisible.
            if s["UTranslation"] or s["VTranslation"]:
                c[3] = 0.0
            sprites += struct.pack("<4f", *c)
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
    with open(out_path, "wb") as f:
        f.write(struct.pack("<6I", 0x31514553, len(d["Sprites"]), n_parts, len(d["SpriteGroups"]), n_anims,
                            len(d["AnimationGroups"])))
        f.write(sprites + parts + groups + anims + anim_groups)
    print("wrote", out_path)


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

    if len(sys.argv) > 3:
        sheet = Image.open(os.path.join(sys.argv[3], "remastered", "field2d", "us", "tt0field.2dd",
                                        "US_tt0field_2dd1.png")).convert("RGBA")
        assert sheet.size == (1024, 512), sheet.size
        with open(os.path.join(out, "kh2_gauge.rgba"), "wb") as f:
            f.write(sheet.tobytes())
        print("wrote", os.path.join(out, "kh2_gauge.rgba"))

        us = os.path.join(sys.argv[3], "remastered", "field2d", "us")
        sheet = Image.open(os.path.join(us, "tt0command.2dd", "US_tt0command_2dd0.png")).convert("RGBA")
        assert sheet.size == (1024, 512), sheet.size
        with open(os.path.join(out, "kh2_command.rgba"), "wb") as f:
            f.write(sheet.tobytes())
        print("wrote", os.path.join(out, "kh2_command.rgba"))

    if len(sys.argv) > 4:
        tools = sys.argv[4]
        with tempfile.TemporaryDirectory() as tmp:
            src2dd = os.path.join(sys.argv[3], "original", "field2d", "us", "tt0command.2dd")
            subprocess.run([os.path.join(tools, "OpenKh.Command.Bar.exe"), "unpack", src2dd, "-o", tmp],
                           check=True, stdout=subprocess.DEVNULL)
            yml = os.path.join(tmp, "comw.yml")
            subprocess.run([os.path.join(tools, "OpenKh.Command.Layout.exe"), "unsequence",
                            os.path.join(tmp, "comw.sqd"), "-o", yml], check=True, stdout=subprocess.DEVNULL)
            write_seq(yml, os.path.join(out, "kh2_command.seq"))


if __name__ == "__main__":
    main()

"""Software renderer for KH2 sequence (SQD) animations, for looking at what is
inside a .2dd before wiring any of it into the overlay.

    python kh2seq.py sheet <seq.yml> <texture.png> <out.png> [first [count]]
    python kh2seq.py one   <seq.yml> <texture.png> <out.png> <group> [frame]

The YAML comes from `OpenKh.Command.Layout unsequence`. The texture is the
remastered one, so sprite coordinates are scaled up to match it.
"""
import math
import sys

import numpy as np
import yaml
from PIL import Image

F_LINEAR = 0x1
F_BOUNCE_OFF = 0x10
F_ROTATION_OFF = 0x20
F_SCALING_OFF = 0x40
F_COLOR_FIXED = 0x80
F_COLOR_MASK = 0x400
F_TRANSLATE_FIXED = 0x1000
F_PIVOT_OFF = 0x4000


def color(c):
    return np.array([(c >> 0) & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, (c >> 24) & 0xFF], np.float32) / 128.0


def lerp(t, a, b):
    return a * (1 - t) + b * t


class Sequence:
    def __init__(self, yml, texture, native_width=None):
        d = yaml.safe_load(open(yml))
        self.sprites = d["Sprites"]
        self.groups = d["SpriteGroups"]
        self.anims = d["AnimationGroups"]
        self.tex = np.asarray(Image.open(texture).convert("RGBA"), np.float32) / 255.0
        if native_width is None:
            # original sheets are powers of two; pick the smallest that holds every sprite
            need = max(max(s["Left"], s["Right"]) for s in self.sprites)
            native_width = 1
            while native_width < need:
                native_width *= 2
        self.k = self.tex.shape[1] / native_width

    def last_frame(self, g):
        a = self.anims[g]["Animations"]
        return max(x["FrameEnd"] for x in a) if a else 0

    def quads(self, g, frame, px=0.0, py=0.0):
        """Yields (corners[4x2], uv rect, vertex colours[4x4], blend) in draw order.
        Corners are top-left, top-right, bottom-left, bottom-right."""
        grp = self.anims[g]
        anims = grp["Animations"]
        if not grp["DoNotLoop"] and anims:
            end = grp["LoopEnd"] or max(x["FrameEnd"] for x in anims)
            start = grp["LoopStart"]
            if frame >= end:
                frame = start if end <= start else start + (frame - start) % (end - start)
        for a in anims:
            if frame < a["FrameStart"] or frame > a["FrameEnd"]:
                continue
            span = a["FrameEnd"] - a["FrameStart"]
            d = (frame - a["FrameStart"]) / span if span else 0.0
            fl = a["Flags"]
            t = d if fl & F_LINEAR else (math.sin(d * math.pi - math.pi / 2) + 1) / 2

            if fl & F_TRANSLATE_FIXED:
                x, y = px + a["TranslateXStart"], py + a["TranslateYStart"]
            else:
                x = px + lerp(t, a["TranslateXStart"], a["TranslateXEnd"])
                y = py + lerp(t, a["TranslateYStart"], a["TranslateYEnd"])
            if fl & F_SCALING_OFF:
                sx = sy = 1.0
            else:
                s = lerp(t, a["ScaleStart"], a["ScaleEnd"])
                sx = s * lerp(t, a["ScaleXStart"], a["ScaleXEnd"])
                sy = s * lerp(t, a["ScaleYStart"], a["ScaleYEnd"])
            col = np.ones(4, np.float32)
            if not fl & F_COLOR_MASK:
                if fl & F_COLOR_FIXED:
                    col = color(a["ColorStart"])
                else:
                    col = lerp(t, color(a["ColorStart"]), color(a["ColorEnd"]))
            rz = 0.0 if fl & F_ROTATION_OFF else lerp(t, a["RotationZStart"], a["RotationZEnd"])
            pvx = pvy = 0.0
            if not fl & F_PIVOT_OFF:
                pvx = lerp(t, a["PivotXStart"], a["PivotXEnd"])
                pvy = lerp(t, a["PivotYStart"], a["PivotYEnd"])
            if not fl & F_BOUNCE_OFF:
                x += math.sin(d * a["BounceXCount"] * math.pi) * lerp(t, a["BounceXStart"], a["BounceXEnd"])
                y += math.sin(d * a["BounceYCount"] * math.pi) * lerp(t, a["BounceYStart"], a["BounceYEnd"])

            c, s_ = math.cos(-rz), math.sin(-rz)
            for part in self.groups[a["SpriteGroupIndex"]]:
                sp = self.sprites[part["SpriteIndex"]]
                pts = []
                for cx, cy in ((part["Left"], part["Top"]), (part["Right"], part["Top"]),
                               (part["Left"], part["Bottom"]), (part["Right"], part["Bottom"])):
                    vx, vy = (cx + pvx) * sx, (cy + pvy) * sy
                    pts.append((vx * c - vy * s_ + x, vx * s_ + vy * c + y))
                cols = np.stack([color(sp["ColorLeft"]), color(sp["ColorTop"]),
                                 color(sp["ColorRight"]), color(sp["ColorBottom"])]) * col
                yield np.array(pts, np.float32), (sp["Left"], sp["Top"], sp["Right"], sp["Bottom"]), cols, a["ColorBlend"]

    def draw(self, canvas, g, frame, px, py, scale=1.0):
        """Rasterises onto an RGBA float canvas. Slow and simple: inverse-maps
        every pixel in each quad's bounding box."""
        H, W = canvas.shape[:2]
        th, tw = self.tex.shape[:2]
        for pts, uv, cols, blend in self.quads(g, frame, px, py):
            pts = pts * scale
            p0, p1, p2 = pts[0], pts[1], pts[2]
            e1, e2 = p1 - p0, p2 - p0
            det = e1[0] * e2[1] - e1[1] * e2[0]
            if abs(det) < 1e-6:
                continue
            x0, y0 = np.floor(pts.min(0)).astype(int)
            x1, y1 = np.ceil(pts.max(0)).astype(int)
            x0, y0, x1, y1 = max(x0, 0), max(y0, 0), min(x1, W), min(y1, H)
            if x0 >= x1 or y0 >= y1:
                continue
            gx, gy = np.meshgrid(np.arange(x0, x1) + 0.5, np.arange(y0, y1) + 0.5)
            dx, dy = gx - p0[0], gy - p0[1]
            u = (dx * e2[1] - dy * e2[0]) / det
            v = (dy * e1[0] - dx * e1[1]) / det
            inside = (u >= 0) & (u <= 1) & (v >= 0) & (v <= 1)
            if not inside.any():
                continue
            l, t, r, b = uv
            su = np.clip((l + (r - l) * u) * self.k, 0, tw - 1).astype(int)
            sv = np.clip((t + (b - t) * v) * self.k, 0, th - 1).astype(int)
            texel = self.tex[sv, su]
            uu, vv = u[..., None], v[..., None]
            tint = (cols[0] * (1 - uu) + cols[1] * uu) * (1 - vv) + (cols[2] * (1 - uu) + cols[3] * uu) * vv
            src = np.clip(texel * tint, 0, 1)
            a = src[..., 3:4] * inside[..., None]
            dst = canvas[y0:y1, x0:x1]
            if blend == 1:
                dst[..., :3] = np.clip(dst[..., :3] + src[..., :3] * a, 0, 1)
            elif blend == 2:
                dst[..., :3] = np.clip(dst[..., :3] - src[..., :3] * a, 0, 1)
            else:
                dst[..., :3] = dst[..., :3] * (1 - a) + src[..., :3] * a
            dst[..., 3:4] = np.clip(dst[..., 3:4] + a * (1 - dst[..., 3:4]), 0, 1)


def to_image(canvas, bg=(52, 56, 66)):
    out = np.zeros(canvas.shape[:2] + (3,), np.float32)
    out[:] = np.array(bg, np.float32) / 255.0
    a = canvas[..., 3:4]
    out = out * (1 - a) + canvas[..., :3] * a
    return Image.fromarray((np.clip(out, 0, 1) * 255).astype(np.uint8))


def sheet(seq, out, first=0, count=None):
    from PIL import ImageDraw
    n = len(seq.anims)
    count = min(count or n, n - first)
    cols = 8
    cw, ch = 240, 180
    rows = (count + cols - 1) // cols
    img = Image.new("RGB", (cols * cw, rows * ch), (30, 32, 38))
    dr = ImageDraw.Draw(img)
    for i in range(count):
        g = first + i
        cell = np.zeros((ch, cw, 4), np.float32)
        seq.draw(cell, g, seq.last_frame(g), cw / 2, ch / 2)
        img.paste(to_image(cell), ((i % cols) * cw, (i // cols) * ch))
        dr.rectangle(((i % cols) * cw, (i // cols) * ch, (i % cols) * cw + cw - 1, (i // cols) * ch + ch - 1), outline=(70, 74, 84))
        dr.text(((i % cols) * cw + 4, (i // cols) * ch + 2), str(g), fill=(255, 220, 90))
    img.save(out)


if __name__ == "__main__":
    mode, yml, tex, out = sys.argv[1:5]
    seq = Sequence(yml, tex)
    print("%d sprites, %d sprite groups, %d animation groups, texture scale %.1f"
          % (len(seq.sprites), len(seq.groups), len(seq.anims), seq.k))
    if mode == "sheet":
        first = int(sys.argv[5]) if len(sys.argv) > 5 else 0
        count = int(sys.argv[6]) if len(sys.argv) > 6 else None
        sheet(seq, out, first, count)
    else:
        g = int(sys.argv[5])
        frame = int(sys.argv[6]) if len(sys.argv) > 6 else seq.last_frame(g)
        canvas = np.zeros((448, 640, 4), np.float32)
        seq.draw(canvas, g, frame, 320, 224)
        to_image(canvas).save(out)

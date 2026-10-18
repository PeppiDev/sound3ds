#!/usr/bin/env python3
"""Ícono, banner y sonido de Sound3DS QR (usa el render del cubo de tools/banner)."""
import os, subprocess, wave
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
CUBE = os.path.join(ROOT, "..", "tools", "banner", "cube_nonotes.png")
FONT = "/usr/share/fonts/rsms-inter-fonts/InterDisplay-ExtraBold.ttf"
PURPLE = (168, 85, 247)


def qr_image(text, box):
    tmp = os.path.join(HERE, "_qr.png")
    subprocess.run(["qrencode", "-s", "1", "-m", "0", "-o", tmp, text], check=True)
    q = Image.open(tmp).convert("L")
    os.remove(tmp)
    return q.resize((box, box), Image.NEAREST)


def background(w, h, s):
    bg = Image.new("RGB", (w * s, h * s))
    px = bg.load()
    for y in range(h * s):
        t = y / (h * s)
        for x in range(w * s):
            dx = (x - w * s * 0.5) / (w * s * 0.5)
            dy = (y - h * s * 0.4) / (h * s * 0.6)
            g = max(0, 1 - (dx * dx + dy * dy)) ** 1.6
            px[x, y] = (min(int(14 + 10 * t + 70 * g), 255), min(int(12 + 6 * t + 25 * g), 255),
                        min(int(24 + 18 * t + 110 * g), 255))
    return bg.convert("RGBA")


# Banner: el cubo a la izquierda mirando un QR, y el título abajo
W, H, S = 256, 128, 3
bg = background(W, H, S)
cube = Image.open(CUBE).convert("RGBA")
crop = cube.crop((int(cube.width * 0.2), 0, int(cube.width * 0.8), cube.height))
cw = int(W * S * 0.52)
ch = int(crop.height * cw / crop.width)
bg.alpha_composite(crop.resize((cw, ch), Image.LANCZOS), (int(2 * S), int(-2 * S)))
qr = qr_image("Sound3DS QR", int(64 * S))
card = Image.new("RGBA", (qr.width + 12 * S, qr.height + 12 * S), (255, 255, 255, 255))
mask = Image.new("L", card.size, 0)
ImageDraw.Draw(mask).rounded_rectangle((0, 0, card.width - 1, card.height - 1), radius=8 * S, fill=255)
card.putalpha(mask)
qr_rgba = Image.merge("RGBA", (qr.point(lambda v: 30 if v < 128 else 255),) * 3 + (Image.new("L", qr.size, 255),))
qr_rgba = Image.composite(Image.new("RGBA", qr.size, (40, 16, 70, 255)), Image.new("RGBA", qr.size, (255, 255, 255, 255)),
                          qr.point(lambda v: 255 if v < 128 else 0))
card.alpha_composite(qr_rgba, (6 * S, 6 * S))
glow = Image.new("RGBA", bg.size, (0, 0, 0, 0))
cx, cy = int(150 * S), int(12 * S)
ImageDraw.Draw(glow).rounded_rectangle((cx - 4 * S, cy - 4 * S, cx + card.width + 4 * S, cy + card.height + 4 * S),
                                       radius=10 * S, fill=PURPLE + (170,))
bg.alpha_composite(glow.filter(ImageFilter.GaussianBlur(6 * S)))
bg.alpha_composite(card, (cx, cy))
font = ImageFont.truetype(FONT, 22 * S)
d = ImageDraw.Draw(bg)
text = "Sound3DS QR"
tw = d.textlength(text, font=font)
tx, ty = (W * S - tw) / 2, 96 * S
g2 = Image.new("RGBA", bg.size, (0, 0, 0, 0))
ImageDraw.Draw(g2).text((tx, ty), text, font=font, fill=PURPLE + (210,))
bg.alpha_composite(g2.filter(ImageFilter.GaussianBlur(5 * S)))
d = ImageDraw.Draw(bg)
d.text((tx, ty + S), text, font=font, fill=(40, 10, 70, 255))
d.text((tx, ty), text, font=font, fill=(255, 255, 255, 255))
bg.resize((W, H), Image.LANCZOS).convert("RGB").save(os.path.join(HERE, "banner.png"))

# Ícono 48x48: QR blanco sobre morado con una corchea
S2 = 8
ic = Image.new("RGBA", (48 * S2, 48 * S2), (0, 0, 0, 0))
d = ImageDraw.Draw(ic)
d.rounded_rectangle((0, 0, 48 * S2 - 1, 48 * S2 - 1), radius=10 * S2, fill=(124, 58, 237, 255))
q = qr_image("S3DS", 30 * S2)
white = Image.new("RGBA", q.size, (255, 255, 255, 255))
ic.paste(white, (9 * S2, 9 * S2), q.point(lambda v: 255 if v < 128 else 0))
d.ellipse((28 * S2, 31 * S2, 40 * S2, 41 * S2), fill=(34, 211, 238, 255))
d.rectangle((37 * S2, 18 * S2, 40 * S2, 36 * S2), fill=(34, 211, 238, 255))
d.polygon([(40 * S2, 18 * S2), (46 * S2, 22 * S2), (46 * S2, 26 * S2), (40 * S2, 23 * S2)], fill=(34, 211, 238, 255))
bgc = Image.new("RGB", ic.size, (16, 14, 28))
bgc.paste(ic, (0, 0), ic)
bgc.resize((48, 48), Image.LANCZOS).save(os.path.join(ROOT, "icon.png"))

# Sonido: dos bips de escáner y la respuesta del cubo (Sol-Do agudo)
SR, DUR = 22050, 1.6
t = np.arange(int(SR * DUR)) / SR
out = np.zeros_like(t)
def env(t0, length, a=0.003, dec=8.0):
    x = t - t0
    e = np.where(x < 0, 0, np.where(x < a, x / a, np.exp(-(x - a) * dec)))
    e[x > length] = 0
    return e
for t0 in (0.0, 0.14):
    out += 0.35 * np.sign(np.sin(2 * np.pi * 1760 * t)) * env(t0, 0.08, 0.002, 30)
for f, t0, a in ((783.99, 0.36, 0.5), (1046.5, 0.5, 0.55), (1567.98, 0.66, 0.4)):
    x = t - t0
    out += a * (np.sin(2 * np.pi * f * x) + 0.3 * np.sin(2 * np.pi * f * 4 * x) * np.exp(-np.clip(x, 0, None) * 18)) * env(t0, 1.0, 0.003, 4.5)
out *= np.clip((DUR - t) / 0.2, 0, 1)
out = out / np.max(np.abs(out)) * 0.8
with wave.open(os.path.join(HERE, "banner.wav"), "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
    w.writeframes((out * 32767).astype(np.int16).tobytes())
print("ok")

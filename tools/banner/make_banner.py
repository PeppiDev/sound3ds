#!/usr/bin/env python3
"""Arma cia/banner.png (256x128) a partir del render del cubo.

Uso:
    blender -b --factory-startup --python render_cube.py   # genera cube.png (Cycles, sin ventana)
    python3 make_banner.py                                  # compone el banner con fondo y título
"""
import os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "cia", "banner.png")
FONT = "/usr/share/fonts/rsms-inter-fonts/InterDisplay-ExtraBold.ttf"
W, H, S = 256, 128, 3  # se dibuja al triple y se reduce para suavizar bordes

bg = Image.new("RGB", (W * S, H * S))
px = bg.load()
for y in range(H * S):
    t = y / (H * S)
    for x in range(W * S):
        dx = (x - W * S / 2) / (W * S * 0.45)
        dy = (y - H * S * 0.38) / (H * S * 0.55)
        g = max(0, 1 - (dx * dx + dy * dy)) ** 1.6   # resplandor morado detrás del cubo
        px[x, y] = (min(int(14 + 10 * t + 70 * g), 255), min(int(12 + 6 * t + 25 * g), 255),
                    min(int(24 + 18 * t + 110 * g), 255))

cube = Image.open(os.path.join(HERE, "cube.png")).convert("RGBA")
cw = int(W * S * 0.94)
ch = int(cube.height * cw / cube.width)
cube = cube.resize((cw, ch), Image.LANCZOS)
bg = bg.convert("RGBA")
bg.alpha_composite(cube, ((W * S - cw) // 2, -1 * S))

font = ImageFont.truetype(FONT, 25 * S) if os.path.exists(FONT) else ImageFont.load_default()
d = ImageDraw.Draw(bg)
text = "Sound3DS"
w = d.textlength(text, font=font)
x, y = (W * S - w) / 2, 93 * S
glow = Image.new("RGBA", bg.size, (0, 0, 0, 0))
ImageDraw.Draw(glow).text((x, y), text, font=font, fill=(168, 85, 247, 210))
bg.alpha_composite(glow.filter(ImageFilter.GaussianBlur(5 * S)))
d = ImageDraw.Draw(bg)
d.text((x, y + S), text, font=font, fill=(40, 10, 70, 255))
d.text((x, y), text, font=font, fill=(255, 255, 255, 255))
bg.resize((W, H), Image.LANCZOS).convert("RGB").save(OUT)
print("banner:", os.path.normpath(OUT))

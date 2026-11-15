#!/usr/bin/env python3
"""Reempaqueta un .bcfnt de mkbcfnt (siempre una hoja A4 de 1024x1024, 512 KB)
en hojas más chicas con solo las celdas usadas.

Uso: repack_bcfnt.py entrada.bcfnt salida.bcfnt [ancho_hoja alto_hoja] [--png vista.png]
"""
import struct
import sys


def morton_tables(w, h):
    """Índice de píxel (en el orden de mosaico 8x8 de la GPU) para cada (x, y)."""
    idx = [[0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            tile = (y // 8) * (w // 8) + (x // 8)
            ix, iy = x & 7, y & 7
            m = 0
            for b in range(3):
                m |= ((ix >> b) & 1) << (2 * b)
                m |= ((iy >> b) & 1) << (2 * b + 1)
            idx[y][x] = tile * 64 + m
    return idx


def decode_a4(data, w, h):
    t = morton_tables(w, h)
    img = [[0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            i = t[y][x]
            b = data[i >> 1]
            img[y][x] = (b >> 4) if (i & 1) else (b & 0xF)
    return img


def encode_a4(img, w, h):
    t = morton_tables(w, h)
    out = bytearray(w * h // 2)
    for y in range(h):
        for x in range(w):
            i = t[y][x]
            v = img[y][x] & 0xF
            out[i >> 1] |= (v << 4) if (i & 1) else v
    return bytes(out)


def main():
    args = [a for a in sys.argv[1:]]
    png = None
    if "--png" in args:
        k = args.index("--png")
        png = args[k + 1]
        del args[k:k + 2]
    src, dst = args[0], args[1]
    nw = int(args[2]) if len(args) > 2 else 256
    nh = int(args[3]) if len(args) > 3 else 256

    b = bytearray(open(src, "rb").read())
    t = b.find(b"TGLP")
    cw, ch, base, maxw, sheet_size = b[t + 8], b[t + 9], b[t + 10], b[t + 11], struct.unpack_from("<I", b, t + 12)[0]
    nsheets, fmt, cols, rows, sw, shh = struct.unpack_from("<HHHHHH", b, t + 16)
    sheet_off = struct.unpack_from("<I", b, t + 28)[0]
    assert fmt == 11 and nsheets == 1, (fmt, nsheets)

    # Cantidad de glifos: el mayor índice que aparece en las tablas CWDH
    finf = b.find(b"FINF")
    cwdh = struct.unpack_from("<I", b, finf + 0x14)[0]   # apunta a los datos (tras magia y tamaño)
    nglyphs = 0
    while cwdh:
        start, end, nxt = struct.unpack_from("<HHI", b, cwdh)
        nglyphs = max(nglyphs, end + 1)
        cwdh = nxt

    pw, ph = cw + 1, ch + 1
    img = decode_a4(b[sheet_off:sheet_off + sheet_size], sw, shh)
    ncols, nrows = nw // pw, nh // ph
    per = ncols * nrows
    count = (nglyphs + per - 1) // per
    sheets = [[[0] * nw for _ in range(nh)] for _ in range(count)]
    for g in range(nglyphs):
        sx, sy = (g % cols) * pw, (g // cols) * ph
        s, k = divmod(g, per)
        dx, dy = (k % ncols) * pw, (k // ncols) * ph
        for yy in range(ph):
            row_src = img[sy + yy]
            row_dst = sheets[s][dy + yy]
            row_dst[dx:dx + pw] = row_src[sx:sx + pw]
    data = b"".join(encode_a4(sh, nw, nh) for sh in sheets)

    # Se reemplaza la hoja (en el mismo lugar) y se corren los punteros de las
    # secciones que vienen después (CWDH y CMAP), más el tamaño total del archivo
    old_end = sheet_off + sheet_size
    delta = len(data) - sheet_size
    out = bytearray(b[:sheet_off] + data + b[old_end:])

    def fix(pos):
        v = struct.unpack_from("<I", out, pos)[0]
        if v >= old_end:
            struct.pack_into("<I", out, pos, v + delta)
            return v + delta
        return v

    cw_ptr = fix(finf + 0x14)
    cm_ptr = fix(finf + 0x18)
    while cw_ptr:
        cw_ptr = fix(cw_ptr + 4)
    while cm_ptr:
        cm_ptr = fix(cm_ptr + 8)
    struct.pack_into("<I", out, t + 12, nw * nh // 2)
    struct.pack_into("<HHHHHH", out, t + 16, count, fmt, ncols, nrows, nw, nh)
    struct.pack_into("<I", out, 0x0C, len(out))
    open(dst, "wb").write(out)
    print(f"{nglyphs} glifos, celda {pw}x{ph}: {count} hoja(s) de {nw}x{nh} -> {len(out)} bytes (antes {len(b)})")

    if png:
        from PIL import Image
        im = Image.new("L", (nw, nh * count))
        for s, sh in enumerate(sheets):
            for y in range(nh):
                for x in range(nw):
                    im.putpixel((x, s * nh + y), sh[y][x] * 17)
        im.save(png)


if __name__ == "__main__":
    main()

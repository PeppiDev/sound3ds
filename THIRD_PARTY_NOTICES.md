# Third-party notices

Sound3DS includes or links the following third-party software. Each component keeps its own license.

| Component | Use | License |
|---|---|---|
| [minimp3](https://github.com/lieff/minimp3) (`include/minimp3.h`) | MP3 decoding | CC0 1.0 (public domain) |
| [dr_flac](https://github.com/mackron/dr_libs) (`include/dr_flac.h`) | FLAC decoding | Public domain (Unlicense) or MIT-0, at your choice |
| [stb_image](https://github.com/nothings/stb) (`include/stb_image.h`) | JPEG/PNG cover art | Public domain (Unlicense) or MIT, at your choice |
| Helix AAC by RealNetworks, via [arduino-libhelix](https://github.com/pschatzmann/arduino-libhelix) (`source/helix-aac/`) | AAC-LC decoding | RealNetworks Public Source License (RPSL) 1.0 |
| [quirc](https://github.com/dlbeer/quirc) (`source/quirc/`) | QR code recognition | ISC |
| [libctru](https://github.com/devkitPro/libctru), [citro3d](https://github.com/devkitPro/citro3d), [citro2d](https://github.com/devkitPro/citro2d) | 3DS system, graphics | zlib |
| [libcurl](https://curl.se) | HTTP/HTTPS | curl license (MIT/X derivative) |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) | TLS | Apache-2.0 |
| [jansson](https://github.com/akheron/jansson) | JSON | MIT |
| [zlib](https://zlib.net) | compression (used by libcurl) | zlib |
| [Nunito](https://github.com/googlefonts/nunito) (The Nunito Project Authors, `data/ui_font.bcfnt`) | UI font (Bold, Latin subset, converted to bcfnt) | SIL Open Font License 1.1 |

## Nunito (OFL 1.1)

`data/ui_font.bcfnt` is a Latin subset of Nunito Bold (weight 700 instance of the variable font), converted to the 3DS bcfnt format with `mkbcfnt` and repacked by `tools/uifont/repack_bcfnt.py`. Nunito is Copyright 2014 The Nunito Project Authors (https://github.com/googlefonts/nunito) and is licensed under the SIL Open Font License, Version 1.1; the full text is in [`tools/uifont/OFL-Nunito.txt`](tools/uifont/OFL-Nunito.txt). The converted font is a Modified Version under the OFL and is not sold by itself.

## Helix AAC (RPSL)

The files in `source/helix-aac/` are Portions Copyright (c) 1995-2005 RealNetworks, Inc. All Rights Reserved, and are distributed under the RealNetworks Public Source License Version 1.0 (the "RPSL"), available at https://www.helixcommunity.org/content/rpsl. The original license header is kept in every file. The changes made for Sound3DS (SBR files removed, ARM assembly path enabled for the 3DS, `fft.c` renamed to `aacfft.c`, Arduino headers replaced by standard ones) are listed in [`source/helix-aac/README.md`](source/helix-aac/README.md), and the modified source is available in this repository under the same license.

## quirc (ISC)

```
quirc -- QR-code recognition library
Copyright (C) 2010-2012 Daniel Beer <dlbeer@gmail.com>

ISC License
===========

Permission to use, copy, modify, and/or distribute this software for
any purpose with or without fee is hereby granted, provided that the
above copyright notice and this permission notice appear in all
copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.
```

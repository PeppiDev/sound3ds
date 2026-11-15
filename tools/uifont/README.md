# Fuente de la interfaz

`data/ui_font.bcfnt` es Nunito Bold (OFL 1.1, ver `OFL-Nunito.txt`) reducida a los
caracteres de `codepoints.txt` (ASCII, Latin-1 y algo de puntuación tipográfica).
Los textos con otros caracteres usan la fuente del sistema (ver `pick_font` en
`source/gfx.c`).

Para regenerarla:

```sh
# 1. Instancia estática de peso 700 desde la fuente variable de Google Fonts
python3 -m fontTools.varLib.instancer Nunito[wght].ttf wght=700 -o Nunito-Bold.ttf

# 2. bcfnt de 18 px solo con los caracteres de la lista (devkitPro tools)
mkbcfnt -s 18 -w tools/uifont/codepoints.txt -o ui18.bcfnt Nunito-Bold.ttf

# 3. mkbcfnt siempre deja una hoja de 1024x1024 (512 KB); se reempaqueta en
#    hojas de 512x256 con solo las celdas usadas (~130 KB)
python3 tools/uifont/repack_bcfnt.py ui18.bcfnt data/ui_font.bcfnt 512 256
```

El Makefile convierte `data/ui_font.bcfnt` en `ui_font_bcfnt.h` con bin2o.
Si cambia el tamaño, el texto se escala solo: `s_ui_k` iguala la altura de
Nunito a la de la fuente del sistema.

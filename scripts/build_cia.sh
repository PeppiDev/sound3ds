#!/usr/bin/env bash
# Compila Sound3DS y lo empaqueta como .cia instalable con FBI.
# Requiere makerom (github.com/3DSGuy/Project_CTR) y bannertool en el equipo anfitrión;
# las rutas se pueden cambiar con MAKEROM / BANNERTOOL.
set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS_DIR="${CTR_TOOLS_DIR:-$HOME/.local/share/3ds-tools}"
MAKEROM="${MAKEROM:-$TOOLS_DIR/makerom/makerom}"
BANNERTOOL="${BANNERTOOL:-$TOOLS_DIR/bannertool/linux-x86_64/bannertool}"
VERSION="${VERSION:-$(grep -m1 -oP '^## \[\K[0-9]+\.[0-9]+\.[0-9]+' "$PROJECT_DIR/CHANGELOG.md")}"
IFS=. read -r V_MAJOR V_MINOR V_MICRO <<< "$VERSION"

for tool in "$MAKEROM" "$BANNERTOOL"; do
    if [ ! -x "$tool" ]; then
        echo "Error: no se encontró $tool"
        exit 1
    fi
done

"$PROJECT_DIR/scripts/build.sh"

OUT="$PROJECT_DIR/build/cia"
mkdir -p "$OUT"
cd "$PROJECT_DIR"

echo "==> [Sound3DS] Generando banner e ícono..."
# Banner 3D (modelo CGFX hecho con tools/banner3d); si falta, la imagen plana
if [ -f cia/banner.cgfx ]; then
    "$BANNERTOOL" makebanner -ci cia/banner.cgfx -a cia/banner.wav -o "$OUT/banner.bnr"
else
    "$BANNERTOOL" makebanner -i cia/banner.png -a cia/banner.wav -o "$OUT/banner.bnr"
fi
# Con banner 3D el SMDH necesita la bandera extendedbanner
SMDH_FLAGS="visible,allow3d,recordusage"
[ -f cia/banner.cgfx ] && SMDH_FLAGS="$SMDH_FLAGS,extendedbanner"
"$BANNERTOOL" makesmdh -s "Sound3DS" -l "Reproductor estetico SoundCloud" -p "PeppiDev" \
    -f "$SMDH_FLAGS" -i cia/icon.png -o "$OUT/icon.icn"

echo "==> [Sound3DS] Empaquetando sound3ds.cia v$VERSION..."
"$MAKEROM" -f cia -o sound3ds.cia -target t -exefslogo \
    -elf sound3ds.elf -rsf cia/sound3ds.rsf \
    -icon "$OUT/icon.icn" -banner "$OUT/banner.bnr" \
    -major "$V_MAJOR" -minor "$V_MINOR" -micro "$V_MICRO"

ls -la sound3ds.cia

#!/usr/bin/env bash
# Empaqueta Sound3DS QR como .cia (requiere makerom y bannertool, igual que scripts/build_cia.sh)
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS_DIR="${CTR_TOOLS_DIR:-$HOME/.local/share/3ds-tools}"
MAKEROM="${MAKEROM:-$TOOLS_DIR/makerom/makerom}"
BANNERTOOL="${BANNERTOOL:-$TOOLS_DIR/bannertool/linux-x86_64/bannertool}"
VERSION="${VERSION:-1.0.0}"
IFS=. read -r V_MAJOR V_MINOR V_MICRO <<< "$VERSION"

"$DIR/build.sh"
mkdir -p "$DIR/build/cia"
cd "$DIR"
"$BANNERTOOL" makebanner -i cia/banner.png -a cia/banner.wav -o build/cia/banner.bnr
"$BANNERTOOL" makesmdh -s "Sound3DS QR" -l "Importa canciones por QR" -p "PeppiDev" \
    -i icon.png -o build/cia/icon.icn
"$MAKEROM" -f cia -o sound3ds-qr.cia -target t -exefslogo \
    -elf sound3ds-qr.elf -rsf cia/sound3ds-qr.rsf \
    -icon build/cia/icon.icn -banner build/cia/banner.bnr \
    -major "$V_MAJOR" -minor "$V_MINOR" -micro "$V_MICRO"
ls -la sound3ds-qr.cia

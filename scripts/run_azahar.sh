#!/usr/bin/env bash
set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$PROJECT_DIR/sound3ds.3dsx"

if [ ! -f "$BIN" ]; then
    echo "Error: $BIN no existe. Compila primero con ./scripts/build.sh"
    exit 1
fi

echo "==> [Sound3DS] Lanzando en emulador Azahar..."
flatpak run org.azahar_emu.Azahar "$BIN"

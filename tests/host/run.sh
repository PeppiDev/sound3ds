#!/usr/bin/env bash
# Compila y ejecuta en el PC la prueba del camino HLS/AAC, y compara el PCM de
# Helix con el que produce ffmpeg a partir de los mismos segmentos.
# Uso: tests/host/run.sh <dir con playlist.m3u8, init.mp4, seg0.m4s...>
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DATA="$1"
OUT="$(mktemp -d)"
H="$ROOT/source/helix-aac"

gcc -O2 -Wall -DUSE_DEFAULT_STDLIB -I"$ROOT/source" -I"$H" -I"$H/compat" \
    "$ROOT/tests/host/test_hls_aac.c" "$ROOT/source/fmp4.c" "$ROOT/source/hls.c" \
    $(ls "$H"/*.c) -o "$OUT/test_hls_aac" -lm
"$OUT/test_hls_aac" "$DATA"

# Referencia: ffmpeg decodifica init + segmentos concatenados
cat "$DATA/init.mp4" $(ls "$DATA"/seg*.m4s | sort -V) > "$OUT/joined.mp4"
ffmpeg -v error -y -i "$OUT/joined.mp4" -f s16le -ac 2 -ar 44100 "$OUT/ref.pcm"
python3 -I - "$DATA/out.pcm" "$OUT/ref.pcm" <<'PY'
import sys, numpy as np
a = np.fromfile(sys.argv[1], dtype=np.int16).astype(np.float64)
b = np.fromfile(sys.argv[2], dtype=np.int16).astype(np.float64)
# Alinear por correlación (ffmpeg descarta el retardo inicial del codificador)
n = min(len(a), len(b), 44100 * 2 * 4)
best = max(range(0, 4096 * 2, 2), key=lambda k: np.dot(a[k:k + n // 2], b[:n // 2]))
a2 = a[best:]
m = min(len(a2), len(b))
err = a2[:m] - b[:m]
snr = 10 * np.log10(np.sum(b[:m] ** 2) / max(np.sum(err ** 2), 1e-9))
print(f"helix={len(a)//2} muestras, ffmpeg={len(b)//2}, desfase={best//2}, SNR={snr:.1f} dB")
sys.exit(0 if snr > 40 else 1)
PY

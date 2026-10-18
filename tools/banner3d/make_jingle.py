#!/usr/bin/env python3
"""Jingle del menú HOME con instrumentos MIDI reales (FluidR3_GM) y voz (Piper).

Requiere: pyfluidsynth + numpy, la soundfont FluidR3_GM, sox, y una voz Piper ya
generada (VOICE_WAV, por ejemplo con:
  echo "Sound three D S!" | piper -m en_US-amy-medium.onnx -f voz.wav).

    python make_jingle.py voz.wav ../../cia/banner.wav
"""
import subprocess, sys, wave
import numpy as np
import fluidsynth

SF2 = "/usr/share/soundfonts/FluidR3_GM.sf2"
SR = 32000
DUR = 2.9
voice_in, out_path = sys.argv[1], sys.argv[2]

fs = fluidsynth.Synth(samplerate=float(SR), gain=0.6)
sf = fs.sfload(SF2)
GLOCK, KALIMBA, SLAP, PAD, BELLS = 0, 1, 2, 3, 4
fs.program_select(GLOCK, sf, 0, 9)      # Glockenspiel
fs.program_select(KALIMBA, sf, 0, 108)  # Kalimba
fs.program_select(SLAP, sf, 0, 36)      # Slap Bass 1
fs.program_select(PAD, sf, 0, 89)       # Pad 2 (warm)
fs.program_select(BELLS, sf, 0, 112)    # Tinkle Bell
DRUMS = 9
fs.program_select(DRUMS, sf, 128, 0)    # kit estándar

# (segundos, canal, nota, velocidad, duración)
E = 0.115  # semicorchea a ~130 bpm
ev = []
def n(t, ch, note, vel, dur):
    ev.append((t, "on", ch, note, vel)); ev.append((t + dur, "off", ch, note, 0))

# Arranque: arpegio que sube (como el cubo saltando) sobre bajo y batería
for i, note in enumerate((67, 72, 76, 79, 84)):       # Sol-Do-Mi-Sol-Do
    n(i * E, GLOCK, note + 12 if i == 4 else note, 105, 0.5)
    n(i * E, KALIMBA, note, 90, 0.4)
n(0.0, SLAP, 36, 110, 0.22); n(2 * E, SLAP, 43, 100, 0.2); n(4 * E, SLAP, 48, 115, 0.5)
n(0.0, DRUMS, 36, 115, 0.1)                           # bombo
n(2 * E, DRUMS, 42, 70, 0.1)                          # hi-hat
n(3 * E, DRUMS, 42, 60, 0.1)
n(4 * E, DRUMS, 38, 110, 0.1)                         # caja
n(4 * E, DRUMS, 49, 95, 1.2)                          # platillo
# Acorde de Do mayor sostenido bajo la voz
for note in (60, 64, 67, 72):
    n(0.55, PAD, note, 70, 1.6)
n(0.55, SLAP, 36, 90, 0.9)
# Destello final
for i, note in enumerate((96, 100, 103, 108)):
    n(2.05 + i * 0.07, BELLS, note, 85, 0.6)
n(2.05, GLOCK, 84, 90, 0.8)
ev.sort(key=lambda e: e[0])

audio = []
t_now = 0.0
for t, kind, ch, note, vel in ev + [(DUR, "end", 0, 0, 0)]:
    samples = int(round((t - t_now) * SR))
    if samples > 0:
        audio.append(np.array(fs.get_samples(samples), dtype=np.float32).reshape(-1, 2).mean(axis=1))
        t_now += samples / SR
    if kind == "on":
        fs.noteon(ch, note, vel)
    elif kind == "off":
        fs.noteoff(ch, note)
fs.delete()
music = np.concatenate(audio) / 32768.0

# Voz: más aguda (voz del cubo), algo de brillo, a 32 kHz mono
subprocess.run(["sox", voice_in, "-r", str(SR), "-c", "1", "/tmp/_voice.wav", "pitch", "350",
                "treble", "4", "norm", "-1"], check=True)
with wave.open("/tmp/_voice.wav") as w:
    voice = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float32) / 32768.0
# recortar silencios de la voz
idx = np.where(np.abs(voice) > 0.02)[0]
voice = voice[max(idx[0] - 200, 0): idx[-1] + 400]

mix = np.zeros(int(DUR * SR), dtype=np.float32)
mix[:len(music)] += music[:len(mix)] * 1.0
start = int(0.62 * SR)
end = min(start + len(voice), len(mix))
mix[start:end] *= 0.55                     # bajar la música bajo la voz
mix[start:end] += voice[:end - start] * 0.95
fade = int(0.2 * SR)
mix[-fade:] *= np.linspace(1, 0, fade)
mix = mix / np.max(np.abs(mix)) * 0.9
with wave.open(out_path, "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
    w.writeframes((mix * 32767).astype(np.int16).tobytes())
print("jingle:", out_path, round(len(mix) / SR, 2), "s; voz", round(len(voice) / SR, 2), "s")

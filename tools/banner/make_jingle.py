#!/usr/bin/env python3
"""Genera cia/banner.wav: el sonido de Sound3DS al elegirlo en el menú HOME (2,4 s).

Un "bloop" de salto del cubo, un arpegio tipo marimba (Do-Mi-Sol-Do) y un destello.
"""
import os
import wave
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "cia", "banner.wav")
SR, DUR = 22050, 2.4
t = np.arange(int(SR * DUR)) / SR
out = np.zeros_like(t)


def env(t0, length, attack=0.004, decay=6.0):
    x = t - t0
    e = np.where(x < 0, 0, np.where(x < attack, x / attack, np.exp(-(x - attack) * decay)))
    e[x > length] = 0
    return e


def marimba(f, t0, amp, decay=7.0):
    x = t - t0
    xp = np.clip(x, 0, None)
    s = (np.sin(2 * np.pi * f * x) + 0.35 * np.sin(2 * np.pi * f * 4 * x) * np.exp(-xp * 18)
         + 0.15 * np.sin(2 * np.pi * f * 10 * x) * np.exp(-xp * 40))
    return amp * s * env(t0, 1.4, 0.003, decay)


# Salto del cubo: barrido hacia arriba y un aterrizaje suave
f = np.where(t < 0.12, 180 + 2600 * t, 492)
out += 0.55 * np.sin(2 * np.pi * np.cumsum(f) / SR) * env(0.0, 0.16, 0.003, 22)
out += 0.35 * np.sin(2 * np.pi * 90 * t) * env(0.16, 0.12, 0.002, 30)
# Arpegio con respuesta
notes = [(523.25, 0.22), (659.25, 0.36), (783.99, 0.50), (1046.5, 0.66),
         (783.99, 0.92), (1046.5, 1.06), (1318.5, 1.22)]
for i, (fr, t0) in enumerate(notes):
    out += marimba(fr, t0, 0.42 if i < 6 else 0.5, 6.5 if i < 6 else 3.2)
for fr in (261.63, 329.63, 392.0):
    out += 0.07 * np.sin(2 * np.pi * fr * t) * env(1.22, 1.2, 0.05, 2.2)
# Destello final
rng = np.random.default_rng(3)
for k in range(6):
    t0 = 1.25 + k * 0.07
    fr = 2093 * (1 + 0.5 * rng.random())
    out += 0.10 * np.sin(2 * np.pi * fr * (t - t0)) * env(t0, 0.3, 0.002, 14)

out *= np.clip((DUR - t) / 0.25, 0, 1)
out = out / np.max(np.abs(out)) * 0.85
with wave.open(OUT, "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(SR)
    w.writeframes((out * 32767).astype(np.int16).tobytes())
print("jingle:", os.path.normpath(OUT))

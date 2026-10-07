#!/usr/bin/env python3
"""Erzeugt src/star_envelope.h aus dem Stern-Lied (SD-Karte 09/007.wav).

Lautstaerke-Huellkurve in 20-ms-Schritten (RMS), auf 0..255 normiert, fuer das
Pulsieren des Hauptlichts im Stern-Modus. Aufruf:
    python3 tools/make_star_envelope.py "/pfad/zu/09/007.wav"
Braucht numpy. Nach einem Tausch des Lieds neu erzeugen.
"""
import sys, wave
import numpy as np

FRAME_MS = 20
src = sys.argv[1]
w = wave.open(src)
sr, n, ch, sw = w.getframerate(), w.getnframes(), w.getnchannels(), w.getsampwidth()
assert sw == 2, "16-bit WAV erwartet"
x = np.frombuffer(w.readframes(n), dtype='<i2').astype(np.float64) / 32768
if ch > 1:
    x = x.reshape(-1, ch).mean(axis=1)
step = int(sr * FRAME_MS / 1000)
rms = np.array([np.sqrt(np.mean(x[i:i + step] ** 2)) for i in range(0, len(x) - step + 1, step)])
db = 20 * np.log10(rms + 1e-9)
# Dynamik spreizen: leiseste 5 % → 0, lauteste 2 % → 1. Ohne Spreizung waere
# das Licht praktisch dauernd fast voll, das Lied ist durchgehend laut.
lo, hi = np.percentile(db, 5), np.percentile(db, 98)
v = np.clip((db - lo) / (hi - lo), 0, 1)
# Wie eine VU-Anzeige: sofort hoch bei einem lauten Schlag, dann langsam
# abklingen. Roh springen die Werte von Frame zu Frame — auf den LEDs saehe
# das nach Flackern aus statt nach Pulsieren.
RELEASE_MS = 150
decay = np.exp(-FRAME_MS / RELEASE_MS)
for i in range(1, len(v)):
    v[i] = max(v[i], v[i - 1] * decay)
vals = np.round(v * 255).astype(int)

name = src.split('/')[-1]
lines = [f"// Automatisch erzeugt von tools/make_star_envelope.py aus {name} — nicht von Hand aendern.",
         f"// Lautstaerke-Huellkurve, {FRAME_MS} ms pro Wert, {len(vals)} Werte = {len(vals)*FRAME_MS/1000:.2f} s, 0..255.",
         "#pragma once", "#include <stdint.h>",
         f"static constexpr uint16_t STAR_ENV_FRAME_MS = {FRAME_MS};",
         f"static constexpr uint16_t STAR_ENV_LEN = {len(vals)};",
         "static const uint8_t STAR_ENV[STAR_ENV_LEN] = {"]
for i in range(0, len(vals), 20):
    lines.append("    " + ", ".join(str(int(a)) for a in vals[i:i + 20]) + ",")
lines.append("};")
out = __file__.rsplit('/tools/', 1)[0] + "/src/star_envelope.h"
open(out, "w").write("\n".join(lines) + "\n")
print(f"{out}: {len(vals)} Werte, Mittel {vals.mean():.0f}, Min {vals.min()}, Max {vals.max()}")

#!/usr/bin/env python3
"""Build the monster defeat cries from CC0 animal recordings.

The four cues follow the classic creature-sound-design recipe popularised by
films such as *Jurassic Park*: layer several real large-animal recordings,
pitch them down with a dying downward glide, shape the formants, add drive and
a short room, then let the whole thing collapse into silence.  Nothing is
synthesised from scratch except a sub-bass thump and the mechanical ring
modulation.

Trimmed 16 kHz mono source segments live in assets/audio/monster-sources/ and
are documented (with Freesound IDs and CC0 attribution) in
assets/audio/SOURCES.md.  The script only needs the Python standard library.

Segment manifest (Freesound ID, start, duration) used to cut the committed
sources; each file is trimmed with a 10 ms fade at both ends:

    559600 lion_roar_559600.wav          11.30s  1.70s
    507467 elephant_angry_507467.wav      1.45s  1.70s
    139052 elephant_scream_139052.wav     0.06s  1.25s
    571288 tiger_roar_571288.wav           0.26s  0.99s
    811790 tiger_shriek_811790.wav         0.00s  0.85s
    350424 rhino_350424.wav                8.40s  1.55s
    535810 rhino_535810.wav                0.75s  1.90s
    397054 gorilla_scream_397054.wav       0.06s  1.15s
    536761 growl_536761.wav                2.10s  1.80s
    631985 alien_snarl_631985.wav          1.50s  1.35s
    559270 hiss_559270.wav                 0.88s  1.30s

    python tools/build_monster_cries.py
    python tools/build_monster_cries.py --preview-dir .tmp/monster-cries-v2
"""

from __future__ import annotations

import argparse
import array
import math
import struct
import sys
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC_DIR = ROOT / "assets" / "audio" / "monster-sources"
OUT = ROOT / "main" / "audio" / "monster_cries.c"
RATE = 16000


# ---------------------------------------------------------------------------
# Small DSP helpers


def read_wav(path: Path) -> list[float]:
    with wave.open(str(path), "rb") as w:
        if w.getsampwidth() != 2:
            raise SystemExit(f"{path}: expected 16-bit PCM")
        raw = w.readframes(w.getnframes())
        channels = w.getnchannels()
    data = array.array("h")
    data.frombytes(raw)
    if sys.byteorder == "big":
        data.byteswap()
    if channels > 1:
        data = data[0::channels]
    return [v / 32768.0 for v in data]


class Biquad:
    """Direct-form-I RBJ biquad used for formant shaping."""

    def __init__(self, kind: str, freq: float, q: float = 0.707,
                 gain_db: float = 0.0) -> None:
        freq = min(max(freq, 20.0), RATE * 0.45)
        w0 = 2.0 * math.pi * freq / RATE
        cos_w0 = math.cos(w0)
        alpha = math.sin(w0) / (2.0 * q)
        if kind == "lowpass":
            b0 = (1.0 - cos_w0) * 0.5
            b1 = 1.0 - cos_w0
            b2 = b0
        elif kind == "highpass":
            b0 = (1.0 + cos_w0) * 0.5
            b1 = -(1.0 + cos_w0)
            b2 = b0
        elif kind == "bandpass":
            b0 = alpha
            b1 = 0.0
            b2 = -alpha
        elif kind == "peaking":
            amp = 10.0 ** (gain_db / 40.0)
            b0 = 1.0 + alpha * amp
            b1 = -2.0 * cos_w0
            b2 = 1.0 - alpha * amp
            alpha = alpha / amp
        else:
            raise ValueError(kind)
        a0 = 1.0 + alpha
        self.b0, self.b1, self.b2 = b0 / a0, b1 / a0, b2 / a0
        self.a1 = -2.0 * cos_w0 / a0
        self.a2 = (1.0 - alpha) / a0
        self.x1 = self.x2 = self.y1 = self.y2 = 0.0

    def run(self, samples: list[float]) -> list[float]:
        out = [0.0] * len(samples)
        for i, x in enumerate(samples):
            y = (self.b0 * x + self.b1 * self.x1 + self.b2 * self.x2
                 - self.a1 * self.y1 - self.a2 * self.y2)
            self.x2, self.x1 = self.x1, x
            self.y2, self.y1 = self.y1, y
            out[i] = y
        return out


def varispeed(src: list[float], n: int, rate_start: float, rate_end: float,
              vib_hz: float = 0.0, vib_depth: float = 0.0,
              phase: float = 0.0) -> list[float]:
    """Read `src` with a time-varying playback rate.

    Rate below 1 stretches the source, so the creature sounds both larger and
    slower; the linear glide toward `rate_end` is the dying fall.
    """
    out = [0.0] * n
    last = len(src) - 1
    pos = 0.0
    span = max(1, n - 1)
    for i in range(n):
        t = i / span
        rate = rate_start + (rate_end - rate_start) * t
        if vib_depth:
            rate *= 1.0 + vib_depth * math.sin(
                2.0 * math.pi * vib_hz * i / RATE + phase)
        if pos >= last:
            break
        j = int(pos)
        frac = pos - j
        out[i] = src[j] * (1.0 - frac) + src[j + 1] * frac
        pos += rate
    return out


def shape(x: list[float], attack: float, hold: float,
          release: float) -> list[float]:
    n = len(x)
    a = min(n, max(1, int(attack * RATE)))
    h = min(n, max(a + 1, int(hold * n)))
    tail = max(1, n - h)
    for i in range(n):
        if i < a:
            g = i / a
        elif i < h:
            g = 1.0
        else:
            g = (1.0 - (i - h) / tail) ** release
        x[i] *= g
    return x


def ring(x: list[float], hz: float, depth: float) -> list[float]:
    for i in range(len(x)):
        x[i] *= (1.0 - depth) + depth * math.sin(2.0 * math.pi * hz * i / RATE)
    return x


def tremolo(x: list[float], hz: float, depth: float, start: float = 0.0) -> list[float]:
    begin = int(start * len(x))
    for i in range(begin, len(x)):
        x[i] *= 1.0 - depth * (0.5 + 0.5 * math.sin(
            2.0 * math.pi * hz * i / RATE))
    return x


def gate(x: list[float], hz: float, depth: float, start: float = 0.0) -> list[float]:
    begin = int(start * len(x))
    for i in range(begin, len(x)):
        pulse = 1.0 if math.sin(2.0 * math.pi * hz * i / RATE) > 0 else 0.0
        x[i] *= 1.0 - depth + depth * pulse
    return x


def saturate(x: list[float], drive: float) -> list[float]:
    norm = math.tanh(drive) or 1.0
    for i in range(len(x)):
        x[i] = math.tanh(drive * x[i]) / norm
    return x


def comb(x: list[float], delay_s: float, feedback: float,
         damp: float) -> list[float]:
    delay = max(1, int(delay_s * RATE))
    buf = [0.0] * delay
    lp = 0.0
    idx = 0
    out = [0.0] * len(x)
    for i, s in enumerate(x):
        y = buf[idx]
        lp = y * (1.0 - damp) + lp * damp
        buf[idx] = s + lp * feedback
        idx = (idx + 1) % delay
        out[i] = y
    return out


def allpass(x: list[float], delay_s: float, gain: float) -> list[float]:
    delay = max(1, int(delay_s * RATE))
    buf = [0.0] * delay
    idx = 0
    out = [0.0] * len(x)
    for i, s in enumerate(x):
        bufout = buf[idx]
        y = -gain * s + bufout
        buf[idx] = s + gain * y
        idx = (idx + 1) % delay
        out[i] = y
    return out


def room(x: list[float], wet: float, decay: float = 0.74) -> list[float]:
    """Short Schroeder room; gives the cry a physical space to die into."""
    combs = [comb(x, d, decay, 0.35) for d in (0.0297, 0.0371, 0.0411, 0.0437)]
    mixed = [sum(v) / len(v) for v in zip(*combs)]
    mixed = allpass(mixed, 0.0050, 0.55)
    mixed = allpass(mixed, 0.0017, 0.55)
    return [x[i] * (1.0 - wet) + mixed[i] * wet for i in range(len(x))]


def compress(x: list[float], threshold_db: float = -17.0, ratio: float = 3.0,
             attack_ms: float = 5.0, release_ms: float = 120.0) -> list[float]:
    """Tame the roar transients so all four creatures hit the same loudness."""
    threshold = 10.0 ** (threshold_db / 20.0)
    a = math.exp(-1.0 / (attack_ms * 0.001 * RATE))
    r = math.exp(-1.0 / (release_ms * 0.001 * RATE))
    env = 0.0
    for i, v in enumerate(x):
        level = abs(v)
        coeff = a if level > env else r
        env = coeff * env + (1.0 - coeff) * level
        if env > threshold:
            x[i] *= (env / threshold) ** (1.0 / ratio - 1.0)
    return x


def sub_thump(n: int, gain: float, f_start: float = 96.0,
              f_end: float = 40.0, length: float = 0.55,
              delay: float = 0.0) -> list[float]:
    out = [0.0] * n
    begin = int(delay * RATE)
    m = min(int(length * RATE), n - begin)
    phase = 0.0
    for i in range(max(0, m)):
        u = i / max(1, m)
        freq = f_start + (f_end - f_start) * u
        phase += 2.0 * math.pi * freq / RATE
        env = (1.0 - math.exp(-i / (0.004 * RATE))) * math.exp(-5.5 * u)
        out[begin + i] += gain * math.sin(phase) * env
    return out


def metal_whine(n: int, gain: float, f_start: float = 1150.0,
                f_end: float = 280.0, delay: float = 0.06) -> list[float]:
    out = [0.0] * n
    begin = int(delay * RATE)
    phase = 0.0
    for i in range(begin, n):
        u = (i - begin) / max(1, n - begin)
        freq = f_start + (f_end - f_start) * u
        phase += 2.0 * math.pi * freq / RATE
        env = (1.0 - u) ** 2
        out[i] += gain * (math.sin(phase) + 0.3 * math.sin(2.0 * phase)) * env
    return out


# ---------------------------------------------------------------------------
# Source cache and layering


SOURCES: dict[str, list[float]] = {}


def load_sources() -> None:
    if not SRC_DIR.is_dir():
        raise SystemExit(f"missing source directory: {SRC_DIR}")
    for path in sorted(SRC_DIR.glob("*.wav")):
        SOURCES[path.stem] = read_wav(path)


def layer(name: str, n: int, gain: float, rate: tuple[float, float],
          vib: tuple[float, float] = (0.0, 0.0), offset: float = 0.0,
          attack: float = 0.02, hold: float = 0.55,
          release: float = 1.6, trim: tuple[float, float] | None = None,
          highpass: float | None = None, lowpass: float | None = None,
          phase: float = 0.0) -> list[float]:
    src = SOURCES[name]
    if trim is not None:
        start, length = trim
        src = src[int(start * RATE):int((start + length) * RATE)]
    sig = varispeed(src, n, rate[0], rate[1], vib[0], vib[1], phase)
    if highpass is not None:
        sig = Biquad("highpass", highpass).run(sig)
    if lowpass is not None:
        sig = Biquad("lowpass", lowpass).run(sig)
    sig = shape(sig, attack, hold, release)
    if offset > 0.0:
        shift = min(n, int(offset * RATE))
        sig = [0.0] * shift + sig[:n - shift]
    return [v * gain for v in sig]


def mix(*tracks: list[float]) -> list[float]:
    n = len(tracks[0])
    out = [0.0] * n
    for track in tracks:
        for i in range(n):
            out[i] += track[i]
    return out


def finish(x: list[float], target_rms_db: float,
           peak_limit: float) -> list[int]:
    """Loudness-match by RMS, then soft-limit only the transient peaks."""
    rms = math.sqrt(sum(v * v for v in x) / max(1, len(x)))
    scale = 10.0 ** (target_rms_db / 20.0) / max(rms, 1e-9)
    knee = peak_limit * 0.7
    n = len(x)
    fade_in = max(1, int(0.004 * RATE))
    fade_out = max(1, int(0.08 * RATE))
    out = [0] * n
    for i, v in enumerate(x):
        g = 1.0
        if i < fade_in:
            g *= i / fade_in
        if i > n - fade_out:
            g *= (n - i) / fade_out
        v *= scale
        if abs(v) > knee:
            v = math.copysign(
                knee + (peak_limit - knee) * math.tanh(
                    (abs(v) - knee) / (peak_limit - knee)), v)
        out[i] = max(-32768, min(32767, int(round(v * 32767.0 * g))))
    return out


# ---------------------------------------------------------------------------
# The four creatures


def cry_heavy(n: int) -> list[float]:
    """巨兽：狮子咆哮做主体，大象低频撑胸腔，结尾带喉音颤抖。"""
    tracks = [
        layer("lion_roar_559600", n, 1.00, (0.88, 0.72), vib=(5.5, 0.035),
              attack=0.025, hold=0.46, release=1.7, lowpass=2200),
        layer("lion_roar_559600", n, 0.80, (0.94, 0.80), vib=(8.0, 0.04),
              offset=0.01, attack=0.02, hold=0.44, release=1.8,
              highpass=500, lowpass=2900, phase=1.9),
        layer("elephant_angry_507467", n, 0.42, (0.54, 0.46), vib=(4.0, 0.03),
              attack=0.07, hold=0.40, release=1.5, highpass=90, lowpass=900),
        layer("elephant_scream_139052", n, 0.38, (0.70, 0.58),
              offset=0.04, attack=0.03, hold=0.38, release=1.8,
              highpass=300, lowpass=1700),
        layer("growl_536761", n, 0.26, (0.82, 0.68), vib=(6.5, 0.05),
              offset=0.10, attack=0.05, hold=0.35, release=2.0,
              highpass=100, lowpass=900),
        sub_thump(n, 0.22, delay=0.01),
        sub_thump(n, 0.12, delay=0.42, length=0.5),
    ]
    out = mix(*tracks)
    tremolo(out, 11.0, 0.22, start=0.45)
    saturate(out, 1.7)
    out = room(out, 0.22)
    # Small speakers lose most of the sub-bass, so lift the chest formant a
    # little to keep the beast audible without turning it into a plain roar.
    out = Biquad("peaking", 420.0, 0.9, gain_db=8.0).run(out)
    return compress(out, -17.0, 3.0, 4.0, 120.0)


def cry_winged(n: int) -> list[float]:
    """翼兽：老虎的砂纸质感加高频尖啸，颤音制造振翅般的撕裂感。"""
    tracks = [
        layer("tiger_roar_571288", n, 0.85, (1.06, 0.84), vib=(9.0, 0.05),
              attack=0.012, hold=0.48, release=1.8, highpass=360, lowpass=5200),
        layer("tiger_shriek_811790", n, 0.55, (1.28, 1.02), vib=(12.0, 0.04),
              attack=0.010, hold=0.40, release=1.9, highpass=900, lowpass=7000),
        layer("elephant_scream_139052", n, 0.34, (1.35, 1.08),
              offset=0.02, attack=0.015, hold=0.36, release=2.0,
              highpass=850, lowpass=6500),
        layer("hiss_559270", n, 0.24, (1.15, 0.95), vib=(16.0, 0.06),
              offset=0.05, attack=0.03, hold=0.30, release=2.2,
              highpass=2000, lowpass=7600),
    ]
    out = mix(*tracks)
    tremolo(out, 27.0, 0.30, start=0.10)
    saturate(out, 1.5)
    out = room(out, 0.18)
    return compress(out, -18.0, 3.5, 3.0, 100.0)


def cry_mech(n: int) -> list[float]:
    """机械虫：犀牛号角做金属骨架，环形调制加门限让声音像机械故障。"""
    tracks = [
        layer("rhino_350424", n, 0.75, (0.74, 0.60), vib=(3.5, 0.025),
              attack=0.018, hold=0.46, release=1.7, highpass=260, lowpass=3000),
        layer("rhino_350424", n, 0.42, (0.86, 0.70), vib=(11.0, 0.04),
              offset=0.01, attack=0.015, hold=0.44, release=1.8,
              highpass=800, lowpass=5500, phase=1.7),
        layer("rhino_535810", n, 0.38, (0.56, 0.47),
              offset=0.03, attack=0.04, hold=0.42, release=1.6,
              highpass=90, lowpass=900),
        layer("growl_536761", n, 0.30, (0.92, 0.78), vib=(7.0, 0.04),
              offset=0.06, attack=0.03, hold=0.38, release=1.9,
              highpass=120, lowpass=800),
        metal_whine(n, 0.12),
    ]
    out = mix(*tracks)
    ring(out, 57.0, 0.50)
    gate(out, 14.0, 0.28, start=0.30)
    saturate(out, 2.3)
    out = room(out, 0.20)
    return compress(out, -18.0, 3.5, 3.0, 110.0)


def cry_alien(n: int) -> list[float]:
    """异星：大象尖叫 + 大猩猩嘶吼 + 外星蛇音，加入游走颤音与失谐。"""
    tracks = [
        layer("elephant_scream_139052", n, 0.60, (1.02, 0.80), vib=(7.0, 0.055),
              attack=0.015, hold=0.44, release=1.8, highpass=550, lowpass=4800),
        layer("gorilla_scream_397054", n, 0.48, (1.16, 0.92), vib=(6.0, 0.06),
              offset=0.03, attack=0.018, hold=0.38, release=2.0,
              highpass=500, lowpass=5200, phase=1.1),
        layer("alien_snarl_631985", n, 0.58, (0.92, 0.76), vib=(5.0, 0.045),
              offset=0.01, attack=0.02, hold=0.46, release=1.7,
              highpass=450, lowpass=3800, phase=2.2),
        layer("hiss_559270", n, 0.38, (1.25, 1.05), vib=(18.0, 0.08),
              offset=0.06, attack=0.03, hold=0.28, release=2.2,
              highpass=1800, lowpass=7600),
    ]
    out = mix(*tracks)
    ring(out, 23.0, 0.18)
    tremolo(out, 8.5, 0.24, start=0.35)
    saturate(out, 1.7)
    out = room(out, 0.24)
    return compress(out, -18.0, 3.0, 4.0, 110.0)


CRIES = [
    ("heavy", "巨兽低沉型", cry_heavy, 17600),
    ("winged", "翼兽尖啸型", cry_winged, 16800),
    ("mech", "机械虫型", cry_mech, 16800),
    ("alien", "异星型", cry_alien, 16800),
]


# ---------------------------------------------------------------------------
# Emission


def emit_array(name: str, samples: list[int]) -> str:
    lines = [f"static const int16_t {name}[] = {{"]
    for i in range(0, len(samples), 12):
        chunk = ", ".join(str(v) for v in samples[i:i + 12])
        lines.append(f"    {chunk},")
    lines.append("};")
    return "\n".join(lines)


def write_wav(path: Path, samples: list[int]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    data = array.array("h", samples)
    if sys.byteorder == "big":
        data.byteswap()
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(data.tobytes())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preview-dir", type=Path, default=None,
                        help="also write 16 kHz WAV previews here")
    args = parser.parse_args()

    load_sources()
    rendered = []
    for key, title, fn, length in CRIES:
        rendered.append((key, title, fn(length), length))

    body = "\n\n".join(
        ["// Generated by tools/build_monster_cries.py. Do not edit by hand.",
         '#include "monster_cries.h"']
        + [emit_array(f"clip_monster_cry_{key}",
                      finish(samples, -15.5, 0.70))
           for key, _title, samples, _length in rendered]
        + ["const sfx_clip_t *monster_cry_clip(sfx_id_t id)\n{\n"
           "    switch (id) {\n"
           + "".join(
               f"    case SFX_CRY_{key.upper()}: {{\n"
               f"        static const sfx_clip_t clip = {{\n"
               f"            .samples = clip_monster_cry_{key},\n"
               f"            .count = {length},\n"
               f"        }};\n"
               f"        return &clip;\n"
               f"    }}\n"
               for key, _title, _samples, length in rendered)
           + "    default:\n"
             "        return 0;\n"
             "    }\n"
             "}"])
    OUT.write_text(body + "\n", encoding="utf-8")

    total = 0
    if args.preview_dir is not None:
        montage: list[int] = []
        for key, title, samples, length in rendered:
            preview = finish(samples, -14.5, 0.95)
            total += length
            path = args.preview_dir / f"monster-cry-{key}.wav"
            write_wav(path, preview)
            montage.extend(preview)
            montage.extend([0] * int(0.35 * RATE))
            peak = max(abs(v) for v in preview) / 32768.0
            rms = math.sqrt(sum(v * v for v in preview) / len(preview)) / 32768.0
            print(f"preview {key:6s} {title}: {length / RATE:.2f}s  "
                  f"peak {20 * math.log10(peak + 1e-9):.1f} dBFS  "
                  f"rms {20 * math.log10(rms + 1e-9):.1f} dBFS")
        write_wav(args.preview_dir / "monster-cries-all.wav", montage)

    print(f"wrote {OUT.relative_to(ROOT)} ({total or sum(c[3] for c in CRIES)} samples)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
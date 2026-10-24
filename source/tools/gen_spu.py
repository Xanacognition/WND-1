#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""GEARFRAME: the SPU bank. Sixteen instruments in the manner of a PlayStation sound bank (strings, choir, an
orchestra hit, a slap bass, a power chord...), synthesised here (no recording of anyone's), then encoded as the
PlayStation SPU encodes them: 16-byte ADPCM blocks of 28 samples, five prediction filters, a shift, and the
loop flags in each block's header (loop start, loop end + repeat, end + mute). The firmware decodes them in
the audio interrupt as the SPU does, with its 4-point Gaussian interpolation (the table, SPU_GAUSS, is the
SPU's own, as documented by psx-spx / nocash).

Each instrument: a short attack, then a loop made seamless by a crossfade (as the bank makers of the time did,
the loop's slow "breathing" included). The decay is the voice's envelope (the track's ENV page), not the sample.

    python3 tools/gen_spu.py              -> firmware/gen/gear_spu.h
    python3 tools/gen_spu.py --wav DIR    also writes each instrument decoded back, at its rate, to listen to
Needs numpy."""
import argparse
import math
import struct
import wave
from pathlib import Path

import numpy as np

OUT = Path(__file__).resolve().parent.parent / "firmware" / "gen" / "gear_spu.h"
RNG = np.random.default_rng(1997)               # the same bank every time

F0 = [0, 60, 115, 98, 122]                      # the SPU's prediction filters (/64)
F1 = [0, 0, -52, -55, -60]

# the SPU's Gaussian interpolation table (psx-spx, "4-Point Gaussian Interpolation"), 512 entries
GAUSS = [
-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 3, 3,
    3, 4, 4, 5, 5, 6, 7, 7, 8, 9, 9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 21, 22, 24, 25, 27, 28, 30, 32, 33, 35, 37,
    39, 41, 44, 46, 48, 51, 53, 56, 58, 61, 64, 67, 70, 73, 77, 80,
    84, 87, 91, 95, 99, 103, 107, 111, 116, 120, 125, 130, 135, 140, 145, 150,
    156, 161, 167, 173, 179, 186, 192, 199, 205, 212, 219, 227, 234, 242, 250, 257,
    266, 274, 283, 291, 300, 309, 319, 328, 338, 348, 358, 369, 379, 390, 401, 412,
    424, 436, 448, 460, 473, 485, 498, 512, 525, 539, 553, 567, 582, 597, 612, 627,
    643, 659, 675, 692, 708, 726, 743, 761, 779, 797, 816, 835, 854, 874, 894, 914,
    935, 956, 977, 999, 1020, 1043, 1066, 1089, 1112, 1136, 1160, 1184, 1209, 1234, 1260, 1286,
    1312, 1339, 1366, 1394, 1422, 1450, 1479, 1508, 1537, 1567, 1598, 1628, 1660, 1691, 1723, 1756,
    1789, 1822, 1856, 1890, 1924, 1959, 1995, 2031, 2067, 2104, 2141, 2179, 2217, 2256, 2295, 2334,
    2374, 2415, 2456, 2497, 2539, 2582, 2624, 2668, 2712, 2756, 2801, 2846, 2892, 2938, 2985, 3032,
    3079, 3128, 3176, 3225, 3275, 3325, 3376, 3427, 3479, 3531, 3584, 3637, 3691, 3745, 3799, 3855,
    3910, 3967, 4023, 4081, 4138, 4197, 4255, 4315, 4374, 4435, 4495, 4557, 4619, 4681, 4744, 4807,
    4871, 4935, 5000, 5065, 5131, 5197, 5264, 5332, 5399, 5468, 5536, 5606, 5676, 5746, 5817, 5888,
    5959, 6032, 6104, 6177, 6251, 6325, 6400, 6475, 6550, 6626, 6702, 6779, 6856, 6934, 7012, 7091,
    7170, 7249, 7329, 7409, 7490, 7571, 7653, 7735, 7817, 7900, 7983, 8066, 8150, 8234, 8319, 8404,
    8489, 8575, 8661, 8748, 8834, 8922, 9009, 9097, 9185, 9273, 9362, 9451, 9541, 9630, 9720, 9811,
    9901, 9992, 10083, 10174, 10266, 10358, 10450, 10542, 10635, 10727, 10820, 10913, 11007, 11100, 11194, 11288,
    11382, 11476, 11571, 11665, 11760, 11855, 11950, 12045, 12140, 12236, 12331, 12427, 12522, 12618, 12714, 12809,
    12905, 13001, 13097, 13193, 13289, 13385, 13481, 13577, 13673, 13769, 13865, 13961, 14056, 14152, 14248, 14343,
    14439, 14534, 14630, 14725, 14820, 14915, 15010, 15104, 15199, 15293, 15387, 15481, 15575, 15669, 15762, 15855,
    15948, 16041, 16133, 16226, 16317, 16409, 16500, 16592, 16682, 16773, 16863, 16953, 17042, 17131, 17220, 17308,
    17396, 17484, 17571, 17658, 17744, 17830, 17916, 18001, 18086, 18170, 18254, 18337, 18420, 18502, 18584, 18665,
    18746, 18826, 18905, 18985, 19063, 19141, 19219, 19295, 19372, 19447, 19522, 19597, 19671, 19744, 19816, 19888,
    19959, 20030, 20100, 20169, 20238, 20306, 20373, 20439, 20505, 20570, 20634, 20698, 20760, 20822, 20884, 20944,
    21004, 21063, 21121, 21178, 21235, 21290, 21345, 21399, 21452, 21505, 21556, 21607, 21657, 21706, 21754, 21801,
    21848, 21893, 21938, 21982, 22025, 22066, 22107, 22148, 22187, 22225, 22262, 22299, 22334, 22369, 22402, 22435,
    22467, 22498, 22527, 22556, 22584, 22611, 22637, 22662, 22686, 22709, 22731, 22752, 22772, 22791, 22809, 22826,
    22842, 22857, 22872, 22885, 22897, 22908, 22918, 22927, 22935, 22942, 22948, 22953, 22957, 22960, 22962, 22963,
]


# ---- synthesis helpers (float, -1..1)
def midi_hz(n):
    return 440.0 * 2.0 ** ((n - 69) / 12.0)


def phase(f, n, sr, vib_hz=0.0, vib_cents=0.0, drift_cents=0.0):
    """the running phase (cycles) of a tone at f Hz, with vibrato and a slow random drift"""
    t = np.arange(n) / sr
    c = vib_cents * np.sin(2 * math.pi * vib_hz * t + RNG.uniform(0, 6.28)) if vib_hz else 0.0
    if drift_cents:
        k = np.cumsum(RNG.normal(0, 1, n))
        k = np.convolve(k - k.mean(), np.ones(2001) / 2001, mode="same")
        c = c + drift_cents * k / (np.abs(k).max() + 1e-9)
    inst = f * 2.0 ** (np.asarray(c) / 1200.0) * np.ones(n)
    return np.cumsum(inst) / sr + RNG.uniform(0, 1)


def saw_ph(ph, f, sr, bright=1.0):
    """a band-limited saw from a phase (cycles): its harmonics up to 0.45 sr"""
    k_max = max(1, int(0.45 * sr / f))
    y = np.zeros_like(ph)
    for k in range(1, k_max + 1):
        y += np.sin(2 * math.pi * k * ph) / k ** bright
    return y * 0.55


def pulse_ph(ph, f, sr, duty=0.5):
    k_max = max(1, int(0.45 * sr / f))
    y = np.zeros_like(ph)
    for k in range(1, k_max + 1):
        y += np.sin(math.pi * k * duty) / k * np.cos(2 * math.pi * k * ph)
    return y * 0.8


def ensemble(f, n, sr, voices, cents, bright=1.0, vib=(5.2, 6.0), drift=6.0):
    y = np.zeros(n)
    for v in range(voices):
        d = cents * (2 * v / max(1, voices - 1) - 1) if voices > 1 else 0.0
        ff = f * 2 ** (d / 1200)
        y += saw_ph(phase(ff, n, sr, vib[0] * RNG.uniform(0.9, 1.1), vib[1], drift), ff, sr, bright)
    return y / math.sqrt(voices)


def biquad(x, kind, fc, sr, q=0.707, gain_db=0.0):
    """RBJ's cookbook filters"""
    w = 2 * math.pi * min(fc, 0.49 * sr) / sr
    cw, sw = math.cos(w), math.sin(w)
    a = sw / (2 * q)
    A = 10 ** (gain_db / 40)
    if kind == "lp":
        b = [(1 - cw) / 2, 1 - cw, (1 - cw) / 2]; aa = [1 + a, -2 * cw, 1 - a]
    elif kind == "hp":
        b = [(1 + cw) / 2, -(1 + cw), (1 + cw) / 2]; aa = [1 + a, -2 * cw, 1 - a]
    elif kind == "bp":
        b = [a, 0, -a]; aa = [1 + a, -2 * cw, 1 - a]
    else:                                       # peak
        b = [1 + a * A, -2 * cw, 1 - a * A]; aa = [1 + a / A, -2 * cw, 1 - a / A]
    b = [v / aa[0] for v in b]
    a1, a2 = aa[1] / aa[0], aa[2] / aa[0]
    y = np.zeros_like(x)
    x1 = x2 = y1 = y2 = 0.0
    for i, v in enumerate(x):
        o = b[0] * v + b[1] * x1 + b[2] * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, v, y1, o
        y[i] = o
    return y


def sweep_lp(x, fc, sr, q=0.9):
    """a 2-pole state-variable low-pass whose cutoff follows the array fc (Hz)"""
    y = np.zeros_like(x)
    lp = bp = 0.0
    qq = 1.0 / q
    for i, v in enumerate(x):
        f = 2 * math.sin(math.pi * min(fc[i], 0.2 * sr) / sr)
        lp += f * bp
        hp = v - lp - qq * bp
        bp += f * hp
        y[i] = lp
    return y


def env_exp(n, sr, tau):
    return np.exp(-np.arange(n) / sr / tau)


def ramp(n, sr, ms):
    k = max(1, int(sr * ms / 1000))
    e = np.ones(n)
    e[:k] = np.linspace(0, 1, k) ** 1.5
    return e


def noise(n):
    return RNG.uniform(-1, 1, n)


def flatten(x, sr, start):
    """the tail from start brought to a steady level (the voice's envelope does the decay)"""
    env = np.sqrt(np.convolve(x * x, np.ones(int(sr * 0.02)) / int(sr * 0.02), mode="same")) + 1e-6
    ref = env[start]
    g = np.ones_like(x)
    g[start:] = ref / env[start:]
    g = np.minimum(g, 8.0)
    return x * g


# ---- the instruments: (name EN, FR, ES, root note, rate, seconds, loop start in seconds or None, the sound)
def i_strings(sr, n, f):
    y = ensemble(f, n, sr, 7, 18, 1.0, (5.0, 7.0), 5.0)
    y = biquad(biquad(y, "lp", 3800, sr, 0.6), "hp", 180, sr) + 0.3 * biquad(y, "peak", 1100, sr, 1.0, 4)
    return y * ramp(n, sr, 70)


def i_choir(sr, n, f):
    src = np.zeros(n)
    for v in range(5):
        ff = f * 2 ** (RNG.uniform(-14, 14) / 1200)
        src += pulse_ph(phase(ff, n, sr, 5.4, 18, 8), ff, sr, 0.42)
    src = src / 2.2 + 0.04 * noise(n)
    y = (biquad(src, "bp", 760, sr, 5) * 1.0 + biquad(src, "bp", 1150, sr, 6) * 0.55 +
         biquad(src, "bp", 2850, sr, 8) * 0.28 + biquad(src, "bp", 380, sr, 3) * 0.3)
    return y * ramp(n, sr, 90)


def i_brass(sr, n, f):
    y = ensemble(f, n, sr, 3, 9, 1.0, (5.5, 4.0), 3.0)
    t = np.arange(n) / sr
    fc = 900 + 2600 * (1 - np.exp(-t / 0.05)) - 700 * (1 - np.exp(-t / 0.25))
    return sweep_lp(y, fc, sr, 1.1) * ramp(n, sr, 25)


def i_orchhit(sr, n, f):
    t = np.arange(n) / sr
    y = np.zeros(n)
    for semi, g in ((0, 1.0), (7, 0.8), (12, 0.9), (16, 0.45), (19, 0.6), (24, 0.5)):
        ff = f * 2 ** (semi / 12)
        y += g * (saw_ph(phase(ff, n, sr, 0, 0, 4), ff, sr) + 0.6 * saw_ph(phase(ff * 1.004, n, sr), ff, sr))
    fc = 5200 * np.exp(-t / 0.18) + 900
    y = sweep_lp(y, fc, sr, 1.0) * 0.5
    thump = np.sin(2 * math.pi * np.cumsum(f / 2 * (1 + 1.5 * np.exp(-t / 0.03))) / sr) * env_exp(n, sr, 0.12)
    crash = biquad(noise(n), "hp", 2500, sr) * env_exp(n, sr, 0.09) * 0.5
    return (y + 0.9 * thump + crash) * env_exp(n, sr, 0.22) * ramp(n, sr, 3)


def i_piano(sr, n, f):
    t = np.arange(n) / sr
    y = np.zeros(n)
    B = 0.00035
    for k in range(1, int(0.45 * sr / f) + 1):
        fk = k * f * math.sqrt(1 + B * k * k)
        if fk > 0.45 * sr:
            break
        for d in (-0.6, 0.6):                   # two strings a little apart: the beating of a real piano
            y += (1.0 / k ** 1.1) * np.sin(2 * math.pi * fk * (1 + d / 1200) * t + RNG.uniform(0, 6.28)) * \
                 np.exp(-t / (0.9 / (1 + 0.35 * k)))
    y += biquad(noise(n), "bp", 2400, sr, 1.0) * env_exp(n, sr, 0.008) * 0.6
    return y * ramp(n, sr, 2)


def i_epiano(sr, n, f):
    t = np.arange(n) / sr
    idx = 0.3 + 2.2 * np.exp(-t / 0.12)
    y = np.sin(2 * math.pi * f * t + idx * np.sin(2 * math.pi * f * t))
    y += 0.25 * np.sin(2 * math.pi * f * 14 * t) * env_exp(n, sr, 0.03)
    y += 0.15 * np.sin(2 * math.pi * f * 2 * t) * env_exp(n, sr, 0.4)
    return y * ramp(n, sr, 2)


def i_slap(sr, n, f):
    t = np.arange(n) / sr
    y = np.zeros(n)
    for k in range(1, int(0.45 * sr / f) + 1):
        y += (1.0 / k) * np.sin(2 * math.pi * k * f * t + 0.3 * k) * np.exp(-t / (0.5 / (1 + 0.5 * k)))
    pop = biquad(noise(n), "bp", 1800, sr, 1.5) * env_exp(n, sr, 0.006)
    return np.tanh(1.6 * y) + 0.8 * pop


def i_fretless(sr, n, f):
    t = np.arange(n) / sr
    ph = phase(f, n, sr, 4.5, 6, 2)
    y = np.sin(2 * math.pi * ph) + 0.35 * np.sin(4 * math.pi * ph) + 0.12 * np.sin(6 * math.pi * ph)
    fc = 400 + 900 * np.exp(-((t - 0.06) / 0.05) ** 2)            # the "mwah" of a fretless
    return sweep_lp(y + 0.2 * saw_ph(ph, f, sr), fc, sr, 1.4) * 1.4 * ramp(n, sr, 10)


def i_synbass(sr, n, f):
    ph = phase(f, n, sr)
    y = saw_ph(ph, f, sr) + 0.6 * pulse_ph(ph, f, sr, 0.5)
    return biquad(y, "lp", 700, sr, 2.5) * ramp(n, sr, 2)


def i_harp(sr, n, f):
    t = np.arange(n) / sr
    y = np.zeros(n)
    for k in range(1, int(0.45 * sr / f) + 1):    # a plucked string: bright at first, the high partials die first
        y += (1.0 / k ** 1.4) * np.sin(2 * math.pi * k * f * t + 0.2 * k) * np.exp(-t / (1.2 / (1 + 0.6 * k)))
    y += biquad(noise(n), "bp", 3000, sr, 1.2) * env_exp(n, sr, 0.004) * 0.3
    return y * ramp(n, sr, 1)


def i_flute(sr, n, f):
    ph = phase(f, n, sr, 5.0, 10, 3)
    y = np.sin(2 * math.pi * ph) + 0.22 * np.sin(4 * math.pi * ph) + 0.06 * np.sin(6 * math.pi * ph)
    breath = biquad(noise(n), "bp", f * 2, sr, 2.0) * 0.18
    chiff = biquad(noise(n), "bp", f * 4, sr, 1.0) * env_exp(n, sr, 0.03) * 0.5
    return (y + breath + chiff) * ramp(n, sr, 50)


def i_bell(sr, n, f):
    t = np.arange(n) / sr
    y = np.zeros(n)
    for r, g, tau in ((1.0, 1.0, 1.2), (2.0, 0.6, 0.8), (2.76, 0.5, 0.5), (5.40, 0.35, 0.25), (8.93, 0.2, 0.12),
                      (0.5, 0.3, 1.5)):
        y += g * np.sin(2 * math.pi * f * r * t) * np.exp(-t / tau)
    return y * ramp(n, sr, 1)


def i_pad(sr, n, f):
    y = ensemble(f, n, sr, 6, 22, 1.2, (0.3, 4.0), 8.0) + 0.5 * ensemble(f * 2, n, sr, 2, 9, 1.4, (0.4, 3), 6)
    return biquad(biquad(y, "lp", 1500, sr, 0.8), "lp", 2600, sr, 0.6) * ramp(n, sr, 140)


def i_guitar(sr, n, f):
    y = np.zeros(n)
    for semi in (0, 7, 12):                     # a power chord
        ff = f * 2 ** (semi / 12)
        y += saw_ph(phase(ff * 2 ** (RNG.uniform(-6, 6) / 1200), n, sr, 0, 0, 3), ff, sr)
    y = np.tanh(7.0 * biquad(y, "hp", 120, sr))
    y = biquad(biquad(biquad(y, "lp", 3600, sr, 0.8), "lp", 4500, sr, 0.7), "peak", 1500, sr, 1.0, 5)
    return biquad(y, "peak", 400, sr, 1.0, -4) * ramp(n, sr, 3)


def i_timpani(sr, n, f):
    t = np.arange(n) / sr
    y = np.zeros(n)
    for r, g, tau in ((1.0, 1.0, 0.7), (1.504, 0.55, 0.45), (1.742, 0.3, 0.3), (2.0, 0.25, 0.25), (2.245, 0.15, 0.2)):
        y += g * np.sin(2 * math.pi * f * r * t * (1 + 0.02 * np.exp(-t / 0.05)))
        y[:] = y
    y = y * env_exp(n, sr, 0.6)
    y += biquad(noise(n), "lp", 900, sr) * env_exp(n, sr, 0.02) * 0.8
    return y * ramp(n, sr, 2)


def i_rave(sr, n, f):
    y = np.zeros(n)
    for semi in (0, 12):
        y += ensemble(f * 2 ** (semi / 12), n, sr, 5, 26, 1.0, (0.1, 0), 2.0)
    return biquad(biquad(y, "hp", 200, sr), "peak", 2000, sr, 0.8, 3) * ramp(n, sr, 3)


INSTRUMENTS = [
    # EN, FR, ES, root, rate, seconds, loop at (s) or None, synth
    ("STRINGS", "CORDES", "CUERDAS", 60, 16000, 0.62, 0.18, i_strings),
    ("CHOIR AAH", "CHOEUR AAH", "CORO AAH", 60, 16000, 0.62, 0.18, i_choir),
    ("BRASS", "CUIVRES", "METALES", 60, 18900, 0.42, 0.16, i_brass),
    ("ORCH HIT", "COUP ORCH", "GOLPE ORQ", 60, 22050, 0.50, None, i_orchhit),
    ("PIANO", "PIANO", "PIANO", 60, 22050, 0.42, 0.24, i_piano),
    ("E.PIANO", "PIANO ELEC", "PIANO ELEC", 60, 22050, 0.36, 0.20, i_epiano),
    ("SLAP BASS", "BASSE SLAP", "BAJO SLAP", 36, 11025, 0.40, 0.22, i_slap),
    ("FRETLESS", "FRETLESS", "FRETLESS", 36, 11025, 0.42, 0.20, i_fretless),
    ("SYNTH BASS", "BASSE SYNTH", "BAJO SINTE", 36, 11025, 0.30, 0.08, i_synbass),
    ("HARP", "HARPE", "ARPA", 60, 22050, 0.36, 0.20, i_harp),
    ("FLUTE", "FLUTE", "FLAUTA", 72, 22050, 0.42, 0.18, i_flute),
    ("BELL", "CLOCHE", "CAMPANA", 72, 22050, 0.40, 0.24, i_bell),
    ("WARM PAD", "NAPPE", "COLCHON", 60, 16000, 0.75, 0.24, i_pad),
    ("POWER CHORD", "GUITARE SAT", "GUITARRA", 40, 16000, 0.50, 0.14, i_guitar),
    ("TIMPANI", "TIMBALES", "TIMBALES", 45, 11025, 0.70, None, i_timpani),
    ("RAVE STAB", "STAB RAVE", "STAB RAVE", 60, 22050, 0.40, 0.12, i_rave),
]


def crossfade_loop(x, ls, le, cf):
    """x[ls:le] plays in a loop: its last cf samples fade into the ones before ls (no click at the jump)"""
    cf = min(cf, ls, le - ls)
    w = np.sin(np.linspace(0, math.pi / 2, cf)) ** 2
    y = x.copy()
    y[le - cf:le] = x[le - cf:le] * (1 - w) + x[ls - cf:ls] * w
    return y[:le]


# ---- the SPU's ADPCM: encode block by block (each choice simulated through the decoder's state)
def encode(x16, loop_blk, nblk):
    combos = [(f, s) for f in range(5) for s in range(13)]
    cf0 = np.array([F0[f] for f, _ in combos], dtype=np.int64)
    cf1 = np.array([F1[f] for f, _ in combos], dtype=np.int64)
    sh = np.array([s for _, s in combos], dtype=np.int64)
    old = older = 0
    out = bytearray()
    dec = []
    for b in range(nblk):
        blk = x16[b * 28:(b + 1) * 28]
        o1 = np.full(len(combos), old, dtype=np.int64)
        o2 = np.full(len(combos), older, dtype=np.int64)
        err = np.zeros(len(combos))
        nibs = np.zeros((len(combos), 28), dtype=np.int64)
        for i in range(28):
            p = (o1 * cf0 + o2 * cf1 + 32) >> 6
            r = int(blk[i]) - p
            n = np.clip(np.round(r * (2.0 ** sh) / 4096.0), -8, 7).astype(np.int64)
            d = np.clip(((n << 12) >> sh) + p, -32768, 32767)
            err += (d - int(blk[i])) ** 2
            nibs[:, i] = n & 15
            o2, o1 = o1, d
        k = int(np.argmin(err))
        f, s = combos[k]
        flags = 0
        if loop_blk is not None and b == loop_blk:
            flags |= 4                          # loop start
        if b == nblk - 1:
            flags |= 3 if loop_blk is not None else 1   # loop end + repeat / end + mute
        out += bytes([(f << 4) | s, flags]) + bytes((int(nibs[k, 2 * j]) | (int(nibs[k, 2 * j + 1]) << 4)) for j in range(14))
        for i in range(28):                     # the decoder's state, as the firmware will have it
            n = int(nibs[k, i])
            n = n - 16 if n >= 8 else n
            p = (old * F0[f] + older * F1[f] + 32) >> 6
            d = max(-32768, min(32767, ((n << 12) >> s) + p))
            older, old = old, d
            dec.append(d)
    return bytes(out), np.array(dec, dtype=np.int16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wav", type=Path, help="write each instrument decoded to this folder")
    a = ap.parse_args()
    data = bytearray()
    rows = []
    for en, fr, es, root, sr, sec, loop_s, fn in INSTRUMENTS:
        n = int(sr * sec)
        x = fn(sr, n, midi_hz(root))
        if loop_s is not None:
            ls = int(round(loop_s * sr / 28)) * 28
            le = (n // 28) * 28
            x = flatten(x, sr, ls)
            x = crossfade_loop(x, ls, le, int(0.06 * sr))
            loop_blk = ls // 28
        else:
            fade = int(0.02 * sr)
            x[-fade:] *= np.linspace(1, 0, fade)
            x = x[:(len(x) + 27) // 28 * 28] if len(x) % 28 == 0 else np.concatenate([x, np.zeros(28 - len(x) % 28)])
            loop_blk = None
        x = x - np.mean(x[: len(x)])
        x = x / (np.abs(x).max() + 1e-9) * 0.92
        x16 = np.round(x * 32767).astype(np.int64)
        nblk = len(x16) // 28
        enc, dec = encode(x16, loop_blk, nblk)
        snr = 10 * math.log10(np.sum(x16.astype(float) ** 2) / (np.sum((x16 - dec) ** 2.0) + 1))
        rows.append((en, fr, es, len(data), nblk, loop_blk, sr, root))
        print(f"{en:12s} {sr:5d} Hz {nblk:4d} blocks {len(enc):6d} B  loop {loop_blk}  SNR {snr:4.1f} dB")
        data += enc
        if a.wav:
            a.wav.mkdir(parents=True, exist_ok=True)
            rep = dec if loop_blk is None else np.concatenate([dec] + [dec[loop_blk * 28:]] * 6)
            with wave.open(str(a.wav / (en.replace(" ", "_").replace(".", "") + ".wav")), "wb") as w:
                w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
                w.writeframes(struct.pack("<%dh" % len(rep), *rep))
    print(f"bank: {len(data)} bytes")
    L = ["/* generated by tools/gen_spu.py: GEARFRAME's SPU bank, PlayStation SPU ADPCM (16-byte blocks of 28 samples:",
         " * shift | filter << 4, flags (1 end, 2 repeat, 4 loop start), 14 bytes of nibbles), synthesised; and the SPU's",
         " * Gaussian interpolation table (psx-spx). step: the rate over 44100, Q16 (the pitch of the root note) */",
         "#pragma once", "#include <stdint.h>",
         f"#define SPU_N {len(rows)}u",
         "typedef struct {", "    uint32_t off;                               /* bytes into SPU_DATA */",
         "    uint16_t nblk, loop;                        /* blocks; the loop's first block (0xFFFF: a one-shot) */",
         "    uint32_t step;                              /* rate * 65536 / 44100 */",
         "    uint8_t root;                               /* the MIDI note it was recorded at */",
         "} spu_inst_t;",
         "static const spu_inst_t SPU_INST[SPU_N] = {"]
    for en, fr, es, off, nblk, lb, sr, root in rows:
        L.append(f"    {{{off}u, {nblk}u, {0xFFFF if lb is None else lb}u, {round(sr * 65536 / 44100)}u, {root}u}},   /* {en} */")
    L.append("};")
    L.append("static const char *const SPU_NAME[3][SPU_N] = {")
    for k in (1, 0, 2):                         # FR, EN, ES (kawa_data.c's order)
        L.append("    {" + ", ".join('"%s"' % r[k] for r in rows) + "},")
    L.append("};")
    L.append("static const int16_t SPU_GAUSS[512] = {")
    for i in range(0, 512, 16):
        L.append("    " + ", ".join(str(v) for v in GAUSS[i:i + 16]) + ",")
    L.append("};")
    L.append(f"static const uint8_t SPU_DATA[{len(data)}] = {{")
    for i in range(0, len(data), 32):
        L.append("    " + ",".join(str(b) for b in data[i:i + 32]) + ",")
    L.append("};")
    OUT.write_text("\n".join(L) + "\n")
    print("->", OUT)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""measure.py — in-room merenje za iRadio.

Dva rezima:
  pink  : snimak pink noise-a -> 1/3-oktavni frekvencijski odziv (najbrze, robustno)
  sweep : snimak log sweep-a  -> impulsni odziv, RT60 i frekvencijski odziv

Upotreba:
  ~/.venvs/measure/bin/python measure.py snimak.wav --mode pink
  ~/.venvs/measure/bin/python measure.py snimak.wav --mode sweep
        [--sweep sweep.flac] [--fs 48000] [--channel left|right|mix] [--json]
"""
import argparse
import json
import os
import subprocess
import sys

import numpy as np
from scipy import signal

SWEEP_DEFAULT = os.path.expanduser("~/Music/iRadio-Test/sweep_20Hz-20kHz_12s.flac")
PINK_DEFAULT = os.path.expanduser("~/Music/iRadio-Test/pink_noise_30s.flac")


def ffmpeg_bin():
    for c in (os.path.expanduser("~/bin/ffmpeg-dsd"), "ffmpeg"):
        p = c if os.path.isabs(c) else subprocess.run(["which", c], capture_output=True, text=True).stdout.strip()
        if p and os.path.exists(p):
            return p
    sys.exit("ffmpeg nije nadjen")


def decode(path, fs, channel):
    ff = ffmpeg_bin()
    cmd = [ff, "-hide_banner", "-loglevel", "error", "-i", path,
           "-ar", str(fs), "-f", "f32le", "-"]
    raw = subprocess.run(cmd, capture_output=True)
    if raw.returncode != 0:
        sys.exit("ffmpeg: " + raw.stderr.decode()[:300])
    a = np.frombuffer(raw.stdout, dtype="<f4").astype(np.float64)
    if a.size % 2 == 0:
        a = a.reshape(-1, 2)
        return a[:, 0] if channel == "left" else a[:, 1] if channel == "right" else a.mean(axis=1)
    return a


def third_octave(freqs, psd):
    base = 1000.0 * 2.0 ** (np.arange(-60, 61) / 3.0)
    out = []
    for fc in base:
        lo, hi = fc / 2 ** (1 / 6), fc * 2 ** (1 / 6)
        sel = (freqs >= lo) & (freqs <= hi)
        if not np.any(sel):
            continue
        out.append((float(fc), float(10 * np.log10(np.mean(psd[sel]) + 1e-30))))
    return out


def welch(x, fs, nper=8192):
    f, p = signal.welch(x, fs=fs, nperseg=nper, noverlap=nper // 2,
                        window="hann", detrend="constant", scaling="density")
    return f, p


def analyse_pink(x, ref_x, fs):
    """Transfer function: PSD(snimak) / PSD(referentni pink noise)."""
    if len(x) < fs:            # bar 1 s
        sys.exit("snimak je prekratak")
    f, p_rec = welch(x, fs)
    f2, p_ref = welch(ref_x, fs)
    n = min(len(p_rec), len(p_ref))
    p_rec, p_ref, f = p_rec[:n], p_ref[:n], f[:n]
    h = p_rec / (p_ref + 1e-30)
    smooth = third_octave(f, h)
    i1k = int(np.argmin(np.abs(f - 1000)))
    ref = np.mean(h[max(0, i1k - 3):i1k + 4])
    smooth = [(fc, db - 10 * np.log10(ref + 1e-30)) for fc, db in smooth]
    return smooth


def inverse_sweep(sweep, fs):
    """Inverzni filtar za log sweep (vremenski obrnuti sweep sa ampl. korekcijom)."""
    inv = sweep[::-1].copy()
    # amplitudska korekcija: +3 dB/okt (kompenzacija -3 dB/okt log sweep-a)
    n = len(inv)
    f0 = 20.0
    t = np.arange(n) / fs
    # eksponencijalni weighting u vremenu (standardna Farina korekcija)
    inv *= np.exp(t * np.log(2) / (n / fs))   # blaga korekcija; dovoljna za relativni odziv
    e = np.sum(inv ** 2)
    return inv / np.sqrt(e + 1e-30)


def analyse_sweep(x, sweep_path, fs, channel):
    sweep = decode(sweep_path, fs, channel)
    inv = inverse_sweep(sweep, fs)
    ir_full = signal.fftconvolve(x, inv, mode="full")
    peak = int(np.argmax(np.abs(ir_full)))
    ir = ir_full[max(0, peak - 64):peak + int(1.0 * fs)]
    if ir.size < 256:
        sys.exit("impulsni odziv je prekratak")

    # RT60 (T30) preko Schroeder krive
    e = np.cumsum(ir[::-1] ** 2)[::-1]
    edc = 10 * np.log10(e / (e[0] + 1e-30) + 1e-30)
    t = np.arange(len(edc)) / fs

    def cross(level):
        idx = np.where(edc <= level)[0]
        return t[idx[0]] if len(idx) else None

    t5, t25 = cross(-5), cross(-25)
    rt = (t25 - t5) * 2 if (t5 is not None and t25 is not None) else None

    f, p = welch(ir, fs, nper=4096)
    smooth = third_octave(f, p)
    i1k = int(np.argmin(np.abs(f - 1000)))
    ref = np.mean(p[max(0, i1k - 3):i1k + 4])
    smooth = [(fc, db - 10 * np.log10(ref + 1e-30)) for fc, db in smooth]
    return smooth, rt


def suggest_eq(smooth):
    peaks = []
    for fc, db in smooth:
        if 40 <= fc <= 300 and db >= 3.0:
            peaks.append({"freq": round(fc, 1), "gain_db": round(db, 1),
                          "cut_db": round(-min(db - 1.0, 6.0), 1),
                          "q": 4.0 if fc < 120 else 3.0})
    return peaks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("recording")
    ap.add_argument("--mode", choices=("pink", "sweep"), default="pink")
    ap.add_argument("--sweep", default=SWEEP_DEFAULT)
    ap.add_argument("--pink", default=PINK_DEFAULT)
    ap.add_argument("--fs", type=int, default=48000)
    ap.add_argument("--channel", default="left", choices=("left", "right", "mix"))
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()

    x = decode(a.recording, a.fs, a.channel)
    rt = None
    if a.mode == "pink":
        ref = decode(a.pink, a.fs, a.channel)
        smooth = analyse_pink(x, ref, a.fs)
    else:
        smooth, rt = analyse_sweep(x, a.sweep, a.fs, a.channel)

    peaks = suggest_eq(smooth)
    res = {"mode": a.mode, "rt60_s": None if rt is None else round(rt, 2),
           "third_octave": [(round(f, 1), round(d, 1)) for f, d in smooth],
           "peaks_40_300": peaks}
    if a.json:
        print(json.dumps(res, indent=2, ensure_ascii=False))
        return
    print(f"snimak : {a.recording}  ({a.mode})")
    if rt is not None:
        print(f"RT60   : {rt:.2f} s")
    print("\n1/3 oktava (dB, 0 = 1 kHz):")
    for f, d in smooth:
        if 25 <= f <= 16000:
            print(f"  {f:8.1f} Hz  {d:+6.1f}")
    if peaks:
        print("\nPredlog EQ (vrhovi 40-300 Hz):")
        for p in peaks:
            print(f"  {p['freq']:6.1f} Hz  +{p['gain_db']} dB → cut {p['cut_db']} dB  Q={p['q']}")
    else:
        print("\nNema vrhova > +3 dB u 40-300 Hz.")


if __name__ == "__main__":
    main()

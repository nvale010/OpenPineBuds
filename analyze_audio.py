#!/usr/bin/env python3
"""
analyze_audio.py — FFT analysis of raw PCM captures from OpenPineBuds

Usage:
    python3 analyze_audio.py <file.raw> [--rate 8000] [--skip 64]

Produces:
    - Waveform plot
    - FFT magnitude spectrum
    - Top 10 frequency peaks printed to stdout
"""

import sys
import argparse
import numpy as np
import matplotlib.pyplot as plt
from scipy import signal


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("file", help="Raw PCM file (.raw)")
    parser.add_argument(
        "--rate", type=int, default=8000, help="Sample rate in Hz (default: 8000)"
    )
    parser.add_argument(
        "--skip",
        type=int,
        default=64,
        help="Bytes to skip at start (banner, default: 64)",
    )
    parser.add_argument(
        "--no-plot", action="store_true", help="Print results only, no GUI"
    )
    args = parser.parse_args()

    # Load raw signed 16-bit little-endian mono PCM
    raw = np.fromfile(args.file, dtype="<i2")
    if args.skip > 0:
        skip_samples = args.skip // 2
        raw = raw[skip_samples:]

    if len(raw) == 0:
        print("ERROR: no samples after skipping header bytes")
        sys.exit(1)

    duration_ms = len(raw) * 1000 / args.rate
    print(f"File        : {args.file}")
    print(f"Samples     : {len(raw)}")
    print(f"Duration    : {duration_ms:.1f} ms")
    print(f"Sample rate : {args.rate} Hz")
    print(f"Peak value  : {np.max(np.abs(raw))}")
    print(f"RMS level   : {np.sqrt(np.mean(raw.astype(np.float64) ** 2)):.1f}")
    print()

    # FFT
    freqs, psd = signal.welch(
        raw.astype(np.float64),
        fs=args.rate,
        nperseg=min(1024, len(raw)),
        scaling="spectrum",
    )
    psd_db = 10 * np.log10(psd + 1e-10)

    # Find peaks
    peaks, props = signal.find_peaks(psd_db, height=np.max(psd_db) - 40, distance=5)
    peak_freqs = freqs[peaks]
    peak_amps = psd_db[peaks]
    order = np.argsort(peak_amps)[::-1]

    print("Top frequency peaks:")
    print(f"  {'Frequency (Hz)':>16}  {'Power (dB)':>10}")
    print(f"  {'-' * 16}  {'-' * 10}")
    for i in order[:10]:
        print(f"  {peak_freqs[i]:>16.1f}  {peak_amps[i]:>10.1f}")

    if args.no_plot:
        return

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 8))
    fig.suptitle(f"Audio Analysis: {args.file.split('/')[-1]}")

    # Waveform
    t = np.arange(len(raw)) / args.rate * 1000  # ms
    ax1.plot(t, raw, linewidth=0.5, color="steelblue")
    ax1.set_xlabel("Time (ms)")
    ax1.set_ylabel("Amplitude (16-bit)")
    ax1.set_title("Waveform")
    ax1.grid(True, alpha=0.3)

    # Spectrum
    ax2.plot(freqs, psd_db, color="darkorange", linewidth=0.8)
    ax2.scatter(
        peak_freqs[order[:10]],
        peak_amps[order[:10]],
        color="red",
        zorder=5,
        s=30,
        label="Peaks",
    )
    for i in order[:5]:
        ax2.annotate(
            f"{peak_freqs[i]:.0f} Hz",
            (peak_freqs[i], peak_amps[i]),
            textcoords="offset points",
            xytext=(5, 5),
            fontsize=8,
            color="red",
        )
    ax2.set_xlabel("Frequency (Hz)")
    ax2.set_ylabel("Power (dB)")
    ax2.set_title("Power Spectrum (Welch)")
    ax2.legend()
    ax2.grid(True, alpha=0.3)

    plt.tight_layout()
    plt.savefig(args.file.replace(".raw", "_fft.png"), dpi=150)
    print(f"\nPlot saved to {args.file.replace('.raw', '_fft.png')}")
    plt.show()


if __name__ == "__main__":
    main()

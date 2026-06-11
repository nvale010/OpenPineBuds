#!/usr/bin/env python3
"""
respiration.py — Respiration rate detection from OpenPineBuds ear canal mic

The feedback mic in a sealed ear canal acts as a pressure sensor.
Chest expansion during inhalation slightly changes the ear canal geometry
(via jaw/throat coupling and Eustachian tube pressure), producing a slow
pressure oscillation at the breath rate (~0.2–0.5 Hz, i.e. 12–30 breaths/min).

Usage:
    python3 respiration.py <file.raw> [--rate 8000] [--skip 120]

The script:
    1. Loads the raw PCM
    2. Applies a low-pass filter (<2 Hz) to isolate the respiration band
    3. Detects breath cycles and estimates rate
    4. Plots the filtered signal with detected peaks
"""

import sys
import argparse
import numpy as np
import matplotlib.pyplot as plt
from scipy import signal

SAMPLE_RATE = 8000


def bandpass(data, fs, low, high, order=2):
    """Use second-order sections for numerical stability at very low frequencies."""
    nyq = fs / 2
    sos = signal.butter(order, [low / nyq, high / nyq], btype="band", output="sos")
    return signal.sosfiltfilt(sos, data)


def lowpass(data, fs, cutoff, order=2):
    nyq = fs / 2
    sos = signal.butter(order, cutoff / nyq, btype="low", output="sos")
    return signal.sosfiltfilt(sos, data)


def highpass(data, fs, cutoff, order=2):
    nyq = fs / 2
    sos = signal.butter(order, cutoff / nyq, btype="high", output="sos")
    return signal.sosfiltfilt(sos, data)


def lowpass(data, fs, cutoff, order=4):
    nyq = fs / 2
    b, a = signal.butter(order, cutoff / nyq, btype="low")
    return signal.filtfilt(b, a, data)


def highpass(data, fs, cutoff, order=4):
    nyq = fs / 2
    b, a = signal.butter(order, cutoff / nyq, btype="high")
    return signal.filtfilt(b, a, data)


def detect_breaths(filtered, fs, min_breath_s=1.5, max_breath_s=8.0):
    """Detect breath peaks in the low-pass filtered signal."""
    min_dist = int(min_breath_s * fs)
    # Normalise
    sig = filtered - np.mean(filtered)
    if np.std(sig) == 0:
        return np.array([]), np.array([])
    sig = sig / np.std(sig)

    # Find positive peaks (inhalation) and negative peaks (exhalation)
    peaks_pos, _ = signal.find_peaks(sig, distance=min_dist, prominence=0.1)
    peaks_neg, _ = signal.find_peaks(-sig, distance=min_dist, prominence=0.1)
    return peaks_pos, peaks_neg


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("file", help="Raw PCM file (.raw)")
    parser.add_argument(
        "--rate", type=int, default=8000, help="Sample rate Hz (default: 8000)"
    )
    parser.add_argument(
        "--skip", type=int, default=120, help="Bytes to skip at start (default: 120)"
    )
    parser.add_argument("--no-plot", action="store_true")
    args = parser.parse_args()

    fs = args.rate

    # -----------------------------------------------------------------------
    # Load
    # -----------------------------------------------------------------------
    raw = np.fromfile(args.file, dtype="<i2").astype(np.float64)
    skip_samples = args.skip // 2
    raw = raw[skip_samples:]

    duration = len(raw) / fs
    print(f"File     : {args.file}")
    print(f"Duration : {duration:.1f} s")
    print(f"Samples  : {len(raw)}")

    if duration < 10:
        print("\nWARNING: Recording is shorter than 10 seconds.")
        print("For reliable respiration detection, record 30-60 seconds")
        print("of normal breathing with the earbud seated in your ear.\n")

    # -----------------------------------------------------------------------
    # Auto-detect and skip the ADC settling transient at the start, then
    # skip an additional filter warm-up window so the bandpass filter
    # never sees the step discontinuity at the cut point.
    #
    # Strategy:
    #   1. Find where signal RMS drops to stable level (transient end)
    #   2. Skip an additional WARMUP seconds after that point
    #   3. Feed the filter WARMUP seconds of stable data before the analysis
    #      window so it is fully initialised (overlap-and-discard)
    # -----------------------------------------------------------------------
    WARMUP_S = 8  # seconds of stable data to feed filter before analysis

    fs_window = fs // 4  # 250ms
    stable_rms = (
        np.sqrt(np.mean(raw[-fs * 20 :].astype(np.float64) ** 2))
        if len(raw) > fs * 20
        else np.sqrt(np.mean(raw.astype(np.float64) ** 2))
    )
    threshold = max(stable_rms * 3, 1000)
    transient_end = 0
    for i in range(0, min(len(raw) - fs_window, fs * 10), fs_window // 2):
        rms = np.sqrt(np.mean(raw[i : i + fs_window].astype(np.float64) ** 2))
        if rms <= threshold:
            transient_end = i
            break

    # Start the filter WARMUP_S before the analysis window
    filter_start = max(transient_end, 0)
    analysis_start = filter_start + WARMUP_S * fs

    if analysis_start >= len(raw):
        print(
            f"WARNING: recording too short after transient skip ({len(raw) / fs:.1f}s)"
        )
        analysis_start = filter_start

    print(
        f"Transient ends at {transient_end / fs:.2f}s, "
        f"filter warm-up until {analysis_start / fs:.2f}s"
    )

    # Also trim the last 2 seconds — the 's' stop command causes a transient
    # at the end that creates a mirror-image spike from filtfilt's edge padding.
    TAIL_TRIM_S = 2
    tail_trim = min(TAIL_TRIM_S * fs, len(raw) // 4)
    raw_for_filter = raw[filter_start:-tail_trim].astype(np.float64)
    raw_for_filter = raw_for_filter - np.mean(raw_for_filter)

    # -----------------------------------------------------------------------
    # Respiration band: 0.1 – 2.0 Hz
    # Filter the full stable signal including warm-up, then discard warm-up.
    # -----------------------------------------------------------------------
    resp_full = bandpass(raw_for_filter, fs, low=0.1, high=2.0)
    resp_full = lowpass(resp_full, fs, cutoff=1.5)

    # Discard the warm-up window — only analyse from analysis_start
    warmup_samples = analysis_start - filter_start
    resp_smooth = resp_full[warmup_samples:]
    raw = raw_for_filter[warmup_samples:]  # align raw for plotting

    # -----------------------------------------------------------------------
    # Detect breath cycles
    # -----------------------------------------------------------------------
    peaks_pos, peaks_neg = detect_breaths(resp_smooth, fs)

    all_peaks = np.sort(np.concatenate([peaks_pos, peaks_neg]))

    # Estimate breath rate from peak spacing
    if len(all_peaks) >= 4:
        # Each breath = one positive + one negative peak = 2 events
        inter_event = np.diff(all_peaks) / fs  # seconds between events
        breath_period = np.median(inter_event) * 2  # full cycle = 2 events
        breath_rate = 60.0 / breath_period
        print(f"\nRespiration estimate:")
        print(f"  Detected events  : {len(all_peaks)} peaks")
        print(f"  Median period    : {breath_period:.1f} s/breath")
        print(f"  Estimated rate   : {breath_rate:.1f} breaths/min")
        if 6 <= breath_rate <= 40:
            print(f"  Assessment       : plausible respiratory signal")
        else:
            print(
                f"  Assessment       : outside normal range — may be noise or motion artefact"
            )
    else:
        print(
            f"\nNot enough peaks detected ({len(all_peaks)}) — recording may be too short,"
        )
        print("or the earbud seal may be insufficient. Try a longer recording.")

    # -----------------------------------------------------------------------
    # Spectrogram of the respiration band
    # -----------------------------------------------------------------------
    f_spec, t_spec, Sxx = signal.spectrogram(
        resp_smooth,
        fs=fs,
        nperseg=min(fs * 10, len(resp_smooth) // 4),
        noverlap=None,
        scaling="spectrum",
    )
    # Only show the respiration band
    resp_mask = f_spec <= 2.5
    f_spec = f_spec[resp_mask]
    Sxx = Sxx[resp_mask, :]

    if args.no_plot:
        return

    # -----------------------------------------------------------------------
    # Plot
    # -----------------------------------------------------------------------
    t = np.arange(len(raw)) / fs

    fig, axes = plt.subplots(3, 1, figsize=(14, 10))
    fig.suptitle(f"Respiration Analysis: {args.file.split('/')[-1]}", fontsize=12)

    # 1. Raw waveform
    ax = axes[0]
    ax.plot(t, raw, linewidth=0.3, color="steelblue", alpha=0.7)
    ax.set_title("Raw mic signal (DC removed)")
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Amplitude")
    ax.grid(True, alpha=0.3)

    # 2. Filtered respiration signal + detected peaks
    ax = axes[1]
    t_resp = np.arange(len(resp_smooth)) / fs  # same length as raw after skip
    norm = resp_smooth / (np.std(resp_smooth) + 1e-10)
    ax.plot(
        t_resp, norm, linewidth=1.2, color="darkorange", label="Resp. signal (norm.)"
    )
    if len(peaks_pos) > 0:
        ax.scatter(
            peaks_pos / fs,
            norm[peaks_pos],
            color="red",
            zorder=5,
            s=50,
            label=f"Inhale peaks ({len(peaks_pos)})",
        )
    if len(peaks_neg) > 0:
        ax.scatter(
            peaks_neg / fs,
            norm[peaks_neg],
            color="blue",
            zorder=5,
            s=50,
            marker="v",
            label=f"Exhale troughs ({len(peaks_neg)})",
        )
    ax.axhline(0, color="gray", linewidth=0.5)
    ax.set_title("Bandpass filtered (0.1–2.0 Hz) — respiration band")
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Normalised amplitude")
    ax.legend(loc="upper right", fontsize=8)
    ax.grid(True, alpha=0.3)

    # 3. Spectrogram of respiration band
    ax = axes[2]
    if Sxx.shape[1] > 1:
        ax.pcolormesh(
            t_spec,
            f_spec * 60,
            10 * np.log10(Sxx + 1e-20),
            shading="gouraud",
            cmap="inferno",
        )
        ax.set_title("Spectrogram (respiration band, 0–2.5 Hz)")
        ax.set_xlabel("Time (s)")
        ax.set_ylabel("Frequency (breaths/min)")
        ax.axhline(
            12, color="cyan", linewidth=0.8, linestyle="--", alpha=0.5, label="12 bpm"
        )
        ax.axhline(
            20, color="lime", linewidth=0.8, linestyle="--", alpha=0.5, label="20 bpm"
        )
        ax.legend(fontsize=8)

    plt.tight_layout()
    outfile = args.file.replace(".raw", "_resp.png")
    plt.savefig(outfile, dpi=150)
    print(f"\nPlot saved to {outfile}")
    plt.show()


if __name__ == "__main__":
    main()

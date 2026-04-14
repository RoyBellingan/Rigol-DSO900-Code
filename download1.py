#!/usr/bin/env python3
"""
Download deep-memory waveforms from a Rigol DHO800/DHO900 oscilloscope.

Reads every requested channel's full RAW acquisition buffer over SCPI,
writes per-channel CSVs, a time-aligned multi-channel CSV, a decimated
CSV, and per-channel verification plots (aligned vs decimated).

IMPORTANT — DHO800/900 WAV subsystem quirk
-------------------------------------------
The scope's WAV read-back engine is **stateful**: after reading one
channel in RAW mode the internal pointers / limits are *not* reset.
Attempting to read the next channel without a full reinitialisation
yields a silently truncated record (often 1/4 or 1/10 of the real
depth).  The workaround is ``_reset_wav_subsystem()``, which cycles
the mode NORMal → RAW, resets STAR/STOP, re-selects the source, and
pauses briefly before proceeding.  See the function's docstring for
details.
"""

import csv
import time
from datetime import datetime
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import pyvisa  # noqa: E402

# ── User-configurable constants ──────────────────────────────────────
IP = "192.168.1.162"
CHANNELS = ["CHAN1", "CHAN2", "CHAN3", "CHAN4"]
OUT_PREFIX = ""
OUT_DIR_PREFIX = "aq_"
CHUNK_POINTS = 250_000   # samples per :WAV:DATA? request (BYTE mode → 1 byte/sample)
OUTPUT_POINTS = 10_000   # target row count for the decimated CSV
RESET_PAUSE = 0.5        # seconds to let the scope settle between channel reads


# ── SCPI helpers ─────────────────────────────────────────────────────

def _open_scope(ip: str):
    rm = pyvisa.ResourceManager("@py")
    scope = rm.open_resource(f"TCPIP::{ip}::INSTR")
    scope.timeout = 180_000
    scope.chunk_size = 1024 * 1024
    scope.read_termination = "\n"
    scope.write_termination = "\n"
    return rm, scope


def _check_scpi_errors(scope, context: str = "", quiet: bool = False):
    """Drain all queued SCPI errors.  Print them unless *quiet*."""
    while True:
        err = scope.query(":SYST:ERR?").strip()
        if err.startswith("0,") or err.startswith("0 ") or err == "0":
            break
        if not quiet:
            tag = f" [{context}]" if context else ""
            print(f"SCPI ERROR{tag}: {err}")


def _acquire_memory_depth(scope) -> int:
    """Return the current acquisition memory depth in points (0 if AUTO)."""
    raw = scope.query(":ACQ:MDEP?").strip().upper()
    if raw not in ("AUTO", "") :
        try:
            return int(float(raw))
        except ValueError:
            pass
    raw = scope.query(":ACQuire:MDEPth?").strip().upper()
    if raw in ("AUTO", ""):
        return 0
    return int(float(raw))


# ── WAV subsystem reset (DHO800/900 workaround) ─────────────────────

def _reset_wav_subsystem(scope, channel: str):
    """
    Reinitialise the WAV read-back engine before reading *channel*.

    **Why this is needed (DHO800/900 firmware quirk):**
    After a RAW bulk read of channel N, the scope's internal WAV state
    (pointers, POIN limit, buffer offsets) is left dirty.  If you simply
    switch :WAV:SOUR to channel N+1 and read again, the scope silently
    returns a *truncated* record — typically 1/4 or even 1/10 of the
    real per-channel depth — with no SCPI error.

    The sequence below was found empirically to clear that state
    reliably on DHO924S firmware 00.01.02:

      1. Switch :WAV:MODE to NORMal (flushes RAW engine).
      2. Reset :WAV:STAR / :WAV:STOP to small defaults.
      3. Select the new channel source.
      4. Switch back to :WAV:MODE RAW + :WAV:FORM BYTE.
      5. Sleep briefly (RESET_PAUSE) to let firmware settle.
    """
    scope.write(":WAV:MODE NORMal")
    _check_scpi_errors(scope, "WAV:MODE NORMal (reset)", quiet=True)
    scope.write(":WAV:STAR 1")
    scope.write(":WAV:STOP 1000")
    _check_scpi_errors(scope, "WAV:STAR/STOP reset", quiet=True)

    scope.write(f":WAV:SOUR {channel}")
    _check_scpi_errors(scope, f"WAV:SOUR {channel}")

    scope.write(":WAV:MODE RAW")
    _check_scpi_errors(scope, "WAV:MODE RAW")
    scope.write(":WAV:FORM BYTE")
    _check_scpi_errors(scope, "WAV:FORM BYTE")

    time.sleep(RESET_PAUSE)
    _check_scpi_errors(scope, f"post-reset {channel}", quiet=True)


# ── Waveform download ───────────────────────────────────────────────

def _read_channel_raw(scope, channel: str, memory_depth: int,
                      chunk: int = CHUNK_POINTS) -> dict:
    """
    Read the full RAW record for *channel*.

    Returns a dict with keys: channel, points, xinc, xorig, xref, values.
    """
    _reset_wav_subsystem(scope, channel)

    scope.write(f":WAV:POIN {memory_depth}")
    _check_scpi_errors(scope, f"WAV:POIN {memory_depth} {channel}", quiet=True)
    accepted = scope.query(":WAV:POIN?").strip()
    print(f"{channel}: :WAV:POIN {memory_depth} -> accepted {accepted}")

    preamble = scope.query(":WAV:PRE?").strip()
    _check_scpi_errors(scope, f"WAV:PRE? {channel}")
    parts = [p.strip() for p in preamble.split(",")]
    if len(parts) < 10:
        raise RuntimeError(f"Unexpected preamble for {channel}: {preamble}")

    points = int(float(parts[2]))
    print(f"{channel}: preamble reports {points} RAW points "
          f"(memory depth setting: {memory_depth})")

    xinc  = float(parts[4])
    xorig = float(parts[5])
    xref  = float(parts[6])
    yinc  = float(parts[7])
    yorig = float(parts[8])
    yref  = float(parts[9])

    values: list[float] = []
    start = 1
    while start <= points:
        stop = min(start + chunk - 1, points)
        scope.write(f":WAV:STAR {start}")
        scope.write(f":WAV:STOP {stop}")
        _check_scpi_errors(scope, f"WAV:STAR/STOP {start}..{stop}")
        raw = scope.query_binary_values(
            ":WAV:DATA?", datatype="B",
            header_fmt="ieee", expect_termination=True,
        )
        _check_scpi_errors(scope, f"WAV:DATA? {channel} {start}..{stop}")
        expected = stop - start + 1
        if len(raw) != expected:
            raise RuntimeError(
                f"{channel}: expected {expected} samples "
                f"for {start}..{stop}, got {len(raw)}"
            )
        for b in raw:
            values.append((b - yref - yorig) * yinc)
        print(f"  {channel}: read {start}..{stop} / {points}")
        start = stop + 1

    return {
        "channel": channel,
        "points": len(values),
        "xinc": xinc, "xorig": xorig, "xref": xref,
        "values": values,
    }


# ── Time helpers ─────────────────────────────────────────────────────

def _ref_time(ref_wf: dict, idx) -> float:
    """Absolute timestamp from the reference (longest) channel's preamble."""
    return ref_wf["xorig"] + (idx - ref_wf["xref"]) * ref_wf["xinc"]


def _evenly_spaced_indices(n: int, k: int) -> list[int]:
    """Return *k* indices in [0, n-1] spanning the full record."""
    if n <= 0 or k <= 0:
        return []
    if n <= k:
        return list(range(n))
    if k == 1:
        return [n // 2]
    return [round(j * (n - 1) / (k - 1)) for j in range(k)]


# ── CSV writers ──────────────────────────────────────────────────────

def _save_single_channel_csv(wf, ref_wf, prefix: str, out_dir: Path):
    """One CSV per channel with timestamps from the reference channel."""
    n = wf["points"]
    ref_n = ref_wf["points"]
    ratio = (ref_n - 1) / (n - 1) if n > 1 else 1

    path = out_dir / f"{prefix}_{wf['channel']}.csv"
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["index", "time_s", "voltage_V"])
        for i, v in enumerate(wf["values"]):
            w.writerow([i, _ref_time(ref_wf, i * ratio), v])
    print(f"Saved {path}  ({n} rows)")


def _build_aligned_rows(waveforms, ref_wf):
    """
    Align all channels to the *shortest* channel's sample count.

    Longer channels are sub-sampled; shorter ones keep every point.
    Returns (aligned_n, [(ref_index, [voltage_per_channel]), ...]).
    """
    aligned_n = min(wf["points"] for wf in waveforms)
    ref_n = ref_wf["points"]
    rows = []
    for i in range(aligned_n):
        ref_idx = round(i * (ref_n - 1) / (aligned_n - 1)) if aligned_n > 1 else 0
        voltages = []
        for wf in waveforms:
            wf_n = wf["points"]
            if wf_n == aligned_n:
                voltages.append(wf["values"][i])
            else:
                j = round(i * (wf_n - 1) / (aligned_n - 1)) if aligned_n > 1 else 0
                voltages.append(wf["values"][j])
        rows.append((ref_idx, voltages))
    return aligned_n, rows


def _save_aligned_csv(waveforms, ref_wf, prefix: str, out_dir: Path):
    """Multi-channel CSV at the shortest channel's sample count."""
    aligned_n, rows = _build_aligned_rows(waveforms, ref_wf)
    header = ["rowid", "time_s"] + [wf["channel"] for wf in waveforms]
    path = out_dir / f"{prefix}_aligned.csv"
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        for i, (ref_idx, voltages) in enumerate(rows):
            w.writerow([i, _ref_time(ref_wf, ref_idx)] + voltages)
    print(f"Saved {path}  ({aligned_n} rows)")


def _save_decimated_csv(waveforms, ref_wf, prefix: str, out_dir: Path):
    """Evenly decimated to OUTPUT_POINTS from the aligned data."""
    aligned_n, all_rows = _build_aligned_rows(waveforms, ref_wf)
    idxs = _evenly_spaced_indices(aligned_n, min(OUTPUT_POINTS, aligned_n))
    header = ["rowid", "time_s"] + [wf["channel"] for wf in waveforms]
    path = out_dir / f"{prefix}_decimated.csv"
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        for new_i, src_i in enumerate(idxs):
            ref_idx, voltages = all_rows[src_i]
            w.writerow([new_i, _ref_time(ref_wf, ref_idx)] + voltages)
    print(f"Saved {path}  ({len(idxs)} rows)")


# ── Plotting ─────────────────────────────────────────────────────────

def _read_multichannel_csv(path: Path):
    """Parse an aligned / decimated CSV into (channel_names, times, {ch: [v]})."""
    with open(path, newline="") as f:
        reader = csv.reader(f)
        header = next(reader)
        if len(header) < 3 or header[0] != "rowid" or header[1] != "time_s":
            raise ValueError(f"Unexpected header in {path}: {header}")
        ch_names = header[2:]
        times: list[float] = []
        cols: dict[str, list[float]] = {c: [] for c in ch_names}
        for row in reader:
            if len(row) < 2 + len(ch_names):
                continue
            times.append(float(row[1]))
            for i, c in enumerate(ch_names):
                cols[c].append(float(row[2 + i]))
    return ch_names, times, cols


def _plot_aligned_vs_decimated(out_dir: Path, prefix: str,
                               channels: list[str]) -> None:
    """Per-channel PNG: aligned trace with decimated dots overlaid."""
    aligned_path  = out_dir / f"{prefix}_aligned.csv"
    decimated_path = out_dir / f"{prefix}_decimated.csv"
    _, t_a, v_a = _read_multichannel_csv(aligned_path)
    _, t_d, v_d = _read_multichannel_csv(decimated_path)

    for ch in channels:
        if ch not in v_a or ch not in v_d:
            print(f"plot: skip {ch} (column missing in CSV)")
            continue
        fig, ax = plt.subplots(figsize=(11, 4))
        ax.plot(t_a, v_a[ch], color="C0", lw=0.6, alpha=0.85,
                label="aligned")
        ax.plot(t_d, v_d[ch], color="C1", ls="none", marker=".",
                markersize=3, label="decimated")
        ax.set_xlabel("time (s)")
        ax.set_ylabel("voltage (V)")
        ax.set_title(f"{ch}: aligned vs decimated")
        ax.legend(loc="best", fontsize=9)
        ax.grid(True, alpha=0.35)
        fig.tight_layout()
        out_png = out_dir / f"{prefix}_{ch}_check.png"
        fig.savefig(out_png, dpi=150)
        plt.close(fig)
        print(f"Saved {out_png}")


# ── Screenshot ────────────────────────────────────────────────────────

def _save_screenshot(scope, out_dir: Path, prefix: str):
    """Download a PNG screenshot of the oscilloscope display via SCPI."""
    print("Capturing screenshot...")
    png = scope.query_binary_values(
        ":DISP:DATA? PNG", datatype="B", container=bytes,
    )
    _check_scpi_errors(scope, "DISP:DATA? PNG")
    path = out_dir / f"{prefix}screenshot.png"
    with open(path, "wb") as f:
        f.write(png)
    print(f"Saved {path}  ({len(png)} bytes)")


# ── Validation ───────────────────────────────────────────────────────

def _validate_channels(channels):
    valid = {"CHAN1", "CHAN2", "CHAN3", "CHAN4"}
    bad = [ch for ch in channels if ch not in valid]
    if bad:
        raise ValueError(f"Invalid channels: {bad}. Valid: {sorted(valid)}")


# ── Main ─────────────────────────────────────────────────────────────

def main():
    _validate_channels(CHANNELS)

    rm, scope = _open_scope(IP)
    try:
        out_dir = Path(
            f"{OUT_DIR_PREFIX}{datetime.now().strftime('%Y-%m-%d_%H%M%S')}"
        )
        out_dir.mkdir(parents=True, exist_ok=True)
        print(f"Output directory: {out_dir.resolve()}")
        print(scope.query("*IDN?").strip())

        _check_scpi_errors(scope, "startup", quiet=True)

        scope.write(":STOP")
        _check_scpi_errors(scope, "STOP")

        memory_depth = _acquire_memory_depth(scope)
        _check_scpi_errors(scope, "ACQ:MDEP")
        print(f"Memory depth (points): {memory_depth}")

        waveforms = []
        for ch in CHANNELS:
            wf = _read_channel_raw(scope, ch, memory_depth, CHUNK_POINTS)
            waveforms.append(wf)

        ref_wf = max(waveforms, key=lambda w: w["points"])
        for wf in waveforms:
            t0 = _ref_time(ref_wf, 0)
            t1 = _ref_time(ref_wf, ref_wf["points"] - 1)
            ratio = ref_wf["points"] / wf["points"]
            print(f"  {wf['channel']}: {wf['points']:,} pts "
                  f"(ratio {ratio:.0f}x), "
                  f"time [{t0:.6e} .. {t1:.6e}]")

        for wf in waveforms:
            _save_single_channel_csv(wf, ref_wf, OUT_PREFIX, out_dir)
        _save_aligned_csv(waveforms, ref_wf, OUT_PREFIX, out_dir)
        _save_decimated_csv(waveforms, ref_wf, OUT_PREFIX, out_dir)
        _plot_aligned_vs_decimated(out_dir, OUT_PREFIX, CHANNELS)
        _save_screenshot(scope, out_dir, OUT_PREFIX)
    finally:
        scope.close()
        rm.close()


if __name__ == "__main__":
    main()

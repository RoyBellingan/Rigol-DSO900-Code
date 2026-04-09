#!/usr/bin/env python3
import csv
from datetime import datetime
from pathlib import Path

import pyvisa

IP = "192.168.1.162"                     # Scope IP
CHANNELS = ["CHAN1", "CHAN2", "CHAN3", "CHAN4"]   # Choose any subset: CHAN1..CHAN4
OUT_PREFIX = "dho900"
OUT_DIR_PREFIX = "aq_"
CHUNK_POINTS = 250_000                   # bytes per :WAV:DATA? (1 byte/point in BYTE mode)
# After reading full memory, keep this many rows in CSV (evenly decimated over full span).
OUTPUT_POINTS = 10_000


def open_scope(ip: str):
    rm = pyvisa.ResourceManager("@py")
    scope = rm.open_resource(f"TCPIP::{ip}::INSTR")
    scope.timeout = 180_000
    scope.chunk_size = 1024 * 1024
    scope.read_termination = "\n"
    scope.write_termination = "\n"
    return rm, scope


def check_scpi_errors(scope, context: str = "", quiet: bool = False):
    """Read and print all queued SCPI errors from the scope."""
    while True:
        err = scope.query(":SYST:ERR?").strip()
        # Rigol returns '0,"No error"' (or similar) when the queue is empty
        if err.startswith("0,") or err.startswith("0 ") or err == "0":
            break
        if not quiet:
            tag = f" [{context}]" if context else ""
            print(f"SCPI ERROR{tag}: {err}")


def scpi_int(scope, cmd: str) -> int:
    s = scope.query(cmd).strip()
    return int(float(s))


def acquire_memory_depth_points(scope) -> int:
    """
    Resolved acquisition memory in points. Prefer :ACQ:MDEP? (same as aq2.py);
    :ACQuire:MDEPth? often returns AUTO without a numeric depth.
    """
    raw = scope.query(":ACQ:MDEP?").strip().upper()
    if raw != "AUTO" and raw:
        try:
            return int(float(raw))
        except ValueError:
            pass
    raw = scope.query(":ACQuire:MDEPth?").strip().upper()
    if raw == "AUTO" or not raw:
        return 0
    return int(float(raw))


def get_waveform_raw_byte(
    scope, channel: str, requested_points: int, chunk_points: int = CHUNK_POINTS
):
    """
    Read full deep memory in RAW mode (same SCPI pattern as aq2.py).
    Use :WAV:POIN + :WAV:POIN? — on DHO924S :WAV:POINts can disagree with the
    resolved :ACQ:MDEP? depth. BYTE + IEEE block; voltage scaling from preamble.
    """
    scope.write(f":WAV:SOUR {channel}")
    check_scpi_errors(scope, f"WAV:SOUR {channel}")
    scope.write(":WAV:MODE RAW")
    check_scpi_errors(scope, "WAV:MODE RAW")
    scope.write(":WAV:FORM BYTE")
    check_scpi_errors(scope, "WAV:FORM BYTE")
    if requested_points > 0:
        scope.write(f":WAV:POIN {requested_points}")
        # WAV:POIN may fail if this channel has fewer points than requested
        # (e.g. different ADC group). Drain the error and let preamble decide.
        check_scpi_errors(scope, f"WAV:POIN {requested_points}", quiet=True)

    preamble = scope.query(":WAV:PRE?").strip()
    check_scpi_errors(scope, f"WAV:PRE? {channel}")
    parts = [x.strip() for x in preamble.split(",")]
    if len(parts) < 10:
        raise RuntimeError(f"Unexpected preamble for {channel}: {preamble}")

    # Always trust the preamble's point count — it reflects what the scope
    # will actually deliver, even if WAV:POIN was rejected.
    points = int(float(parts[2]))

    xinc = float(parts[4])
    xorig = float(parts[5])
    xref = float(parts[6])
    yinc = float(parts[7])
    yorig = float(parts[8])
    yref = float(parts[9])

    values: list[float] = []
    start = 1

    while start <= points:
        stop = min(start + chunk_points - 1, points)
        scope.write(f":WAV:STAR {start}")
        scope.write(f":WAV:STOP {stop}")
        check_scpi_errors(scope, f"WAV:STAR/STOP {start}..{stop}")
        raw = scope.query_binary_values(
            ":WAV:DATA?",
            datatype="B",
            header_fmt="ieee",
            expect_termination=True,
        )
        check_scpi_errors(scope, f"WAV:DATA? {channel} {start}..{stop}")
        expected = stop - start + 1
        if len(raw) != expected:
            raise RuntimeError(
                f"{channel}: expected {expected} samples for {start}..{stop}, got {len(raw)}"
            )
        for b in raw:
            values.append((b - yref - yorig) * yinc)

        print(f"{channel}: read {start}..{stop} / {points}")
        start = stop + 1

    return {
        "channel": channel,
        "points": len(values),
        "xinc": xinc,
        "xorig": xorig,
        "xref": xref,
        "values": values,
    }


def evenly_spaced_indices(n: int, k: int) -> list[int]:
    """k indices in [0, n-1] spanning the full record (inclusive endpoints)."""
    if n <= 0 or k <= 0:
        return []
    if n <= k:
        return list(range(n))
    if k == 1:
        return [n // 2]
    return [round(j * (n - 1) / (k - 1)) for j in range(k)]


def _ref_time(ref_wf: dict, idx: int) -> float:
    """Timestamp from the reference (longest) channel's preamble."""
    return ref_wf["xorig"] + (idx - ref_wf["xref"]) * ref_wf["xinc"]


def save_single_channel_csv(wf, ref_wf, prefix: str, out_dir: Path):
    """Per-channel CSV with corrected timestamps spanning the full reference time window."""
    n = wf["points"]
    ref_n = ref_wf["points"]
    ratio = (ref_n - 1) / (n - 1) if n > 1 else 1

    filename = out_dir / f"{prefix}_{wf['channel']}.csv"
    with open(filename, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["index", "time_s", "voltage_V"])
        for i, v in enumerate(wf["values"]):
            ref_idx = i * ratio
            t = _ref_time(ref_wf, ref_idx)
            w.writerow([i, t, v])
    print(f"Saved {filename}  ({n} rows)")


def _build_aligned_rows(waveforms, ref_wf):
    """
    Align all channels to the shortest channel's length.
    Longer channels are subsampled (every Nth), shorter keep all points.
    Returns (aligned_length, list of (ref_index, [voltage_per_channel])).
    """
    lengths = [wf["points"] for wf in waveforms]
    aligned_n = min(lengths)
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
                wf_idx = round(i * (wf_n - 1) / (aligned_n - 1)) if aligned_n > 1 else 0
                voltages.append(wf["values"][wf_idx])
        rows.append((ref_idx, voltages))
    return aligned_n, rows


def save_aligned_csv(waveforms, ref_wf, prefix: str, out_dir: Path):
    """All channels at the shortest channel's sample count, shared timestamp."""
    aligned_n, rows = _build_aligned_rows(waveforms, ref_wf)

    header = ["rowid", "time_s"] + [wf["channel"] for wf in waveforms]
    filename = out_dir / f"{prefix}_aligned.csv"
    with open(filename, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        for i, (ref_idx, voltages) in enumerate(rows):
            w.writerow([i, _ref_time(ref_wf, ref_idx)] + voltages)
    print(f"Saved {filename}  ({aligned_n} rows)")


def save_decimated_csv(waveforms, ref_wf, prefix: str, out_dir: Path):
    """Decimated to OUTPUT_POINTS from the aligned data, shared timestamp."""
    aligned_n, all_rows = _build_aligned_rows(waveforms, ref_wf)
    idxs = evenly_spaced_indices(aligned_n, min(OUTPUT_POINTS, aligned_n))

    header = ["rowid", "time_s"] + [wf["channel"] for wf in waveforms]
    filename = out_dir / f"{prefix}_decimated.csv"
    with open(filename, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        for new_i, src_i in enumerate(idxs):
            ref_idx, voltages = all_rows[src_i]
            w.writerow([new_i, _ref_time(ref_wf, ref_idx)] + voltages)
    print(f"Saved {filename}  ({len(idxs)} rows)")


def validate_channels(channels):
    valid = {"CHAN1", "CHAN2", "CHAN3", "CHAN4"}
    bad = [ch for ch in channels if ch not in valid]
    if bad:
        raise ValueError(f"Invalid channels: {bad}. Valid values: {sorted(valid)}")


def main():
    validate_channels(CHANNELS)

    rm, scope = open_scope(IP)
    try:
        out_dir = Path(
            f"{OUT_DIR_PREFIX}{datetime.now().strftime('%Y-%m-%d_%H%M%S')}"
        )
        out_dir.mkdir(parents=True, exist_ok=True)
        print(f"Output directory: {out_dir.resolve()}")

        print(scope.query("*IDN?").strip())

        # Drain any stale errors left in the SCPI error queue
        check_scpi_errors(scope, "startup", quiet=True)

        # RAW internal memory should be read with acquisition stopped
        scope.write(":STOP")
        check_scpi_errors(scope, "STOP")

        memory_depth = acquire_memory_depth_points(scope)
        check_scpi_errors(scope, "ACQ:MDEP")
        print(f"Current memory depth (points): {memory_depth}")

        waveforms = []
        for ch in CHANNELS:
            wf = get_waveform_raw_byte(scope, ch, memory_depth, CHUNK_POINTS)
            waveforms.append(wf)

        ref_wf = max(waveforms, key=lambda w: w["points"])
        for wf in waveforms:
            t0 = _ref_time(ref_wf, 0)
            t1 = _ref_time(ref_wf, ref_wf["points"] - 1)
            ratio = ref_wf["points"] / wf["points"]
            print(f"  {wf['channel']}: {wf['points']} pts (ratio {ratio:.0f}x), "
                  f"time window [{t0:.6e} .. {t1:.6e}]")

        for wf in waveforms:
            save_single_channel_csv(wf, ref_wf, OUT_PREFIX, out_dir)

        save_aligned_csv(waveforms, ref_wf, OUT_PREFIX, out_dir)
        save_decimated_csv(waveforms, ref_wf, OUT_PREFIX, out_dir)
    finally:
        scope.close()
        rm.close()


if __name__ == "__main__":
    main()

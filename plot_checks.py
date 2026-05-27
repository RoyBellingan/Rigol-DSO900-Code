#!/usr/bin/env python3
"""Per-channel PNG from the decimated CSV only (fast sanity check)."""

import csv
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402


def read_decimated_csv(path: Path):
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


def main():
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} <out_dir> <prefix> CHAN1 [CHAN2 ...]", file=sys.stderr)
        sys.exit(1)

    out_dir = Path(sys.argv[1])
    prefix = sys.argv[2]
    channels = sys.argv[3:]

    decimated_path = out_dir / f"{prefix}_decimated.csv"
    ch_names, times, voltages = read_decimated_csv(decimated_path)

    for ch in channels:
        if ch not in voltages:
            print(f"plot: skip {ch} (column missing in {decimated_path.name})")
            continue
        fig, ax = plt.subplots(figsize=(11, 4))
        ax.plot(times, voltages[ch], color="C0", lw=0.8, marker=".", markersize=2)
        ax.set_xlabel("time (s)")
        ax.set_ylabel("voltage (V)")
        ax.set_title(f"{ch}: decimated")
        ax.grid(True, alpha=0.35)
        fig.tight_layout()
        out_png = out_dir / f"{prefix}_{ch}_check.png"
        fig.savefig(out_png, dpi=150)
        plt.close(fig)
        print(f"Saved {out_png}")


if __name__ == "__main__":
    main()

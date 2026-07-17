#!/usr/bin/env python3
"""Average power per 1-second window from a shunt-voltage scope CSV.

CHAN1 / voltage_V is the voltage across a shunt on NEUTRAL.
Current:  I = V_shunt / R
Power:    P ≈ V_line_rms * I_rms   (apparent power; PF assumed 1)

Works with both _CHAN1.csv and the decimated _decimated.csv.
Stdlib only — no pandas/numpy required.

Usage

python3 avg_power_1s.py _decimated.csv          # fast
python3 avg_power_1s.py _CHAN1.csv              # full rate
python3 avg_power_1s.py _CHAN1.csv -R 100 -V 230
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from collections import defaultdict
from pathlib import Path


def load_waveform(path: Path) -> tuple[list[float], list[float]]:
    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        if not reader.fieldnames or "time_s" not in reader.fieldnames:
            raise ValueError(f"{path}: missing time_s column")

        vcol = None
        for col in ("voltage_V", "CHAN1"):
            if col in reader.fieldnames:
                vcol = col
                break
        if vcol is None:
            raise ValueError(
                f"{path}: expected voltage_V or CHAN1 column, got {list(reader.fieldnames)}"
            )

        time_s: list[float] = []
        voltage_v: list[float] = []
        for row in reader:
            time_s.append(float(row["time_s"]))
            voltage_v.append(float(row[vcol]))
        return time_s, voltage_v


def avg_power_per_second(
    time_s: list[float],
    voltage_v: list[float],
    *,
    r_ohm: float,
    v_line_rms: float,
) -> list[dict]:
    if r_ohm <= 0:
        raise ValueError("shunt resistance must be > 0")

    # Accumulate sum(I^2), sum(V^2), count per integer-second bin
    sum_i2: dict[int, float] = defaultdict(float)
    sum_v2: dict[int, float] = defaultdict(float)
    count: dict[int, int] = defaultdict(int)

    for t, v in zip(time_s, voltage_v):
        b = math.floor(t)
        i = v / r_ohm
        sum_i2[b] += i * i
        sum_v2[b] += v * v
        count[b] += 1

    rows = []
    for b in sorted(count):
        n = count[b]
        i_rms = math.sqrt(sum_i2[b] / n)
        rows.append(
            {
                "t_start_s": float(b),
                "t_end_s": float(b + 1),
                "n_samples": n,
                "I_rms_A": i_rms,
                "P_avg_W": v_line_rms * i_rms,
                "P_shunt_avg_W": (sum_v2[b] / n) / r_ohm,
            }
        )
    return rows


def write_results_csv(path: Path, rows: list[dict]) -> None:
    fields = [
        "t_start_s",
        "t_end_s",
        "n_samples",
        "I_rms_A",
        "P_avg_W",
        "P_shunt_avg_W",
    ]
    with path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for row in rows:
            w.writerow(
                {
                    "t_start_s": f"{row['t_start_s']:.6g}",
                    "t_end_s": f"{row['t_end_s']:.6g}",
                    "n_samples": row["n_samples"],
                    "I_rms_A": f"{row['I_rms_A']:.6g}",
                    "P_avg_W": f"{row['P_avg_W']:.6g}",
                    "P_shunt_avg_W": f"{row['P_shunt_avg_W']:.6g}",
                }
            )


def print_table(rows: list[dict]) -> None:
    headers = ("t_start_s", "t_end_s", "n_samples", "I_rms_A", "P_avg_W", "P_shunt_avg_W")
    fmt = "{:>10} {:>8} {:>10} {:>12} {:>10} {:>14}"
    print(fmt.format(*headers))
    for r in rows:
        print(
            fmt.format(
                f"{r['t_start_s']:.1f}",
                f"{r['t_end_s']:.1f}",
                r["n_samples"],
                f"{r['I_rms_A']:.6g}",
                f"{r['P_avg_W']:.6g}",
                f"{r['P_shunt_avg_W']:.6g}",
            )
        )


def main() -> int:
    p = argparse.ArgumentParser(
        description="Compute 1-second average power from shunt voltage CSV"
    )
    p.add_argument(
        "csv",
        nargs="?",
        default="_decimated.csv",
        type=Path,
        help="Input CSV (_decimated.csv or _CHAN1.csv)",
    )
    p.add_argument(
        "-R",
        "--shunt-ohm",
        type=float,
        default=100.0,
        help="Shunt resistance in ohms (default: 100)",
    )
    p.add_argument(
        "-V",
        "--line-rms",
        type=float,
        default=230.0,
        help="Assumed mains RMS voltage (default: 230)",
    )
    p.add_argument(
        "-o",
        "--output",
        type=Path,
        default=None,
        help="Write results CSV (default: <input>_power_1s.csv)",
    )
    args = p.parse_args()

    if not args.csv.is_file():
        print(f"error: file not found: {args.csv}", file=sys.stderr)
        return 1

    time_s, voltage_v = load_waveform(args.csv)
    result = avg_power_per_second(
        time_s,
        voltage_v,
        r_ohm=args.shunt_ohm,
        v_line_rms=args.line_rms,
    )

    out = args.output
    if out is None:
        out = args.csv.with_name(args.csv.stem + "_power_1s.csv")

    write_results_csv(out, result)

    print(
        f"Source: {args.csv}  "
        f"({len(time_s)} samples, "
        f"{time_s[0]:.3f} … {time_s[-1]:.3f} s)"
    )
    print(
        f"Shunt R={args.shunt_ohm:g} Ω,  "
        f"V_line_rms={args.line_rms:g} V,  "
        f"P ≈ V_rms · I_rms"
    )
    print()
    print_table(result)
    print()
    if result:
        sum_i2 = sum((v / args.shunt_ohm) ** 2 for v in voltage_v)
        i_rms = math.sqrt(sum_i2 / len(voltage_v))
        print(f"Overall capture: I_rms={i_rms:.6g} A, P_avg={args.line_rms * i_rms:.6g} W")
    print(f"Wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

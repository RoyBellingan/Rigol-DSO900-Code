#!/usr/bin/env python3
"""
Drive the built-in AFG on a Rigol DHO914S / DHO924S over SCPI.

Uses the :SOURce subsystem (see DHO800/900 Programming Guide §3.25).
Amplitude is peak-to-peak volts (:SOURce:VOLTage:AMPLitude).

Examples:
    # 50 Hz sine at 2 Vpp (defaults), output ON
    python afg_control.py

    # 50 Hz sine at 2 Vpp, output ON
    python afg_control.py --amplitude 2.0
    python afg_control.py --frequency 50 --amplitude 1.5 --verify

    # Disable AFG output (ignores frequency/amplitude)
    python afg_control.py --off

    # Query current settings
    python afg_control.py --query

    # Query current settings
    python afg_control.py -q
"""

from __future__ import annotations

import argparse
import time

import numpy as np
import pyvisa

IP = "192.168.1.162"
DEFAULT_FREQ_HZ = 50.0
DEFAULT_AMPLITUDE_VPP = 2.0
VERIFY_CHANNEL = "CHAN1"


def _open_scope(ip: str):
    rm = pyvisa.ResourceManager("@py")
    scope = rm.open_resource(f"TCPIP0::{ip}::INSTR")
    scope.timeout = 30_000
    scope.read_termination = "\n"
    scope.write_termination = "\n"
    return rm, scope


def _check_scpi_errors(scope, context: str = "", *, quiet: bool = False) -> None:
    while True:
        err = scope.query(":SYST:ERR?").strip()
        if err.startswith("0,") or err.startswith("0 ") or err == "0":
            break
        if not quiet:
            tag = f" [{context}]" if context else ""
            raise RuntimeError(f"SCPI error{tag}: {err}")


def query_afg(scope) -> dict[str, str]:
    return {
        "function": scope.query(":SOURce:FUNCtion?").strip(),
        "frequency_hz": scope.query(":SOURce:FREQuency?").strip(),
        "amplitude_vpp": scope.query(":SOURce:VOLTage:AMPLitude?").strip(),
        "offset_v": scope.query(":SOURce:VOLTage:OFFSet?").strip(),
        "output": scope.query(":SOURce:OUTPut:STATe?").strip(),
    }


def configure_afg(
    scope,
    *,
    frequency_hz: float | None = None,
    amplitude_vpp: float | None = None,
    offset_v: float | None = None,
    waveform: str = "SINusoid",
    output_on: bool | None = None,
) -> None:
    """Apply AFG settings.  None means leave that parameter unchanged."""
    scope.write(f":SOURce:FUNCtion {waveform}")
    if frequency_hz is not None:
        scope.write(f":SOURce:FREQuency {frequency_hz}")
    if amplitude_vpp is not None:
        scope.write(f":SOURce:VOLTage:AMPLitude {amplitude_vpp}")
    if offset_v is not None:
        scope.write(f":SOURce:VOLTage:OFFSet {offset_v}")
    if output_on is not None:
        scope.write(f":SOURce:OUTPut:STATe {'ON' if output_on else 'OFF'}")
    _check_scpi_errors(scope, "configure_afg")


def measure_channel_vpp(scope, channel: str = VERIFY_CHANNEL) -> dict[str, float]:
    """Read the on-screen waveform and return basic amplitude stats."""
    _check_scpi_errors(scope, "pre-WAV", quiet=True)
    scope.write(f":WAV:SOUR {channel}")
    scope.write(":WAV:MODE MAX")
    scope.write(":WAV:FORM WORD")
    time.sleep(0.15)
    _check_scpi_errors(scope, "WAV setup", quiet=True)

    preamble = scope.query_ascii_values(":WAV:PRE?", container=np.ndarray)
    xinc = float(preamble[4])
    yinc = float(preamble[7])
    yref = float(preamble[9])
    chan = channel.replace("CHAN", "")
    offset_v = float(scope.query(f":CHAN{chan}:OFFS?").strip())

    data = scope.query_binary_values(":WAV:DATA?", datatype="H", container=np.ndarray)
    _check_scpi_errors(scope, "WAV:DATA?")
    voltage = (data - yref) * yinc - offset_v

    return {
        "points": len(data),
        "sample_rate_hz": 1.0 / xinc,
        "v_min": float(voltage.min()),
        "v_max": float(voltage.max()),
        "vpp": float(voltage.max() - voltage.min()),
        "vrms": float(np.sqrt(np.mean(voltage**2))),
    }


def _print_state(state: dict[str, str]) -> None:
    on = state["output"] in ("1", "ON")
    print("AFG state:")
    print(f"  waveform   : {state['function']}")
    print(f"  frequency  : {state['frequency_hz']} Hz")
    print(f"  amplitude  : {state['amplitude_vpp']} Vpp")
    print(f"  offset     : {state['offset_v']} V")
    print(f"  output     : {'ON' if on else 'OFF'}")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Configure the Rigol DHO900 built-in AFG (50 Hz sine by default).",
    )
    parser.add_argument("--ip", default=IP, help=f"Scope IP (default: {IP})")
    parser.add_argument(
        "--frequency", "-f", type=float, default=DEFAULT_FREQ_HZ,
        help=f"Sine frequency in Hz (default: {DEFAULT_FREQ_HZ})",
    )
    parser.add_argument(
        "--amplitude", "-a", type=float, default=DEFAULT_AMPLITUDE_VPP,
        help=f"Peak-to-peak amplitude in V (default: {DEFAULT_AMPLITUDE_VPP})",
    )
    parser.add_argument(
        "--offset", type=float, default=0.0,
        help="DC offset in V (default: 0)",
    )
    parser.add_argument(
        "--off", action="store_true",
        help="Disable AFG output (ignores frequency/amplitude)",
    )
    parser.add_argument(
        "--query", "-q", action="store_true",
        help="Only query and print current AFG settings",
    )
    parser.add_argument(
        "--verify", action="store_true",
        help=f"After setup, measure Vpp on {VERIFY_CHANNEL}",
    )
    parser.add_argument(
        "--verify-channel", default=VERIFY_CHANNEL,
        help=f"Channel for --verify (default: {VERIFY_CHANNEL})",
    )
    args = parser.parse_args()

    rm, scope = _open_scope(args.ip)
    try:
        print(scope.query("*IDN?").strip())
        _check_scpi_errors(scope, "startup", quiet=True)

        if args.query:
            _print_state(query_afg(scope))
            return

        if args.off:
            configure_afg(scope, output_on=False)
            print("AFG output disabled.")
            _print_state(query_afg(scope))
            return

        configure_afg(
            scope,
            frequency_hz=args.frequency,
            amplitude_vpp=args.amplitude,
            offset_v=args.offset,
            waveform="SINusoid",
            output_on=True,
        )
        print(
            f"AFG: {args.frequency:g} Hz sine, "
            f"{args.amplitude:g} Vpp, offset {args.offset:g} V, output ON"
        )
        _print_state(query_afg(scope))

        if args.verify:
            stats = measure_channel_vpp(scope, args.verify_channel)
            print(f"\n{args.verify_channel} verification:")
            print(f"  points       : {stats['points']}")
            print(f"  sample rate  : {stats['sample_rate_hz']:.1f} Hz")
            print(f"  V range      : {stats['v_min']:.4f} .. {stats['v_max']:.4f} V")
            print(f"  measured Vpp : {stats['vpp']:.4f} V")
            print(f"  measured Vrms: {stats['vrms']:.4f} V")
    finally:
        scope.close()
        rm.close()


if __name__ == "__main__":
    main()

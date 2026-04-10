# DHO900

Python utility for downloading **deep-memory waveforms** from **Rigol DHO800/DHO900-series** oscilloscopes (tested on DHO924S) over the LAN via SCPI.

## What it does

1. Connects to the scope over **VISA TCP/IP**.
2. Stops the acquisition (`:STOP`).
3. Reads every requested channel's full RAW buffer from internal memory.
4. Exports:
   - **Per-channel CSV** (`_CHAN1.csv`, ...) — every sample with absolute timestamps.
   - **Aligned CSV** (`_aligned.csv`) — all channels at the shortest channel's sample count, shared time axis.
   - **Decimated CSV** (`_decimated.csv`) — down-sampled to `OUTPUT_POINTS` rows.
   - **Verification plots** (`_CHAN*_check.png`) — aligned trace with decimated dots overlaid for quick sanity-checking.

All output goes to a timestamped folder (`aq_YYYY-MM-DD_HHMMSS/`).

## Requirements

- **Python 3.10+** (3.11+ recommended).
- Scope on the LAN with VISA TCP access enabled.
- Dependencies listed in `requirements.txt`:

```
pip install -r requirements.txt
```

The `@py` backend (PyVISA-py) is used — no National Instruments VISA needed on Linux.

## Quick start

```bash
# one-time setup
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt

# edit config at the top of download1.py (IP, CHANNELS, etc.), then:
python download1.py
```

## Configuration (top of `download1.py`)

| Constant | Default | Meaning |
|---|---|---|
| `IP` | `192.168.1.162` | Scope IP address |
| `CHANNELS` | `CHAN1`..`CHAN4` | Which channels to download |
| `CHUNK_POINTS` | `250 000` | Samples per `:WAV:DATA?` request |
| `OUTPUT_POINTS` | `10 000` | Row count for the decimated CSV |
| `RESET_PAUSE` | `0.5` s | Pause between channel reads (see below) |

## Known firmware quirk: WAV subsystem state leak

The DHO800/DHO900 WAV read-back engine is **stateful across channel switches**.  After reading one channel in RAW mode, the internal state (pointers, POIN limit, buffer offsets) is **not** automatically reset.  If you simply switch `:WAV:SOUR` to the next channel, the scope silently returns a **truncated record** — often 1/4 or 1/10 of the real per-channel depth — with **no SCPI error**.

### Symptoms

- Channel read first gets the correct point count (e.g. 1 000 000).
- Subsequent channels get far fewer points (e.g. 250 000, 100 000, or even 50 000).
- Changing the channel order changes which channel is truncated.
- `:WAV:POIN?` reports the reduced value as if it were the real depth.

### Workaround (`_reset_wav_subsystem`)

Before each channel read the script performs a full reinitialisation cycle:

1. `:WAV:MODE NORMal` — flushes the RAW engine state.
2. Reset `:WAV:STAR 1` / `:WAV:STOP 1000` — clears stale chunk pointers.
3. `:WAV:SOUR CHANn` — select the new channel.
4. `:WAV:MODE RAW` + `:WAV:FORM BYTE` — re-enter RAW read mode.
5. `time.sleep(RESET_PAUSE)` — give the firmware time to settle.

This was found empirically on **DHO924S firmware 00.01.02**.  If you still see truncation, increase `RESET_PAUSE` (try `1.0`).

### Other things that do NOT work

| Attempt | Result |
|---|---|
| Omit `:WAV:POIN` entirely | Scope uses a stale value; points vary unpredictably |
| `:WAV:POIN 50000000` (max spec) | Scope rejects it and falls back to a small default (~50k) |
| Probing `:WAV:POIN` with descending values | Each rejected write further corrupts the state |

## Troubleshooting

- **Timeout / connection errors** — confirm IP, firewall, and that the scope accepts VISA TCP connections.
- **SCPI errors at runtime** — the script drains and prints the error queue; check channel selection, memory depth, and acquisition state.
- **Truncated channels** — increase `RESET_PAUSE` or power-cycle the scope.
- **Very few unique voltage values** — this is normal for BYTE (8-bit) format when the signal spans a small fraction of the vertical scale.  Adjusting the V/div on the scope will improve ADC utilisation.

## Other files

| File | Purpose |
|---|---|
| `test2.py` | Earlier single-channel experiment |
| `12bit check.py` | WORD-format (16-bit) feasibility test |
| `scope_analyzer.cpp` | Offline C++ waveform analyser |

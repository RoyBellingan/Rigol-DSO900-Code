# DHO900

Tools for downloading **deep-memory waveforms** from **Rigol DHO800/DHO900-series** oscilloscopes (tested on DHO924S) over the LAN via SCPI, plus offline FFT analysis.

## Quick start (recommended: C++)

```bash
g++ -std=c++20 -O2 -Wall -Wextra -o scope_download \
    scope_download.cpp scope_download_main.cpp scope_analyzer.cpp -lfftw3 -pthread

source myenv/bin/activate
pip install -r requirements.txt

./scope_download              # all channels 1-4
./scope_download 1 3          # CHAN1 and CHAN3 only
./scope_download --channels 1,3,4 --no-plots
```

**Requirements:** scope on the LAN (TCP port **5555**), `libfftw3`, `python3` + `matplotlib` only for verification PNGs (`plot_checks.py`). No VISA install needed.

Output goes to `aq_YYYY-MM-DD_HHMMSS/` with CSVs, PNG checks, screenshot, and **`output.log`** (FFT/harmonic analysis of `_decimated.csv` via `scope_analyzer`).

### `scope_download` options

| Option | Default | Meaning |
|---|---|---|
| `--ip` | `192.168.1.162` | Scope IP address |
| `--port` | `5555` | SCPI TCP port |
| `[channels...]` | `1 2 3 4` | Positional channel numbers (1..4) |
| `--channels` | `1,2,3,4` | Same as positional, comma-separated |
| `--chunk` | `250000` | Samples per `:WAV:DATA?` request |
| `--decimate` | `10000` | Row count for `_decimated.csv` |
| `--reset-pause` | `0.5` | Pause between channel reads (firmware workaround) |
| `--out-prefix` | *(empty)* | File prefix (`_CHAN1.csv`, etc.) |
| `--out-dir-prefix` | `aq_` | Output folder prefix |
| `--no-raw` | off | Skip per-channel full-depth `_*CHAN*.csv` |
| `--no-aligned` | off | Skip `*_aligned.csv` |
| `--xzDecimated` | off | Compress `_decimated.csv` with `xz -6` (removes `.csv`; runs after analysis/plots) |
| `--no-plots` | off | Skip `_CHAN*_check.png` |
| `--no-screenshot` | off | Skip `screenshot.png` |
| `--no-analysis` | off | Skip `output.log` (FFT report) |
| `--fundamental` | `50` | Target fundamental [Hz] for analysis |
| `--max-harmonic` | `15` | Max harmonic order for analysis |

## Python download (`download1.py`)

Legacy/reference implementation using PyVISA-py. Slower CSV export; same SCPI logic and output layout.

### What it does

1. Connects to the scope over **VISA TCP/IP**.
2. Stops the acquisition (`:STOP`).
3. Reads every requested channel's full RAW buffer from internal memory.
4. Exports per-channel, aligned, and decimated CSVs plus verification plots.

### Requirements

- **Python 3.10+** (3.11+ recommended).
- `pip install -r requirements.txt` (PyVISA + PyVISA-py + matplotlib).

```bash
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
| `scope_download` | Fast C++ deep-memory downloader (build from `scope_download*.cpp`) |
| `plot_checks.py` | Verification PNGs from `_decimated.csv` only |
| `download1.py` | Python downloader (reference) |
| `test2.py` | Earlier single-channel experiment |
| `12bit check.py` | WORD-format (16-bit) feasibility test |
| `scope_analyzer.hpp` / `scope_analyzer.cpp` | Reusable C++ waveform analysis library |
| `scope_analyzer_main.cpp` | CLI entry point for the analyser |
| `analyze_capture.sh` | Wrapper: drag a capture folder onto this script |

### Scope analyzer

Build:

```bash
g++ -std=c++20 -O2 -Wall -Wextra -o scope_analyzer scope_analyzer.cpp scope_analyzer_main.cpp -lfftw3
chmod +x analyze_capture.sh
```

Run on a capture folder (reads `_decimated.csv` or `decimated.csv`, writes `output.log`):

```bash
./analyze_capture.sh "220Ohm 0.4A"
# same as: ./scope_analyzer "220Ohm 0.4A"
```

Or with explicit CSV paths:

```bash
./scope_analyzer input.csv output.log [fundamental_hz] [max_harmonic]
```

Reuse the analysis logic from another C++ tool:

```cpp
#include "scope_analyzer.hpp"

scope_analyzer::AnalysisOptions opts;
opts.targetFundamentalHz = 50.0;
opts.maxHarmonic = 15;

auto result = scope_analyzer::analyzeCsvFile("capture/_decimated.csv", opts);
// result.channels, result.pairs, result.fs, ...
```

**Drag-and-drop:** in the file manager, drag a capture folder onto `analyze_capture.sh` (works in Dolphin and most KDE setups). A terminal may flash briefly depending on your file-manager settings.

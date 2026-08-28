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

### Bit depth

The DHO800/900 digitizer is **12-bit**. `:WAV:FORM BYTE` truncates every sample to
8 bits and discards the low 4, so `--format word` is now the default and costs only
transfer time (2 bytes per point instead of 1). Pass `--byte` for the old behaviour.

Measured on a DHO924S (firmware 00.01.02), same vertical window:

| Format | Word LSB | Real step | Effective bits | Quantization noise |
|---|---|---|---|---|
| BYTE | 1.7067 mV | 1.7067 mV | 8.00 | 0.493 mV RMS |
| WORD | 0.006667 mV | **0.11333 mV** | **11.91** | 0.033 mV RMS |

That is a **23.5 dB lower quantization floor** for twice the transfer bytes.

Two things about WORD on this scope are not obvious, and both are handled:

- The 12-bit sample is **scaled across the whole 16-bit word**, not right-aligned in
  0..4095. Codes run 0..65535, `yinc` is the BYTE value divided by 256, and the real
  step is 17 counts, not a power of two. The downloader therefore **measures** the
  smallest step in the record and reports effective bits rather than assuming them.
- Byte order is little endian, confirmed against a BYTE capture of the same signal
  (AC RMS 16.8615 vs 16.8599 mV; 50 Hz line 5.1282 vs 5.1267 mV). Because both byte
  orders stay inside 0..65535, the order is detected by **continuity**: the wrong one
  scrambles the record and inflates its total variation, in practice by about 30x.
  Override with `--word-order le|be`.

`:WAV:PRE?` is read after `:WAV:FORM` is set, so `yinc`/`yref` already describe the
active format and the volt conversion needs no extra scaling.

**WORD does not fix clipping.** Clipping is the vertical window, not the bit depth: a
sample that pegs the rail pegs it at either depth.

A little clipping is usually the right trade, though: filling the window buys
resolution across the whole record, while a few lost peak samples cost very little.
Measured by clipping a real unclipped capture at increasing levels, then comparing a
5 kHz lock-in tone and three bands against the original:

| Clipped | h1 | h3 | 1-10 kHz | 20-100 kHz | 200-800 kHz |
|---|---|---|---|---|---|
| 0.01 % | -0.00 dB | -0.01 dB | -0.00 dB | -0.00 dB | -0.01 dB |
| 0.08 % | -0.01 dB | -0.05 dB | -0.01 dB | -0.03 dB | -0.08 dB |
| 0.29 % | -0.03 dB | -0.09 dB | -0.04 dB | -0.09 dB | -0.29 dB |
| 0.96 % | -0.09 dB | -0.21 dB | -0.12 dB | -0.65 dB | **-2.32 dB** |
| 2.81 % | -0.27 dB | -0.45 dB | -0.30 dB | -1.07 dB | -3.26 dB |

Under about 0.3 % every error stays below 0.1 dB. Past 1 % the wideband cost climbs
fast, and the highest band suffers first. Both tools therefore report the clipped
percentage but only warn past `--clip-tolerance` (default 0.5 %); `scope_download`
exits 1 in that case, so a script can catch it. Pass `--clip-tolerance 0` to flag any
clipping at all.

Note the errors above are all negative. Clipping a *periodic* waveform generates
harmonics, but these clipped samples are sparse impulsive peaks, so the effect is
mostly lost energy rather than added spectrum.

Offline checks for the unpack path (no scope needed):

```bash
g++ -std=c++20 -O2 -I. -o test_unpack test_unpack.cpp scope_analyzer.cpp -lfftw3 -pthread
./test_unpack
```

**Requirements:** scope on the LAN (TCP port **5555**), `libfftw3`, `python3` + `matplotlib` only for verification PNGs (`plot_checks.py`). No VISA install needed.

Output goes to `aq_YYYY-MM-DD_HHMMSS/` with per-channel **`_CHAN1.bin` + `_CHAN1.json`**, aligned/decimated CSVs, PNG checks, screenshot, and **`output.log`** (FFT/harmonic analysis of `_decimated.csv` via `scope_analyzer`).

The full-rate record is little-endian signed int16. WORD codes span 0..65535, so each stored sample is `raw = code - 32768`. The sidecar gives `t0`, `dt`, `scale`, and `offset`:

```
t = t0 + i * dt
v = raw * scale + offset
```

`mic_analyzer` and `batRX` still read `_CHAN1.csv`. Pass `--csv` when you need that workflow.

### `scope_download` options

| Option | Default | Meaning |
|---|---|---|
| `--ip` | `192.168.1.162` | Scope IP address |
| `--port` | `5555` | SCPI TCP port |
| `[channels...]` | `1 2 3 4` | Positional channel numbers (1..4) |
| `--channels` | `1,2,3,4` | Same as positional, comma-separated |
| `--format` | `word` | Transfer width: `word` = 12-bit (full digitizer resolution), `byte` = 8-bit |
| `--byte` | off | Shorthand for `--format byte` |
| `--word-order` | `auto` | WORD byte order: `auto`, `le`, `be` |
| `--chunk` | `250000` byte / `125000` word | Samples per `:WAV:DATA?` request |
| `--decimate` | `10000` | Row count for `_decimated.csv` |
| `--clip-tolerance` | `0.5` | Percent of samples allowed on a rail before warning and exit 1 |
| `--reset-pause` | `0.5` | Pause between channel reads (firmware workaround) |
| `--out-prefix` | *(empty)* | File prefix (`_CHAN1.bin`, `_CHAN1.json`, etc.) |
| `--out-dir-prefix` | `aq_` | Output folder prefix |
| `--csv` | off | Also write per-channel full-depth `_*CHAN*.csv` |
| `--no-raw` | off | Skip per-channel int16 `.bin` + `.json` (independent of `--csv`) |
| `--no-aligned` | off | Skip `*_aligned.csv` |
| `--xzDecimated` | off | Compress `_decimated.csv` with `xz -6` (removes `.csv`; runs after analysis/plots) |
| `--no-plots` | off | Skip `_CHAN*_check.png` |
| `--no-screenshot` | off | Skip `screenshot.png` |
| `--no-analysis` | off | Skip `output.log` (FFT report) |
| `--fundamental` | `50` | Target fundamental [Hz] for analysis |
| `--max-harmonic` | `15` | Max harmonic order for analysis |

## Acoustic / ultrasonic analysis (`mic_analyzer`)

`scope_analyzer` (`output.log`) is built for **50 Hz harmonic analysis of the decimated
file**. At 25 kS/s everything above 12.5 kHz aliases into the audio band, so its numbers
are meaningless for a microphone. Use `mic_analyzer` instead: it reads the **full-rate**
`_CHAN1.csv` and compares a run against a silence capture.

```bash
g++ -std=c++20 -O2 -o mic_analyzer mic_analyzer.cpp -lfftw3 -pthread

./mic_analyzer aq_2026-08-19_193808 --baseline aq_2026-08-19_192331
```

About 0.7 s for a 1 Mpt record. Writes `mic_report.md` into the capture folder and prints
a one-line-per-tone summary to the terminal.

### What it reports

| Section | Content |
|---|---|
| Record integrity | samples, dt jitter, DC, AC RMS, code step (LSB), distinct levels, dB above the quantization floor, clipping warning |
| Band RMS | RMS per band; with `--baseline`, a **three-column differential table** (baseline / run / dB change) |
| Tones | automatic detection, refined by lock-in: exact frequency, amplitude, SNR, block-to-block stability, lift over baseline |
| Harmonics | per tone, lock-in at each multiple, with the baseline amplitude beside it |
| Mains hum | harmonic table of the mains fundamental |
| Impulsive interference | high-pass RMS, burst count and duty, and a **mains-phase chi-square test** that separates electrical EMI from acoustic events |
| Findings | rule-based plain sentences drawn from the tables above |

The differential table is the point of the tool. Absolute levels are dominated by EMI;
a band that moves by several dB against a silence baseline is where the stimulus landed,
and bands that move less than about 0.2 dB carry nothing new.

The mains-phase chi-square has 9 degrees of freedom: about 9 if the bursts arrive at
random mains phase, hundreds if they are locked to it. Locked means the energy is
conducted or radiated interference, not sound.

### Options

| Option | Default | Meaning |
|---|---|---|
| `--baseline <dir\|csv>` | *(none)* | Silence capture to compare against |
| `--column <name\|idx>` | first non-index, non-time | Data column |
| `--report <path>` | `<dir>/mic_report.md` | Report path |
| `--tone <hz[,hz...]>` | *(auto)* | Lock-in at these frequencies instead of auto-detecting |
| `--no-auto-tone` | off | Skip automatic detection |
| `--harmonics <n>` | `8` | Harmonics per tone |
| `--keep-harmonics` | off | Do not fold a detection that is a harmonic of a stronger tone |
| `--mains <hz>` | `50` | Mains frequency, `0` disables |
| `--mains-harmonics <n>` | `9` | Mains harmonic count |
| `--nfft <n>` | `32768` | Welch segment length |
| `--min-tone <hz>` | `200` | Ignore detections below this |
| `--tone-lift <db>` | `10` | Detection threshold over floor or baseline |
| `--max-tones <n>` | `6` | Detection cap |
| `--clip-tolerance <pct>` | `0.5` | Clipping allowed before warning |
| `--burst-cut <hz>` | `20000` | Burst high-pass corner |
| `--burst-sigma <x>` | `6` | Burst threshold in sigma |
| `--burst-gap <us>` | `100` | Minimum gap between bursts |
| `--no-bursts` | off | Skip burst analysis |

The input may be a capture folder or a CSV. Given a folder it picks `_CHAN1.csv`, then
`_aligned.csv`, and never the decimated file.

### Typical workflow

```bash
./scope_download 1 --no-plots --csv            # capture silence, note the folder
./scope_download 1 --no-plots --csv            # capture with the stimulus on
./mic_analyzer <stimulus_dir> --baseline <silence_dir>
```

For a known drive frequency, skip detection and go straight to lock-in, which pulls a
tone out well below the visible noise floor:

```bash
./mic_analyzer piezo_run --baseline silence_run --tone 40000 --harmonics 4
```

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

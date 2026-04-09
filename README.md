# DHO900

Small Python utilities for **Rigol DHO900-series** oscilloscopes (for example DHO924S) on the LAN. They talk to the instrument over **VISA/SCPI** (TCP/IP), pull **deep waveform memory** in RAW mode, and write **CSV** files for analysis or plotting elsewhere.

## What it is for

- Capture the full acquisition buffer from one or more channels after a stopped acquisition.
- Export data as:
  - one CSV per channel (`dho900_CHAN1.csv`, …),
  - a time-aligned multi-channel file (`dho900_aligned.csv`),
  - a downsampled file (`dho900_decimated.csv`) with a fixed maximum number of points.

The main script is **`download1.py`**. Other `.py` files in the repo are experiments or earlier variants (for example `aq2.py`); start with `download1.py` unless you know you need another script.

## Requirements

- **Python 3.10+** (3.11+ recommended).
- Scope reachable on your network (Ethernet), with VISA **TCPIP** access enabled as in the Rigol manual.
- Dependencies are listed in `requirements.txt` (`PyVISA`, `PyVISA-py`, `numpy`). The **`@py`** backend uses **PyVISA-py** so you do **not** need National Instruments VISA for typical TCP/IP use on Linux.

## Install (virtual environment)

From the repository root:

```bash
cd /path/to/DHO900
python3 -m venv .venv
source .venv/bin/activate    # Windows: .venv\Scripts\activate
pip install --upgrade pip
pip install -r requirements.txt
```

## Configure and run

1. Edit **`download1.py`** at the top: set **`IP`** to your scope’s address, and **`CHANNELS`** to the subset you want (`CHAN1` … `CHAN4`). Optional: adjust **`CHUNK_POINTS`**, **`OUTPUT_POINTS`**, or **`OUT_PREFIX`** if needed.
2. With the venv activated:

   ```bash
   python download1.py
   ```

3. Output goes to a new folder named like **`YYYY-MM-DD_HHMMSS/`** in the current working directory, containing the CSV files described above.

## Troubleshooting

- **Timeout or connection errors**: confirm IP, firewall, and that the scope accepts VISA TCP connections.
- **SCPI errors** printed at runtime: the script reports the instrument’s error queue; check channel selection, memory depth, and that acquisition was in a state compatible with RAW readout after **`:STOP`**.

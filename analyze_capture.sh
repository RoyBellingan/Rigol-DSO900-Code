#!/usr/bin/env bash
# Drag a capture folder onto this script (or pass the folder as $1).
# Reads _decimated.csv / decimated.csv, writes output.log in that folder.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$ROOT/scope_analyzer"

if [[ ! -x "$BIN" ]]; then
    echo "analyze_capture.sh: build scope_analyzer first:" >&2
    echo "  g++ -std=c++20 -O2 -Wall -Wextra -o scope_analyzer scope_analyzer.cpp scope_analyzer_main.cpp -lfftw3" >&2
    exit 1
fi

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <capture_folder> [fundamental_hz] [max_harmonic]" >&2
    echo "   or: drag a capture folder onto this file in the file manager" >&2
    exit 1
fi

exec "$BIN" "$@"

# SDR Trim

IQ recording utility for SDRplay RSPdx and RSPduo receivers.

Trim, convert, extract channels, and apply Digital Down-Conversion (DDC) to IQ recordings in a single pass. Includes a Windows GUI front-end.

---

## Features

- **Time trim** — cut a recording to a start and end time (HHMM), with automatic midnight crossing
- **Format conversion** — convert between any supported format pair in one pass
- **Channel extraction** — extract tuner A or tuner B from dual-tuner recordings
- **DDC** — extract a narrow frequency slice at a reduced sample rate using a Kaiser-windowed FIR filter
- **Automatic format detection** — input format detected from file header and filename; no flags needed
- **RF64 support** — WAV output automatically switches to RF64 for files over 4 GB
- **Output filename prediction** — output filename generated from recording metadata

## Supported Formats

| Format | Extension | Tuner modes |
|---|---|---|
| Linrad raw | `.raw` | Single and dual tuner |
| WavViewDX raw | `.raw` (headerless) | Single and dual tuner |
| SDRuno WAV | `.wav` | Single tuner |
| SDR Connect WAV | `.wav` | Single tuner |

## Building

### Requirements

- GCC (Linux/macOS) or MSYS2/MinGW64 (Windows)
- No external libraries beyond the standard C library

### CLI tool only

```bash
# Linux / macOS
make sdrtrim

# Windows (MSYS2/MinGW64)
make
```

### CLI tool + GUI (Windows only)

```bash
make
```

This builds both `sdrtrim.exe` and `sdrtrimgui.exe`. Both must be in the same folder.

### Manual build

```bash
# Linux / macOS
gcc -O2 -Wall -o sdrtrim sdrtrim.c -lm

# Windows CLI
gcc -Wall -O2 -o sdrtrim.exe sdrtrim.c -lm

# Windows GUI
windres sdrtrimgui.rc -O coff -o sdrtrimgui.res
gcc -Wall -O2 -o sdrtrimgui.exe sdrtrimgui.c sdrtrimgui.res \
    -lcomctl32 -lcomdlg32 -lshell32 -mwindows -municode
```

## CLI Usage

```
Trim:        sdrtrim <input> <start_HHMM> <end_HHMM> [options]
Full-file:   sdrtrim <input> [options]
```

### Options

| Option | Description |
|---|---|
| `--ch1` | Extract tuner A as single-tuner output |
| `--ch2` | Extract tuner B as single-tuner output |
| `linrad` | Output format: Linrad raw |
| `wavviewdx` | Output format: WavViewDX raw |
| `sdruno` | Output format: SDRuno WAV |
| `sdrconnect` | Output format: SDR Connect WAV |
| `--ddc <kHz> <bw_kHz>` | DDC: extract frequency slice |
| `--fmt <format>` | Output format (compatibility alias) |

Default output format is the same as the input format.

### Examples

```bash
sdrtrim rec.raw 0330 1500
sdrtrim rec.raw 0330 1500 --ch1 sdruno
sdrtrim rec.raw 0330 1500 --ddc 1044 500
sdrtrim rec.raw wavviewdx
sdrtrim rec.raw --ch1
sdrtrim rec.raw --ddc 1044 500 --ch1 sdrconnect
```

## GUI (Windows)

`sdrtrimgui.exe` provides a graphical front-end. Place it in the same folder as `sdrtrim.exe`.

Features:
- Browse or drag-and-drop input file
- All sdrtrim options available via controls
- Predicted output filename shown in real time
- Overwrite warning before running
- Real-time progress bar, percentage and ETA display
- Cancel button
- **Batch mode** — queue multiple jobs from any source files and run sequentially
- Settings and window size remembered between sessions (`sdrtrimgui.ini`)

## Files

| File | Description |
|---|---|
| `sdrtrim.c` | CLI utility source |
| `sdrtrimgui.c` | GUI front-end source |
| `sdrtrimgui.rc` | Windows resource file (embeds manifest) |
| `sdrtrimgui.manifest` | Common Controls v6 manifest |
| `Makefile` | Build rules for all platforms |

## Version History

See the user guide (`sdr_trim_user_guide.docx`) for full version history.

Current version: **1.4**

## License

Copyright © 2026 Dave Headland. All rights reserved.

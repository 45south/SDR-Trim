# SDR Trim  v1.5.1

This is beta software. Contact me if you experience bugs or any other issues with this software. Currently under test. 

IQ recording utility for SDRplay RSPdx, RSPduo, Perseus, and Jaguar receivers.

**Trim · Convert · Extract · DDC**

## Features

- Time trim to second precision (HHMMSS)
- Format conversion between Linrad raw, WavViewDX raw, SDRuno WAV, SDR Connect WAV, Perseus WAV, and Jaguar WAV
- Dual-tuner channel extraction (Tuner A / Tuner B) from RSPduo recordings
- Digital Down-Conversion (DDC) — extract a narrow frequency slice at a reduced sample rate
- Batch job queue
- Overwrite protection

## Supported Formats

| Format | Extension | Notes |
|--------|-----------|-------|
| Linrad raw | .raw | Single and dual tuner |
| WavViewDX raw | .raw | Headerless, metadata in filename |
| SDRuno WAV | .wav | 2 MHz, single tuner |
| SDR Connect WAV | .wav | 2 MHz, single tuner |
| Perseus WAV | .wav | 1,999,000 Hz, single tuner |
| Jaguar WAV | .wav | 1,600,000 Hz, single tuner |

## Building

Requires MSYS2/MinGW-w64 on Windows.

```
make clean && make
```

Output: `sdrtrim.exe`

## Files

| File | Description |
|------|-------------|
| sdrtrim.c | Main source (entire application) |
| sdrtrim.rc | Windows resource file |
| sdrtrim.manifest | Application manifest (Common Controls v6) |
| Makefile | Build script |

## Usage

Run `sdrtrim.exe`. No installation required — settings are saved to `sdrtrim.ini` in the same folder as the executable.

See the user guide for full documentation.

## Author

Dave Headland — [github.com/45south](https://github.com/45south)

# SDR Trim  v1.5.5

IQ recording utility for SDRplay RSPdx, RSPduo, Perseus, and Jaguar receivers.

---

### ⚠️ A Quick Word of Warning: This is an AI-Assisted Project ⚠️

**This project is a personal project for myself.**

Let's be upfront: a large language model (AI) helped write a significant portion of this code, *if not most.* I guided it, reviewed its output the best I could, and tested the result, but this project didn't evolve through the typical trial-and-error of a human-only endeavor. Even this README you're reading was drafted by the AI based on the source code, then edited and refined by me.

Second, it's worth knowing that this was a learning project for myself. The focus was always on getting a practical, working result, which means some of the solutions are probably not what you'd find in a textbook. 

*What does this mean?*

*   **It's Experimental.** While it works, it hasn't been battle-tested across a wide variety of SDR formats.
*   **Design choices not stable.** You may see features etc. suddenly appear and disappear. You may also see large commits of lots of changes. **The mainline codebase may also be broken at times due to fast moving code and changes. Releases may be more stable.** 
*   **Bugs are expected.** The logic very likely has quirks that haven't been discovered yet. Other issues causing crashes likely exist too. 
*   **Use with caution!** Check edited and converted files before deleting your original recordings, if they are important to you.

---

**Trim · Convert · Extract · DDC**

## Features

- Time trim to second precision (HHMMSS)
- Format conversion between Linrad raw, WavViewDX raw, SDRuno WAV, SDR Connect WAV, Perseus WAV, and Jaguar WAV
- Dual-tuner channel extraction (Tuner A / Tuner B) from RSPduo recordings
- Digital Down-Conversion (DDC) — extract a narrow frequency slice at a reduced sample rate
- Sequential recording — join multiple consecutive SDRuno WAV files and trim/convert across file boundaries
- Output folder selection — write output files to any folder independently of the input location
- Pause/Resume while a job is running, without losing place
- Cooperative Cancel — partial output is kept with a correct header, never deleted
- Batch job queue
- Overwrite protection

## Supported Formats

| Format | Extension | Notes |
|--------|-----------|-------|
| Linrad raw | .raw | Single and dual tuner |
| WavViewDX raw | .raw | Headerless, metadata in filename |
| SDRuno WAV | .wav | 2 MHz, single tuner |
| SDR Connect WAV | .wav | Single tuner |
| Perseus WAV | .wav | 24-bit at 125k–1,000k Hz; 16-bit at 2,000k Hz |
| Jaguar WAV | .wav | 1,600,000 Hz and 2,000,000 Hz, single tuner |

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

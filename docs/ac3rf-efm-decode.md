# ac3rf-efm-decode — CLI Reference

`ac3rf-efm-decode` decodes EFM CD audio and AC3-RF surround audio from laserdisc RF captures.
It is also a reusable C++ library; see `player/src/` for the library source.

For a detailed explanation of how AC3-RF decoding works, see [ac3rf-decoding.md](ac3rf-decoding.md).

## Usage

```
ac3rf-efm-decode [options] <input_file>
```

Input is read from stdin if no filename is given. Output is written to stdout by default.

## Modes

| Option | Description                                                                       |
|---|-----------------------------------------------------------------------------------|
| `--ac3` | Decode AC3-RF surround audio (default)                                            |
| `--efm` | Decode EFM baseband audio                                                         |
| `--efm-rf` | Decode EFM from RF input (runs bandpass + envelope detection before EFM decoding) |
| `--efm-t-values` | Decode EFM T-values (raw run-length encoded EFM symbols, u8 input)                |
| `--resample <Hz>` | Resample input to target frequency and write raw output (no audio decoding)       |

## Input format

| Option | Description |
|---|---|
| `--uint8` / `--sint8` | Force 8-bit unsigned / signed sample format |
| `--uint16` / `--sint16` | Force 16-bit unsigned / signed sample format (little-endian) |
| `--uint16be` / `--sint16be` | Force 16-bit unsigned / signed sample format (big-endian) |
| `--lds` | Force lds format |
| `--flac` | Force FLAC format |
| `--ldf` | Force FLAC-in-Ogg (ldf) format |
| `--sample-freq <Hz>` | Input sample rate in Hz. Default 40 MHz. |
| `--seek <seconds>` | Skip this many seconds of input before decoding |
| `--duration <seconds>` | Stop after decoding this many seconds of audio |

Format is auto-detected from the filename extension (`.u8`, `.s8`, `.u16`, `.s16`,
`.u16be`, `.s16be`, `.lds`, `.flac`, `.ldf`) when no format flag is given.

## Decoding options

| Option | Description |
|---|---|
| `--decimation <n>` | Log₂ decimation factor for EFM timing recovery (default: auto) |
| `--adaptive-filter-size <n>` | Adaptive FIR filter size for EFM timing recovery, 0–100 |
| `--error-concealment <mode>` | Erasure concealment strategy: `none`, `repeat`, `li` (linear interpolation), `ar` (autoregressive), `sar` (slow autoregressive). Default: `sar`. |
| `--simd` / `--no-simd` | Enable or disable SIMD (AVX/NEON) FIR filtering. Enabled by default on supported hardware. |

## Output

| Option | Description                                                                                                     |
|---|-----------------------------------------------------------------------------------------------------------------|
| `--output-filename <file>` | Write decoded audio to this file instead of stdout. Use `-` if using multiple output files to revert to stdout. |

Output is raw signed 16-bit little-endian stereo PCM at 44.1 kHz (EFM) or 
a 48 kHz AC3 bitstream (after AC3-RF decoding).

## Logging

| Option | Description |
|---|---|
| `--log <level>` | Log verbosity: 0=off, 1=error, 2=warn, 3=info, 4=debug |
| `--log-filename <file>` | Write log output to this file instead of stderr |
| `--reclock-debug-filename <file>` | Write EFM timing recovery debug data to file |

## Examples

### AC3-RF

Decode AC3-RF from a 40 MHz unsigned 8-bit RF capture, play via a52dec:

```bash
ac3rf-efm-decode capture.u8 | a52dec -o wav | aplay -f S16_LE -r 48000 -c 2
```

Same at 62.5 MHz:

```bash
ac3rf-efm-decode --sample-freq 62500000 capture.u8 | a52dec -o wav | aplay -f S16_LE -r 48000 -c 2
```

Decode AC3-RF from a signed 16-bit lds capture, write the raw AC3 bitstream to a file:

```bash
ac3rf-efm-decode capture.lds --output-filename audio.ac3
```

Decode only 60 seconds starting at 5 minutes in:

```bash
ac3rf-efm-decode --seek 300 --duration 60 capture.u8 | a52dec -o wav > out.wav
```

### EFM

Decode EFM from an RF capture and play immediately (Linux):

```bash
ac3rf-efm-decode --efm-rf capture.u8 | aplay -f S16_LE -r 44100 -c 2
```

Decode EFM from an RF capture and play immediately (macOS):

```bash
ac3rf-efm-decode --efm-rf capture.u8 | ffplay -f s16le -ar 44100 -ch_layout stereo -i pipe:
```

Decode EFM from a FLAC RF capture to a WAV file:

```bash
ac3rf-efm-decode --efm-rf capture.flac | ffmpeg -f s16le -ar 44100 -ch_layout stereo -i pipe: out.wav
```

Decode EFM from a pre-demodulated baseband capture (e.g. written by museld `--efm-baseband`):

```bash
ac3rf-efm-decode --efm capture.s16 | aplay -f S16_LE -r 44100 -c 2
```

Try a different error concealment strategy (useful for badly damaged discs):

```bash
ac3rf-efm-decode --efm-rf --error-concealment ar capture.u8 | aplay -f S16_LE -r 44100 -c 2
```

Increase log verbosity to debug EFM timing recovery:

```bash
ac3rf-efm-decode --efm-rf --log 4 capture.u8 > /dev/null
```

Write EFM timing recovery debug data for analysis in Octave/Python:

```bash
ac3rf-efm-decode --efm-rf --reclock-debug-filename reclock.bin capture.u8 > /dev/null
```

### Resampling

Resample a 62.5 MHz capture to 40 MHz and write raw s16 output:

```bash
ac3rf-efm-decode --sample-freq 62500000 --resample 40000000 capture.u8 > resampled.s16
```

## Python bindings

The `ac3rf` Python module exposes `Ac3RfDemodulator` and `Ac3Decoder` for use in scripts and
notebooks. nanobind is fetched automatically at build time; Python 3.8+ is required.

```bash
cmake -DCMAKE_BUILD_TYPE=Release -S player -B player/build-release
cmake --build player/build-release
# Add the build dir to PYTHONPATH so Python can find the module:
export PYTHONPATH=player/build-release/python
```

Basic usage:

```python
import ac3rf
import numpy as np

logger = ac3rf.StreamLogger(ac3rf.StreamLogger.LOG_WARN)
demod = ac3rf.Ac3RfDemodulator(logger, 40e6, 65536, ac3rf.simd_supported())
decoder = ac3rf.Ac3Decoder(logger)

# Feed float32 samples; call repeatedly as data arrives
samples = np.frombuffer(open("capture.u8", "rb").read(), dtype=np.uint8).astype(np.float32) - 128
symbols = demod.demodulate_to_symbols(samples)
ac3_blocks = decoder.decode_symbols(symbols)   # list of bytes, one per 1536-byte AC3 sync frame
```

## Author and License

Software written by Staffan Ulfberg 2021–2026.

Licensed under the [GNU General Public License v3](../gpl-3.0.txt).

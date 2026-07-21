# ac3rf-efm-decode — CLI Reference

`ac3rf-efm-decode` decodes EFM CD audio and AC3-RF surround audio from laserdisc RF captures.
It is also a reusable C++ library; see `player/src/` for the library source.

For a detailed explanation of how AC3-RF decoding works, see [ac3rf-decoding.md](ac3rf-decoding.md).

## Usage

```
ac3rf-efm-decode [options] <input_file>
```

Multiple input files can be given and are processed in order; each file uses the most recent
input format, sample rate, and output settings, so options can appear between filenames.
Input is read from stdin if no filename is given (the input format must then be specified
explicitly). Output is written to stdout by default.

`--version` prints the build version (the output of `git describe --always --dirty` at build
time); `--help` lists all options.

## Modes

| Option | Description                                                                       |
|---|-----------------------------------------------------------------------------------|
| `--ac3` | Decode AC3-RF surround audio (the default when no mode option is given)           |
| `--efm` | Decode EFM baseband audio (as captured from the EFM output of some players)       |
| `--efm-rf` | Decode EFM from RF input (runs bandpass + envelope detection before EFM decoding) |
| `--efm-t-values` | Decode EFM T-values from ld-decode (raw run-length encoded EFM symbols; implies u8 input) |
| `--resample <Hz>` | Resample input to the target frequency and write raw u8 output (no audio decoding; mostly a development tool) |

## Input format

| Option | Description |
|---|---|
| `--input-format <fmt>` | Input sample type: `u8`, `s8`, `u16`, `s16`, `u16be`, `s16be`, `lds`, `flac`, `ldf`. Auto-detected from filename extension when omitted. |
| `--sample-freq <Hz>` | Input sample rate in Hz. Default 40 MHz. Write `40e6` or `40000000` — not `40`! |
| `--seek <seconds>` | Skip this many seconds of input before decoding |
| `--duration <seconds>` | Stop after decoding this many seconds of audio |

Format is auto-detected from the filename extension (`.u8`, `.s8`, `.u16`, `.s16`,
`.u16be`, `.s16be`, `.lds`, `.flac`, `.flac.ldf`, `.ldf`) when no format flag is given.

`lds` and `ldf` are the file formats used by the [ld-decode](https://github.com/happycube/ld-decode)
toolchain: `lds` files contain 10-bit samples packed so that four samples occupy five bytes, and
`ldf` files contain 16-bit samples compressed with FLAC inside an Ogg container. `flac` files
contain 8- or 16-bit samples in plain FLAC. Some `.ldf` files in the wild are plain FLAC rather
than FLAC-in-Ogg; those need an explicit `--input-format flac` (run `file` on the file to find out
which kind it is).

Seeking in FLAC input only works if the file contains a seek table in its metadata; one can be
added with the `flac` tool (e.g. `--seekpoint=100x` for 100 evenly spaced seek points).

## Decoding options

| Option | Description |
|---|---|
| `--decimation <n>` | Log₂ of the decimation factor applied before EFM decoding; decimation makes decoding faster. Default: the highest decimation that keeps the sample rate before the fractional resampler above 8 MHz. |
| `--adaptive-filter-size <n>` | Adaptive FIR filter size for the EFM signal. Default 3; 0 disables adaptive filtering. Values over 15 are probably overkill, but try 9 or 11 if a distorted input signal causes many errors. |
| `--error-concealment <mode>` | Erasure concealment strategy: `none`, `repeat` (repeat previous sample), `li` (linear interpolation), `ar` (autoregressive), `sar` (slower, more advanced autoregressive). Default: `ar`. |
| `--simd` / `--no-simd` | Enable or disable SIMD (AVX/NEON) FIR filtering. Enabled by default on supported hardware. |

When decoding DTS audio, turn concealment off (`--error-concealment none`): the computed
concealment values are likely worse than the values at erasure positions, which are sometimes
correct.

## Output

| Option | Description                                                                                                     |
|---|-----------------------------------------------------------------------------------------------------------------|
| `--output-filename <file>` | Write decoded audio to this file instead of stdout. Use `-` to select stdout explicitly. |

Output is raw signed 16-bit little-endian stereo PCM at 44.1 kHz (EFM) or 
a 48 kHz AC3 bitstream (after AC3-RF decoding).

Output is never written to a terminal: if stdout is a tty, the program refuses to run unless
stdout is forced with `--output-filename -`. When several input files are processed with the same
output file in effect, the output is appended.

## Logging

| Option | Description |
|---|---|
| `--log <level>` | Log verbosity: 0=off, 1=error, 2=warn, 3=info, 4=debug |
| `--log-filename <file>` | Write log output to this file instead of stderr |
| `--reclock-debug-filename <file>` | Write EFM timing recovery debug data to file |
| `--t-values-output-filename <file>` | Write EFM t-values (run lengths between NRZI transitions) as one unsigned byte per value — the same format as ld-decode `.efm` files. Values are kept in the EFM-legal range 3–11 without changing the total bit count (short runs are merged into the next run, long dropout runs are split into legal chunks), so consumers never lose 588-bit frame alignment. |
| `--circ-debug-filename <file>` | Write the CIRC decoder's frame-by-frame debug dump (data and erasure flags before and after C1 and C2) to a text file. |

The reclock debug file contains binary float pairs: the first float in each pair is the symbol
sample used by the Mueller-Müller timing detector, the second is the computed timing error. It can
be plotted with e.g. gnuplot:

```
s=1861950; l=200; plot "reclock.bin" binary format="%float%float" every ::s::(s+l) using ($1) with linespoints, \
  "" binary format="%float%float" every ::s::(s+l) using ($2*1-1) with points
```

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

Decode EFM from a pre-demodulated baseband capture (as captured from a player's EFM output):

```bash
ac3rf-efm-decode --efm capture.s16 | aplay -f S16_LE -r 44100 -c 2
```

Decode EFM-encoded DTS (concealment must be off for DTS):

```bash
ac3rf-efm-decode --efm-rf --error-concealment none video.ldf | ffplay -f dts -i pipe:
```

Try a different error concealment strategy (useful for badly damaged discs):

```bash
ac3rf-efm-decode --efm-rf --error-concealment sar capture.u8 | aplay -f S16_LE -r 44100 -c 2
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

Resample a 62.5 MHz capture to 40 MHz (the output is raw u8):

```bash
ac3rf-efm-decode --sample-freq 62500000 --resample 40e6 capture.u8 > resampled.u8
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

Licensed under the [GNU General Public License](../gpl-3.0.txt), version 3 or (at your option) any later version.

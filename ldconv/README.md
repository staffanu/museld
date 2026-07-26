# ldconv — laserdisc RF capture format converter

Converts between the sample formats laserdisc RF captures come in: the packed 10-bit
`.lds` of the Domesday Duplicator, the FLAC-compressed `.ldf`, and raw files such as
`.s16`. One program in place of the `ld-lds-converter` + `ffmpeg` + `flac` pipelines
that `ld-compress` runs.

```
ldconv capture.lds capture.ldf        # compress, as ld-compress -c does
ldconv capture.ldf capture.lds        # and back again
ldconv capture.ldf capture.s16        # unpack for a decoder that wants raw samples
ldconv -s 30s -n 10s in.ldf out.s16   # 10 seconds of capture, from 30 seconds in
ldconv --info capture.ldf             # what is this file?
```

`ldconv --help` lists every option. Output is byte-for-byte what ld-decode's own tools
produce, in both directions; `tests/roundtrip.sh` checks that against
`ld-lds-converter` and `ffmpeg` where they are installed.

## Formats

| Name | Extensions | Description |
|---|---|---|
| `lds` | `.lds` | 10-bit unsigned, 4 samples packed into 5 bytes (Domesday Duplicator) |
| `r30` | `.r30` | 10-bit unsigned, 3 samples per little-endian 32-bit word (deprecated) |
| `s8` `u8` | `.s8` `.u8` `.r8` | 8-bit raw (cxadc) |
| `s16` `u16` | `.s16` `.raw` `.u16` `.r16` | 16-bit raw, little endian |
| `s16be` `u16be` | `.s16be` `.u16be` | 16-bit raw, big endian |
| `f32` | `.rf` | 32-bit float, ±1.0 full scale |
| `flac` | `.flac` `.flac.ldf` | native FLAC, as `ld-compress -a` writes it |
| `ldf` | `.ldf` `.raw.oga` `.oga` | FLAC in Ogg, as `ld-compress -c` writes it |

The input format is read from the file's own header where there is one — a `.ldf`
holding a native FLAC stream rather than an Ogg one works either way, which the
extension alone cannot tell you — and from the extension otherwise.
`--input-format`/`--output-format` override both, and are required for the `-`
that means standard input or output.

## Sample values

The formats do not all carry the same number of bits, so values are rescaled to line
full scale up with full scale, and unsigned formats have their zero level moved:
`.lds` code 0..1023 becomes 16-bit −32768..32704 and back, the ×64 that ld-decode's
tools use. `--gain` overrides the factor, `--no-scale` keeps code values as they are.
Anything that falls outside the destination's range is clamped, and counted in the
summary.

FLAC cannot express a 40 MHz sample rate, so the rate is stored in the header in kHz —
40 MHz becomes 40000 Hz, as `ld-compress` does it. `--rate 62.5e6` records 62500 Hz
instead; the samples are unaffected either way.

## Speed

Encoding uses every core. Converting a 1 GB `.lds` (839 M samples, about 21 seconds of
a 40 MHz capture) on a laptop i7-1185G7, four cores with SMT:

| | Time | Output |
|---|---|---|
| `ldconv capture.lds capture.ldf` | 11 s | 590 MB |
| `ldconv --lpc-order 32 --blocksize 16384 capture.lds capture.ldf` | 69 s | 561 MB |
| `ld-compress -c capture.lds` | 70 s | 577 MB |

Most of the difference in the first row is that the default compresses less hard than
`ld-compress` does: a file about 2% larger, in roughly a sixth of the time. Asking for
comparable compression with `--lpc-order 32 --blocksize 16384` — a longer predictor,
and larger frames to amortize its coefficients over — takes about as long as
`ld-compress` and gives the smallest file of the three. Uncompressing is disk-bound
and takes about the same time either way, though `ld-compress` spends three times the
CPU on it.

These are single measurements on one machine and one capture, so treat them as rough:
how much the higher predictor order buys depends on the disc and the capture hardware.

## Building

Needs CMake 3.22, a C++20 compiler and libFLAC++ (1.5 or newer for multithreaded
encoding; older versions build and run single-threaded).

```bash
cmake -DCMAKE_BUILD_TYPE=Release -S ldconv -B ldconv/build-release
cmake --build ldconv/build-release
ldconv/tests/roundtrip.sh ldconv/build-release/ldconv
```

## Licence

GPL v3 or later, as the rest of this repository.

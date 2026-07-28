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

## Resolution

`--bits N` rounds every sample to `N` bits before it is written. It is the only lossy
thing ldconv does, and it is there because the compressed size of an RF capture is
mostly the cost of storing noise: FLAC's predictor cannot predict the low bits, so
each one of them costs about a bit per sample. Dropping the ones that carry no
signal makes the file smaller and changes nothing that a decoder can see.

`N` counts against the output format's full scale, not against the capture's own
range, so it means the same thing whatever went in. A capture that sits in the low
bits of a 16-bit file rather than filling it — 12-bit samples stored as 0..4095, say —
is 12 bits of signal in a range that `--bits` measures as 16, and needs either a
`--gain 16` to line it up first or an `N` four higher to mean the same thing. The
`Values` line from `--info` shows which kind a file is.

The samples stay in the same 16-bit domain — the low bits are zeroed, not removed —
so the output is an ordinary `.ldf` that `ld-decode` and museld read as usual. FLAC
notices that a whole frame shares those zero bits and stores the narrower width, so
the saving is real rather than a pile of zeros.

`--info` reports how much resolution a file actually uses, which is what says how far
`--bits` can go for free:

```
$ ldconv --info capture.ldf
  Values: -20288..25856 in the first 4194304 samples
  Resolution: 10 bit; the low 6 bits of every sample are zero, so
              --bits 10 costs nothing and --bits 9 is the first that rounds
              real capture away
```

A Domesday Duplicator capture is 10-bit, so `--bits 10` and anything above it produce
the identical file. Below that, on 5 seconds of a 40 MHz NTSC capture (200 M samples,
400 MB as `.s16`):

| | Output | Bits/sample | vs 10-bit |
|---|---|---|---|
| `--bits 10` (or none) | 117 MB | 4.66 | — |
| `--bits 9` | 98 MB | 3.91 | −16% |
| `--bits 8` | 83 MB | 3.30 | −29% |
| `--bits 7` | 75 MB | 3.01 | −36% |

The first bit dropped is the one that pays best, because it is the most random; by the
seventh the predictor is already tracking what is left. A 12-bit capture whose lowest
two bits are below its own noise floor should therefore lose rather more than 16% each
from the first two, but that depends on the capture, so measure yours before committing
to it — the original cannot be recovered.

Rounding is to nearest, and a sample already on a step is left where it is, so
`--bits` twice is `--bits` once and re-encoding an already-reduced capture costs
nothing further.

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

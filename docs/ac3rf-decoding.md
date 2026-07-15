# How AC3-RF Decoding Works

This document describes the AC3-RF demodulation and decoding pipeline implemented in
`player/src/ac3/`. AC3-RF is the format used to store AC3 surround audio on Laserdiscs.

The modulation scheme is documented in
[US patent 5,748,834](https://patents.google.com/patent/US5748834A/en), but the framing,
interleaving, and Reed-Solomon encoding are not described in any prior published source;
they were reverse-engineered for this implementation.

## Overview

The AC3 bitstream is QPSK-modulated onto a 2.88 MHz carrier, Reed-Solomon encoded in two
layers (C1 and C2), and interleaved. The decoding pipeline reverses these steps:

```
RF → half-band decimation → IQ mixing to baseband (2.88 MHz NCO)
  → I/Q decimation → root-raised-cosine filter → differential QPSK detection
  → symbol timing DPLL → frame sync → de-interleave → Reed-Solomon C1/C2 → AC3 bitstream
```

All filter parameters and decimation depths are computed from the input sample rate at
startup; any capture rate of at least 7 MHz works.

## 1. Decimation and mixing to baseband

The input signal is first decimated by a power of two using Kaiser-windowed half-band
low-pass stages, keeping the sample rate above 7 MHz. The decimated signal is then
multiplied by a complex exponential at the 2.88 MHz carrier frequency, shifting the AC3
band to baseband as a complex I/Q signal. The mixer is a free-running numerically
controlled oscillator: a 14-bit integer phase accumulator indexing a 16384-entry complex
exponential table. No carrier phase recovery is attempted — thanks to the differential
encoding (below), a small frequency error only appears as a slow, constant rotation of the
constellation, which the differential detector tolerates.

The I/Q signal is decimated further by half-band stages, keeping at least 5 samples per
symbol, and finally filtered with a root-raised-cosine filter (roll-off 0.7, spanning
±3 symbol periods). There is no explicit bandpass filter at RF; the band selection to
2.88 MHz ± 150 kHz happens at baseband in these low-pass stages.

## 2. Carrier and symbol rate

The carrier frequency of 2.88 MHz is exactly 10× the QPSK symbol rate of 288 kSymbols/s.
Each symbol carries two bits, giving a raw bit rate of 576 kbit/s.
There are 10 cycles of the carrier for each symbol period.

## 3. Differential QPSK detection

QPSK encodes two bits per symbol as one of four phase states: 45°, 135°, 225°, or 315°
relative to the carrier. Rather than recovering an absolute carrier reference, the encoder
uses differential encoding: each transmitted symbol is the previous symbol's phase *plus*
the data symbol (mod 4). The decoder therefore looks at the *phase difference* between
consecutive symbols, which is always 0°, 90°, 180°, or 270°, and the original carrier
phase is irrelevant.

The detector runs on the filtered baseband I/Q signal at the full (post-decimation) sample
rate: each sample is multiplied by the complex conjugate of the sample one symbol period
earlier, so the argument of the product is the phase advance over exactly one symbol.
The decision is a simple quadrant test — whichever of |Re| and |Im| is larger selects the
axis, and its sign resolves the direction: 0° → symbol 0, 90° → 1, 270° → 2, 180° → 3.

(An earlier version of this decoder instead used the 1-bit demodulation scheme described
in the patent — sums of products of hard-limited samples at four different lags — which is
attractive because it is cheap to implement in hardware. Processing the full-resolution
signal is more robust with noisy captures.)

## 4. Symbol clock recovery (DPLL)

The symbol stream is not aligned to the input sample clock, so a digital phase-locked loop
selects which of the ~5+ differential decisions per symbol period to emit. A 10-bit
counter advances by a nominal per-sample step (symbol rate / sample rate × 2¹⁰); each time
it wraps around, the current decision is output as the next symbol.

The phase detector observes where the detected symbol value *changes* within the counter
cycle: symbol transitions should occur half a period away from the sampling instant, and
their offset from the counter midpoint is the phase error. Only cycles containing exactly
one transition update the loop — multiple toggles within one symbol period indicate noise
and are ignored. In practice there are enough clean transitions to keep the loop locked
reliably. The error drives a proportional-plus-integral loop filter whose gains are
computed from an explicit second-order loop design: natural frequency 1800 Hz, damping
factor 0.6. The designed gains are divided by the combined phase-detector/VCO gain (≈0.3;
the detector's average gain is well below unity because only cycles with a single clean
transition contribute an error update), so that the realized closed-loop dynamics match
the design values.

## 5. Frame synchronisation and byte alignment

Symbols are grouped four at a time into bytes, most significant symbol first. To establish
byte (and frame) boundaries, the decoder searches for the synchronisation pattern:

```
0 1 1 3 <n n n n> 0 0 0 0
```

The `nnnn` field encodes a counter 0–71 (using symbol pairs 0000–1013) that increments
on each occurrence of the pattern. The sync pattern repeats every **160 symbols** (40 bytes),
so there are **37 data bytes** between consecutive sync patterns. Groups of 72 consecutive
sync patterns (frames) form one complete block.

## 6. Reed-Solomon C1 — inner code

The inner RS code operates on pairs of consecutive frames. Each pair forms a 74-byte
double-frame. Taking every other byte of the double-frame yields two separate 37-byte
sequences, each of which is a C1 codeword. C1 is a **(37, 33)** Reed-Solomon code over
GF(2⁸), so it carries 33 data bytes and 4 parity bytes. The two codewords are decoded
independently, but the interleaved byte order is preserved in the output.

## 7. Reed-Solomon C2 — outer code and de-interleaving

The outer code requires de-interleaving. The 72 frames (36 double-frames) are written
into a **36 × 74** matrix, two consecutive frames per row. Reading the *columns* of this
matrix gives C2 codewords: 36 symbols per column, with the first 32 rows holding data
and the last 4 rows holding parity. C2 is a **(36, 32)** Reed-Solomon code.

There are 74 columns in total; discarding the C1 parity bytes (columns 33 and 37 of each
pair) leaves **66 data columns**. The decoded data is read column by column.

Both C1 and C2 use the same field parameters:

| Parameter | Value |
|---|---|
| Field | GF(2⁸) |
| Irreducible polynomial | x⁸ + x⁷ + x² + x + 1 (0x187) |
| Primitive element | 2 |
| First consecutive root | 2¹²⁰ |

## 8. Block structure and AC3 framing

Each fully-decoded block contains 66 × 32 = **2112 bytes**. The first two bytes are always
`0x10 0x00` and are not part of the AC3 payload.

There are **25 blocks per second** (288,000 symbols/s ÷ 160 symbols/frame ÷ 72 frames/block),
carrying exactly **31.25 AC3 sync frames** per second. An AC3 sync frame at 48 kHz is
1536 bytes, so four input blocks carry five sync frames (4 × 2110 usable bytes > 5 × 1536).
The remaining space is zero-padded.

An AC3 sync frame begins with the sync word `0x0B77`, followed by a 2-byte CRC, then a
byte whose value is always `0x1C` on Laserdiscs (encoding 48 kHz sample rate and 1536-byte
frame size per the AC3 specification). Once the start of a sync frame is located, the
decoder reads 1536 bytes, then skips zero bytes until the next `0x0B77` word.

## Acknowledgments

Thanks to Ian and Leighton Smallshire for convincing me to look for the inner Reed-Solomon code! When I first figured out how this all works I didn't realize that a second layer of encoding was being used, and assumed that the parity bytes represented some auxiliary data.

## References

- US Patent 5,748,834 — QPSK modulation of AC3 on Laserdisc
- ATSC A/52 — AC3 audio coding standard

## Author and License

Software written by Staffan Ulfberg 2021–2026.

Licensed under the [GNU General Public License](../gpl-3.0.txt), version 3 or (at your option) any later version.

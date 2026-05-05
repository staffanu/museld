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
RF → bandpass FIR → decimation → IQ mixing (DPLL) → QPSK demodulation
  → frame sync → de-interleave → Reed-Solomon C1/C2 → AC3 bitstream
```

## 1. Bandpass filtering

The input RF signal is first passed through a bandpass FIR filter with a passband centred
at 2.88 MHz, ±0.15 MHz. This removes out-of-band noise and the video carrier before
further processing.

## 2. Carrier and symbol rate

The carrier frequency of 2.88 MHz is exactly 10× the QPSK symbol rate of 288 kSymbols/s.
Each symbol carries two bits, giving a raw bit rate of 576 kbit/s.
There are 10 cycles of the carrier for each symbol period.

## 3. QPSK demodulation

QPSK encodes two bits per symbol as one of four phase states: 45°, 135°, 225°, or 315°
relative to the carrier. Rather than recovering an absolute carrier reference, the encoder
uses differential encoding: each transmitted symbol is the previous symbol's phase *plus*
the data symbol (mod 4). The decoder therefore looks at the *phase difference* between
consecutive symbols, which is always 0°, 90°, 180°, or 270°, and the original carrier
phase is irrelevant.

Multiplying pairs of ±1 samples yields +1 if they agree and −1 if they differ. Summing
several such products over a few carrier cycles gives a reliable estimate. The demodulator
adds the 160- and 168-sample products to a first accumulator (in-phase), and the 164- and
172-sample products to a second (quadrature). The accumulator with the larger absolute
value indicates whether the phase shift is a multiple of 0°/180° or 90°/270°; its sign
resolves the ambiguity. The result is a symbol value 0–3.

## 4. Symbol clock recovery (DPLL)

The symbol stream is not aligned to the input sample clock. A digital phase-locked loop
(DPLL) tracks the symbol rate by observing when the demodulated signal transitions between
values. The phase detector looks for signal transitions; transitions accompanied by multiple
glitches (the signal switching more than once before settling) are ignored, and only
clean single-step transitions are used to update the loop. In practice there are enough
clean transitions to keep the DPLL locked reliably.

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

## References

- US Patent 5,748,834 — QPSK modulation of AC3 on Laserdisc
- ATSC A/52 — AC3 audio coding standard
- *Digital Audio on LaserDisc*, Ian (ld-decode project) — framing reverse-engineering

## Author and License

Software written by Staffan Ulfberg 2021–2026.

Licensed under the [GNU General Public License v3](../gpl-3.0.txt).

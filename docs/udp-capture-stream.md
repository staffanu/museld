# The UDP capture stream (a proposal)

How a capture device sends raw RF samples over a network to a player or a recorder, and
what museld's `udp://` input expects.  **This is a proposal, not a standard.**  It is what
the ethadc capture box (an FPGA with an ADC and an Ethernet port, in development) and
museld agree on today, written down so that anyone else sending laserdisc RF over UDP can
look at it before choosing something incompatible, and so that the format can be changed
while nothing much depends on it.  Comments and better ideas are welcome: mail them to
staffan@ulfberg.se.  The header files that are the format as
code live in the ethadc repository and are copied into museld under
`player/third_party/ethadc/` (`Protocol.h` is the wire format, `StreamReassembler.h` what a
receiver does with it); until that repository is public, the copy in museld is the
reference.

## What it is for

A laserdisc RF capture is one channel of samples, 8 to 16 bits each, at whatever rate the
capture device runs: 40 MHz is usual, 30 MHz captures decode fine, MUSE wants 62.5 MHz, and
a CD's EFM is covered by 10 MHz.  That is 10 to 125 MB/s, for as long as a disc side
plays.  The sender is hardware (or a program
standing in for it), the receiver a PC, a NAS or a TV box on the same network, decoding
live or writing a file.  The design follows from that:

- **One way, no handshake.**  The sender pushes datagrams at the sample rate from the
  moment it is switched on, to an address it was configured with; the receiver binds a
  port and listens.  Nothing is negotiated, nothing is acknowledged, and a sender with no
  receiver keeps sending.  This is what a small FPGA can do with a counter and a few
  registers, and it means a receiver can join a running stream.
- **Self-describing packets.**  Every packet carries the sample format and the sample
  rate, so a receiver that joins late, or restarts, knows what it is getting without any
  other channel.
- **Loss is concealed, never resynchronised.**  The samples downstream are a continuous
  stream with no framing of their own, so a lost packet must become exactly as many
  filler samples as it carried, or everything after it is shifted.  Packets are therefore
  a fixed size, and counted.
- **Nothing that is not needed.**  No timestamps (the sample rate is the clock), no
  checksum beyond UDP's, no metadata, no channel count: one channel of one format at one
  rate per stream.  Adding any of these later is what the version byte is for.

## The packet

A UDP datagram: 16 bytes of header, then samples.  All fields little endian, naturally
aligned, fixed size.

| offset | size | field | meaning |
|---|---|---|---|
| 0 | 2 | `magic` | `0xE7AD` |
| 2 | 1 | `version` | 1 |
| 3 | 1 | `format` | the sample format, below |
| 4 | 4 | `seq` | packet counter, counting up by one per packet, wrapping at 2^32 |
| 8 | 4 | `sample_rate` | samples per second, constant for the life of a stream |
| 12 | 2 | `payload_len` | bytes of samples following the header |
| 14 | 2 | `flags` | bit 0: stream restart; bit 1: last packet of the stream |

A receiver rejects a datagram whose magic or version is wrong, whose format is unknown,
whose length is not header plus `payload_len`, or whose payload is not a whole number of
sample groups: a stray packet on the port must not be able to steer the stream.

**`seq` counts packets, not samples or bytes.**  With a payload length that never changes
within a stream, a gap of N in the sequence is N × `payload_len` bytes of samples, and the
sender needs only a counter.  So every packet of a stream has the same `payload_len`, with
one exception: the packet that ends a stream may be shorter, and says so with the end flag.
A sender that streams continuously, the hardware, never sets the end flag; a program
sending a file does.

**The restart flag** is set on the first packet of a stream and whenever the sender starts
its counter over.  A receiver seeing it on a sequence number that cannot belong to the
stream in hand drops what it holds and starts afresh, rather than treating the jump as an
enormous loss; on a sequence number inside its window it is a duplicate or a late copy of
the first packet and is ignored.

**The sample format and rate** are fixed for the life of a stream.  A packet whose format
differs from the stream's is malformed and dropped; a change of either is done by starting
a new stream with the restart flag.

### Sample formats

| `format` | name | sample | bytes per group | samples per group |
|---|---|---|---|---|
| 0 | `u8` | unsigned 8-bit, offset binary | 1 | 1 |
| 1 | `s8` | signed 8-bit, two's complement | 1 | 1 |
| 2 | `u16` | unsigned 16-bit, little endian | 2 | 1 |
| 3 | `s16` | signed 16-bit, little endian | 2 | 1 |
| 4 | `u10p` | unsigned 10-bit, four samples packed in five bytes | 5 | 4 |
| 5 | `s10p` | signed 10-bit (two's complement pattern), the same packing | 5 | 4 |

The names are museld's `--input-format` names, so a stream written to a file is a file
museld plays.  Lower-resolution data in a wider container (a 10-bit ADC in `u16`) is sent
as the device delivers it; the receiver is told nothing about the true resolution and
need not know, except that "zero signal" is then the middle of the data's range, not of
the container's, which is why a receiver conceals a loss by repeating the last sample
rather than writing a midscale value.

The packed 10-bit layout is the Domesday Duplicator's `.lds` packing, chosen so that a
`u10p` stream written to a file *is* an `.lds` file.  Samples s0..s3 become bytes b0..b4,
most significant bits first:

```
b0 = s0[9:2]
b1 = s0[1:0] << 6 | s1[9:4]
b2 = s1[3:0] << 4 | s2[9:6]
b3 = s2[5:0] << 2 | s3[9:8]
b4 = s3[7:0]
```

### Payload size

A payload is a whole number of sample groups, so no sample straddles a packet; a multiple
of 10 bytes satisfies every format.  Two sizes are in use:

| payload | datagram | on the wire (IPv4) | fits |
|---|---|---|---|
| 1440 | 1456 | 1484 | a 1500-byte MTU |
| 8950 | 8966 | 8994 | a 9000-byte MTU (jumbo frames) |

The receiver takes the size from the stream's first packet, so any size up to the UDP
maximum of 65507 bytes of datagram is legal; the two above are what a sender should use
unless it knows its network.  At 40 MHz and 16 bits the small size is 55,000 packets per
second, which a Mac's loopback already drops a tenth of, and a Linux receiver needs
`net.core.rmem_max` raised to hold a few tens of milliseconds; jumbo frames cut the packet
rate sixfold and are the right choice on any network that passes them.

## What a receiver does

This is museld's receiver, and the `ethadc-rx` program's; another receiver may do
differently, but the stream is designed around this behaviour.

- **Reorder window.**  Packets are held in a window of N sequence numbers (256 by default)
  and handed on in order; a packet arriving out of order is put back in place, a duplicate
  is dropped, one arriving after its slot was given up on is counted and dropped.
- **Priming.**  For the first few milliseconds of a stream (5 ms by default) packets are
  buffered and nothing is handed on, so that a stream whose first packets were reordered
  does not lose its head to whichever arrived first.
- **Loss.**  A missing packet is declared lost once a timeout (5 ms by default) has passed
  *and* later packets are waiting behind it; a sender that has simply stopped does not make
  the receiver fill forever.  A lost packet becomes exactly `payload_len` bytes of filler:
  by default the last sample group received, repeated (no DC step whatever the container's
  zero is); midscale or zero on request.  A packet too far ahead to fit the window forces
  everything before it out, concealed, so the stream never stalls on a long gap.
- **Reporting.**  The receiver counts packets, bytes, losses, duplicates, reorders,
  late packets, timeouts and window overruns, and museld's `udp://` input logs them.
- **Back-pressure is not loss.**  Between the receiving thread and the consumer sits a
  bounded queue that drops a whole push when it does not fit and counts it, so a consumer
  that cannot keep up shows as overflow, not as packet loss that looks like the network's.

## Using it

```console
# a file sent as the hardware would, paced to real time, jumbo frames
ethadc-tx --dest 10.0.2.50:5000 --sample-rate 40M --format s16 --throttle --jumbo capture.s16

# the player, bound on all interfaces; the format and rate come from the stream
museld udp://:5000

# or a recorder, with museld's options printed when the stream starts
ethadc-rx --port 5000 > capture.s16
```

museld detects the disc standard from the first samples unless `--input-type` says, and
refuses options that contradict what the packets say.  A `udp://address:port` binds that
address only.  The receive socket buffer should be tens of megabytes; museld asks for 32 MB
and logs what the kernel granted.

## Open questions

Things the proposal leaves out on purpose, which anyone building on it may want
settled first:

- **Discovery.**  The sender is configured with the receiver's address.  An announcement
  (multicast, mDNS) so that a player can find a capture box would be a separate, optional
  thing and should not be needed to receive a stream.
- **More than one channel**, for a device that samples two things at once, or a second
  stream of metadata (the device's gain, the player's state).  A separate stream on another
  port, with its own format byte, keeps this one simple; the version byte is there if the
  header needs to grow.
- **Other containers**: 12-bit packed, 24-bit, floating point.  New format numbers, no
  other change.
- **A timestamp**, for aligning two devices' streams.  Not needed for one stream, since the
  sample rate is the clock; if it is ever added, it belongs in a new version of the header.
- **Loss concealment on the sender's side** (forward error correction) is out of scope: a
  local network that drops packets at these rates is misconfigured, and the receiver's
  counters say so.

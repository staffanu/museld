ac3rf-efm-decode — AC3-RF and EFM audio decoder for laserdisc RF captures
=========================================================================

This is a macOS build of the ac3rf-efm-decode tool from the museld project:

    https://github.com/staffanu/museld

It is a universal build: it runs natively on both Apple Silicon and Intel Macs.
macOS 15 (Sequoia) or newer is required.

It decodes the digital audio carried on laserdiscs from an RF capture:

  * EFM — the CD audio track, decoded with CIRC Reed-Solomon error correction.
    Works for RF from laserdiscs and from CD players. DTS on laserdiscs is also
    EFM-encoded.
  * AC3-RF — the QPSK-modulated AC3 surround track at a 2.88 MHz carrier.

This download contains the command line decoder only. If you also want the
real-time MUSE/NTSC player, download museld-macos-universal.zip instead — it
contains this program as well.

macOS will refuse to run this at first
--------------------------------------

The program is signed, but only ad-hoc — it is not signed with a paid Apple
Developer ID and is not notarised, so Gatekeeper does not trust it. If you
downloaded with a browser, macOS has tagged the files as quarantined and will
refuse to open them. Clear the tag once, on the whole folder:

    xattr -cr /path/to/ac3rf-efm-decode-macos-universal

Alternatively, don't let the tag be set in the first place — quarantine is
applied by the program doing the downloading, and curl does not apply it:

    curl -LO https://github.com/staffanu/museld/releases/latest/download/ac3rf-efm-decode-macos-universal.zip
    unzip ac3rf-efm-decode-macos-universal.zip

Contents
--------

    ac3rf-efm-decode   The decoder.
    lib/               Libraries needed by it.
    gpl-3.0.txt        The licence this software is distributed under.

Keep the files together and run the program from a terminal.

Quick start
-----------

Decode AC3-RF surround audio from a 40 MHz RF capture:

    ./ac3rf-efm-decode --sample-freq 40e6 --output-filename out.ac3 capture.s16

Decode EFM (CD) audio from the same capture:

    ./ac3rf-efm-decode --sample-freq 40e6 --efm-rf --output-filename out.pcm capture.s16

Give --sample-freq the rate the capture was made at; it is not guessed. Input
files ending in .u8, .s16, .lds, .ldf or .flac are recognised by extension,
otherwise pass --input-format. Run with --help for all options, and see
docs/ac3rf-efm-decode.md in the repository for the full reference.

Licence and source code
-----------------------

Software written by Staffan Ulfberg 2021-2026 and distributed under the GNU
General Public License, version 3 or (at your option) any later version — see
gpl-3.0.txt. The complete corresponding source code is at
https://github.com/staffanu/museld.

The libraries in lib/ are unmodified builds from Homebrew (https://brew.sh),
relinked to load from this folder; their sources are available from Homebrew and
from the respective upstream projects.

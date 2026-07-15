ac3rf-efm-decode — AC3-RF and EFM audio decoder for laserdisc RF captures
=========================================================================

This is a Windows build of the ac3rf-efm-decode tool from the museld project:

    https://github.com/staffanu/museld

It decodes the digital audio carried on laserdiscs from an RF capture:

  * EFM — the CD audio track, decoded with CIRC Reed-Solomon error correction.
    Works for RF from laserdiscs and from CD players. DTS on laserdiscs is also
    EFM-encoded.
  * AC3-RF — the QPSK-modulated AC3 surround track at a 2.88 MHz carrier.

This download contains the command line decoder only. If you also want the
real-time MUSE/NTSC player, download museld-windows-x86_64.zip instead — it
contains this program as well.

Contents
--------

    ac3rf-efm-decode.exe   The decoder.
    *.dll                  Runtime libraries needed by it.
    gpl-3.0.txt            The licence this software is distributed under.

Keep the files together and run the program from a terminal (cmd.exe or
PowerShell).

Quick start
-----------

Decode AC3-RF surround audio from a 40 MHz RF capture:

    ac3rf-efm-decode.exe --sample-freq 40e6 --output-filename out.ac3 capture.s16

Decode EFM (CD) audio from the same capture:

    ac3rf-efm-decode.exe --sample-freq 40e6 --efm-rf --output-filename out.pcm capture.s16

Give --sample-freq the rate the capture was made at; it is not guessed. Input
files ending in .u8, .s16, .lds, .ldf or .flac are recognised by extension,
otherwise pass --input-format. Run with --help for all options, and see
docs/ac3rf-efm-decode.md in the repository for the full reference.

Licence and source code
-----------------------

Software written by Staffan Ulfberg 2021-2026 and distributed under the GNU
General Public License v3 — see gpl-3.0.txt. The complete corresponding source
code is at https://github.com/staffanu/museld.

The included DLLs are unmodified library builds from the MSYS2 project
(https://www.msys2.org); their sources are available from MSYS2 and from the
respective upstream projects.

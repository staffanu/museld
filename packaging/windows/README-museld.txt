museld — MUSE/NTSC laserdisc player and AC3-RF/EFM audio decoder
================================================================

This is a Windows build of the museld project:

    https://github.com/staffanu/museld

Contents
--------

    museld.exe             Real-time MUSE (Hi-Vision HD) and NTSC laserdisc decoder.
    ac3rf-efm-decode.exe   Command line AC3-RF and EFM audio decoder.
    shaders\               GPU compute shaders, loaded by museld.exe at startup.
    fonts\                 Bundled subtitle font (Noto Sans JP, SIL Open Font License).
    *.dll                  Runtime libraries needed by the two programs.
    gpl-3.0.txt            The licence this software is distributed under.

Keep the files together — museld.exe looks for shaders\ and fonts\ next to
itself and will not start if they are missing. Unpack the whole folder
somewhere and run the programs from a terminal (cmd.exe or PowerShell).

Video file output
-----------------

museld can write what it decodes to a video file with --write, which needs
FFmpeg. That is what the two downloads differ in:

    museld-windows-x86_64.zip               --write is not available
    museld-windows-x86_64-video-export.zip  --write works (bundles FFmpeg)

Everything else is identical. The FFmpeg libraries are most of the size of the
larger download, so take the small one unless you want to write video files.
Using --write in the small build fails with "FFMPEG is not available".

Requirements
------------

museld.exe needs a GPU with Vulkan support and up-to-date graphics drivers.
ac3rf-efm-decode.exe has no special requirements.

Status: the Windows build of museld.exe is EXPERIMENTAL. It compiles and links,
but has had little testing on real Windows machines — expect problems, and
please report them at https://github.com/staffanu/museld/issues. The
ac3rf-efm-decode.exe decoder is in better shape.

Quick start
-----------

Play a MUSE RF capture (62.5 MHz sample rate is the default):

    museld.exe my-capture.s16

Decode AC3-RF surround audio from a laserdisc RF capture:

    ac3rf-efm-decode.exe --sample-freq 40e6 --output-filename out.ac3 capture.s16

Decode EFM (CD) audio from a laserdisc RF capture:

    ac3rf-efm-decode.exe --sample-freq 40e6 --efm-rf --output-filename out.pcm capture.s16

Both programs list all options with --help. Full documentation is in the docs
directory of the repository.

Licence and source code
-----------------------

Software written by Staffan Ulfberg 2021-2026 and distributed under the GNU
General Public License, version 3 or (at your option) any later version — see
gpl-3.0.txt. The complete corresponding source code is at
https://github.com/staffanu/museld.

The included DLLs are unmodified library builds from the MSYS2 project
(https://www.msys2.org); their sources are available from MSYS2 and from the
respective upstream projects. The bundled font is licensed under the SIL Open
Font License.

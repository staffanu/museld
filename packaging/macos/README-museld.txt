museld — MUSE/NTSC laserdisc player and AC3-RF/EFM audio decoder
================================================================

This is a macOS build of the museld project:

    https://github.com/staffanu/museld

It is a universal build: it runs natively on both Apple Silicon and Intel Macs.
macOS 15 (Sequoia) or newer is required.

macOS will refuse to run this at first
--------------------------------------

The programs are signed, but only ad-hoc — they are not signed with a paid Apple
Developer ID and are not notarised, so Gatekeeper does not trust them. What
happens next depends on how you downloaded them.

If you downloaded with a browser, macOS has tagged the files as quarantined and
will refuse to open them ("cannot be opened because the developer cannot be
verified"). Clear the tag once, on the whole folder:

    xattr -cr /path/to/museld-macos-universal

Alternatively, don't let the tag be set in the first place — quarantine is
applied by the program doing the downloading, and curl does not apply it:

    curl -LO https://github.com/staffanu/museld/releases/latest/download/museld-macos-universal.zip
    unzip museld-macos-universal.zip

(If you go looking for advice about this elsewhere, note that the old trick of
right-clicking and choosing Open no longer works: macOS Sequoia removed it. The
remaining way through the dialog is System Settings > Privacy & Security, where
an "Open Anyway" button appears after a blocked attempt.)

Contents
--------

    museld                 Start museld. Sets up the Vulkan driver, then runs museld-bin.
    museld-bin             The real player: real-time MUSE (Hi-Vision HD) and NTSC decoder.
    ac3rf-efm-decode       Command line AC3-RF and EFM audio decoder.
    shaders/               GPU compute shaders, loaded by museld at startup.
    fonts/                 Bundled subtitle font (Noto Sans JP, SIL Open Font License).
    lib/                   Libraries needed by the programs, including MoltenVK.
    vulkan/icd.d/          Tells the Vulkan loader where to find MoltenVK.
    gpl-3.0.txt            The licence this software is distributed under.

Keep the files together and run the programs from a terminal: museld looks for
shaders/, fonts/ and vulkan/ next to itself. Run ./museld, not ./museld-bin —
the latter will not find the graphics driver.

Minimal and full downloads
--------------------------

museld can write what it decodes to a video file with --write (needs FFmpeg)
and OCR burned-in subtitles with --ocr (needs ONNX Runtime). That is what the
two downloads differ in:

    museld-macos-universal.zip       --write and --ocr are not available
    museld-macos-universal-full.zip  everything works (bundles FFmpeg and
                                     ONNX Runtime)

Everything else is identical. Those libraries are most of the size of the
larger download, so take the small one unless you want the features. Using
--write in the small build fails with "FFMPEG is not available", and --ocr
with "requires a build with -DUSE_OCR=ON".

Requirements
------------

museld draws through Vulkan, which reaches the GPU here via MoltenVK on top of
Metal; MoltenVK is included, so any Mac that runs macOS 15 should work.
ac3rf-efm-decode has no special requirements.

Status: the macOS build of museld is EXPERIMENTAL and has had little testing —
expect problems, and please report them at
https://github.com/staffanu/museld/issues. The ac3rf-efm-decode decoder is in
better shape.

Quick start
-----------

Play a MUSE RF capture (62.5 MHz sample rate is the default):

    ./museld my-capture.s16

Decode AC3-RF surround audio from a laserdisc RF capture:

    ./ac3rf-efm-decode --sample-freq 40e6 --output-filename out.ac3 capture.s16

Decode EFM (CD) audio from a laserdisc RF capture:

    ./ac3rf-efm-decode --sample-freq 40e6 --efm-rf --output-filename out.pcm capture.s16

Both programs list all options with --help. Full documentation is in the docs
directory of the repository.

Licence and source code
-----------------------

Software written by Staffan Ulfberg 2021-2026 and distributed under the GNU
General Public License, version 3 or (at your option) any later version — see
gpl-3.0.txt. The complete corresponding source code is at
https://github.com/staffanu/museld.

The libraries in lib/ are unmodified builds from Homebrew (https://brew.sh),
relinked to load from this folder; their sources are available from Homebrew and
from the respective upstream projects. The bundled font is licensed under the SIL
Open Font License.

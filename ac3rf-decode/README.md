# README #

## C++ AC3RF and EFM demodulator / decoder ###

This directory contains a C++ implementation of an AC3RF demodulator, and of an EFM decoder.  It can be
run on large files and is reasonably fast (several times real-time on a modern CPU).  It runs
successfully run on Linux, FreeBSD, Macos, and Windows.

### Building

The project uses cmake and C++23.  Here are build instructions for some specific platforms.

#### Ubuntu Linux

The following installs most of the dependencies, but you also need a C++ compiler, and I didn't
want to pick one for you.  Recent versions of g++ and clang should both work.  Odds are that
they are already installed.  I'm using Ubuntu 25.10, but I've built the project on several other
versions with minor or no changes.

```console
sudo apt install cmake libeigen3-dev libflac++-dev
git clone https://bitbucket.org/staffanulfberg/ldaudio.git
cd ldaudio/ac3rf-decode
git submodule update --init --recursive
cmake -DCMAKE_BUILD_TYPE=Release -B build-release .
cmake --build build-release
```

#### Macos

In Macos, I use [Homebrew](https://brew.sh/) for the dependencies.  Here too, you need a C++ compiler: you
can use clang from Apple's developer tools, or g++ (e.g., from Homebrew).  Notice that a git
client is also part of the command line developer tools.

```console
brew install cmake eigen flac
git clone https://bitbucket.org/staffanulfberg/ldaudio.git
cd ldaudio/ac3rf-decode
git submodule update --init --recursive
cmake -DCMAKE_BUILD_TYPE=Release -B build-release .
cmake --build build-release
```

#### Windows

In Windows, I've compiled using [MSYS2](https://www.msys2.org).  I used the UCRT64 environment: after
installing MSYS2 there should be a shortcut or menu entry to start a UCRT64 environment bash shell.
There will be confirmation prompts for installing packages, so copy this line by line!

```console
pacman -S git
pacman -S $MINGW_PACKAGE_PREFIX-{cmake,gcc,eigen3,flac}
git clone https://bitbucket.org/staffanulfberg/ldaudio.git
cd ldaudio/ac3rf-decode
git submodule update --init --recursive
cmake -DCMAKE_BUILD_TYPE=Release -B build-release .
cmake --build build-release
```

### Command line options

The following command line options are supported:

```bash
--uint8 --sint8 --uint16 --sint16 --lds --flac --ldf
```
Use one of these options to Specify the input format.  Can be omitted if the input filename ends with ".u8",
".s8", ".u16", ".s16", ".lds", ".flac", or ".ldf" respectively, and the file is in the corresponding format.
For reading from stdin the format always has to be specified.

"lds" and "ldf" are file formats used by the
[ld-decode](https://github.com/happycube/ld-decode) toolchain. "lds" files are 10 bits packed so that 4 10-bit samples occupies 5 bytes.
"ldf" files have 16-bit samples compressed using FLAC, wrapped in an Ogg container.

"flac" files have 8-bit or 16-bit samples compressed using FLAC.

It seems that there are "ldf" files that use plain FLAC for storage. In this case "--flac" has to be specified,
or the program tries to read the file as FLAC inside an Ogg container.  To find out which is the case,
run `file` on the file.

```bash
--sample-freq <frequency>
```
The the sample frequency in Hz.  Default is 40 MHz (written as "40000000" or "40e6", not "40"!). 

```bash
-- seek <seconds>
```
Seeks to the specified time in the input file before starting to decode.  Notice that FLAC files 
cannot be seeked unless they contain a seek table in their metadata.  This can be added using the
`flac` command line tool (e.g., `--seekpoint=100x` for 100 evenly spaced seek points).

```bash
--log <level>
```
Sets the log level.  Default is warn (2).  Supported levels are: 0 (off), 1 (error), 2 (warn), 3 (info), 4 (debug). 

```bash
--simd / --no-simd
```
Use (or do not use) SIMD instructions to speed up FIR filtering.  Not available for all platforms/compilers.
Default is enabled if supported.

```bash
--efm --efm-rf --efm-t-values
```
Specify one of these options to decode EFM (stereo PCM data encoded the way that CDs are encoded).
There are three different ways that the program can read EFM data: baseband EFM (as captured from the EFM
output of some players), RF (the raw signal from te pickup of a modified player), or "T values" from ld-decode.
--efm-t-values also implicitly sets the input file type to unsigned bytes.

```bash
--resample <target sample frequency>
```
Instead of decoding the input, the input is resampled to the specified target sample frequency
using a fractional resampler.  The output is written in uint8 format.
For general resampling operations there are better tools,
but I use this for development.

```bash
--decimation <log2 of decimation factor>
```
Decimates the input to the EFM decoder by a factor of 2^[log2 of decimation factor].
This makes decoding faster.  Default is to decimate using the highest possible decimation
that keeps the sample rate before the fractional resampler over 8 MHz.

```bash
--adaptive-filter-size <size>
```
Enables adaptive filtering of the EFM signal.  Default is 3.
A value of 0 disables adaptive filtering.
Reasonable values for the filter are 2 and up; over 15 is probably overkill.
Try increasing this to 9 or 11 if there are lots of errors, which might be
caused by a distorted input signal.

```bash
--reclock-debug-filename <filename>
```
Outputs a file containing resampled data from the clock recovery stage of the EFM decoder.
This can be shown, e.g., using gnuplot with the following command:

> s=1861950; l=200; plot "cmake-build-relwithdebinfo/reclock.bin" binary format="%float%float" every ::s::(s+l) using (\$1) with linespoints,
> "" binary format="%float%float" every ::s::(s+l) using (\$2*1-1) with points

The file contains binary float pairs. The first float in every pair is the symbol sample used by
the Mueller-Muller timing detector, and the second is the computed timing error.

```bash
--error-concealment <repeat|li|ar|sar|none>
```
For EFM decoding, determines how residual erasures after the CIRC decoder are handled.

There are five options: "none" (no concealment), "repeat" (just repeat the previous sample
value), "li" (use linear interpolation), "ar" (an autoregressive model), and "sar" (a
second, more advanced but slow autoregressive model).

The default is "ar".

When decoding DTS audio, turn concealment off: the computed concealment values are likely worse
than the values in erasure positions, which are sometimes correct.

```bash
--output-filename <filename>
```
Sets the output filename.  Default is to write the output to stdout.  Stdout can also be explicitly
specified using the special filename "-".  If stdout is a terminal, the program will not write
output to stdout unless "-" is specified.

```bash
--log-filename <filename>
```
Sets the filename used for logging.  Default is to write the log to stderr.

```bash
--version
```
Prints the version of the program.  This is the output from `git describe --always --dirty` when the program was built.
The is normally just part of the md5 sum of the latest commit, since there are currently no real release schedule in place.

```bash
[filename]
```
Any other arguments are assumed to be filenames that are read and processed.  Multiple files can be read and are processed in order.
The most recent setting for input format, sample frequency, and output file is used.  If the
same output file is used for consecutive input files, the output is appended.

If no filename is given input is read from stdin.

### Examples

Pipe the output of the AC3RF demodulator to [ffplay](https://ffmpeg.org/ffplay.html):
```bash
./cmake-build-release/ac3rf-decode --sample-freq 24.583e6 --uint8 rf-ac3.24.583MHz.u8 | ffplay -f ac3 -i pipe:
```

Pipe the output of the EFM decoder to play (from the SoX package):
```bash
./cmake-build-release/ac3rf-decode --efm-rf --sample-freq 40e6 capture.lds | play -t raw -c 2 -r 44100 -b 16 -e signed-integer -
```

Pipe the output of the EFM decoder to ffplay:
```bash
./cmake-build-release/ac3rf-decode --efm-rf --sample-freq 40e6 capture.lds | ffplay -f s16le -ar 44100 -ch_layout stereo -i pipe:
```

Pipe the output of the EFM decoder to ffplay (for EFM encoded DTS):
```bash
./cmake-build-release/ac3rf-decode --efm-rf --sample-freq 40e6 --error-concealment none video.ldf | ffplay -f dts -i pipe:
```

Create a raw sample file using the EFM decoder, and convert it to a WAV file:
```bash
./cmake-build-release/ac3rf-decode --efm-rf --sample-freq 40e6 --output-filename output.pcm capture.lds
ffmpeg -f s16le -ar 44100 -ch_layout stereo -i output.pcm output.wav
```
# README #

### C++ AC3RF and EFM demodulator / decoder ###

This directory contains a C++ implementation of an AC3RF demodulator, and of an EFM decoder.  It can be
run on large files and is reasonably fast (several times real-time on a modern CPU).

#### Building

The project uses cmake, and the following commands should build the project on Ubuntu.  This also installs most
of the dependencies, but you also need a C++ compiler, and I didn't want to pick one for you.  Recent versions of
g++ and clang should both work (the project uses C++23).  I'm using Ubuntu 25.10.  For macOS, modify as needed
(I used [Homebrew](https://brew.sh/) for the dependencies; they mostly have similar names, currently "gnuradio-dev" and "flac".)
I also tried compiling on Windows using MSYS2, which worked with no modifications.  I don't promise to keep testing
that is does, however.

```console
sudo apt install cmake libeigen3-dev libflac++-dev
git clone https://bitbucket.org/staffanulfberg/ldaudio.git
cd ldaudio/ac3rf-decode
git submodule update --init --recursive
cmake -DCMAKE_BUILD_TYPE=Release -B build-release .
cmake --build build-release
```

### Command line options

The following command line options are supported:

> --uint8
> --sint8
> --uint16
> --sint16
> --lds
> --flac
> --ldf

The input format.  This option can be omitted if the input file ends with ".u8",
".s8", ".u16", ".s16", ".lds", ".flac", or ".ldf" respectively.  For reading from stdin the format
always has to be specified.

"lds" and "ldf" are file formats used by the
[ld-decode](https://github.com/happycube/ld-decode) toolchain. "lds" files are
10 bits packed so that 4 10-bit samples occupies 5 bytes.
"ldf" files have 16-bit samples compressed using FLAC, wrapped in an Ogg container.

"flac" files have 8-bit or 16-bit samples compressed using FLAC.

> --sample-freq <frequency>

The the sample frequency in Hz.  Default is 40 MHz. 

> -- seek <seconds>

Seeks to the specified time in the input file before starting to decode.

> --log <level>

Sets the log level.  Default is warn (2).  Supported levels are: 0 (off), 1 (error), 2 (warn), 3 (info), 4 (debug). 

> --simd / --no-simd

Use (or do not use) SIMD instructions to speed up FIR filtering.  Not available for all platforms/compilers.
Default is enabled if supported.

> --efm

Tells the decoder that the input is EFM encoded.  Baseband EFM (as captured from the EFM
output of some players) is assumed.

> --efm-rf

Tells the decoder that the input is EFM encoded.  RF input, containing video and EFM is assumed.

> --efm-t-values

Tells the decoder that the input is a file containing "T values" from ld-decode.  The input file
is expected to be unsigned bytes.

> --decimation <log2 of decimation factor>

Decimates the input to the EFM decoder by a factor of 2^[log2 of decimation factor].
This makes decoding faster.  Default is to decimate using the highest possible decimation
that keeps the sample rate before the fractional resampler over 8 MHz.

> --adaptive-filter-size <size>

Enables adaptive filtering of the EFM signal.  Default is 3.  
Reasonable values are 2 and up; over 15 is probably overkill.
Try increasing this to 9 or 11 if there are lots of errors, which might be
caused by a distorted input signal.

> --reclock-debug-filename <filename>

Outputs a file containing resampled data from the clock recovery stage of the EFM decoder.
This can be shown, e.g., using gnuplot with the following command:

> s=1861950; l=200; plot "cmake-build-relwithdebinfo/reclock.bin" binary format="%float%float" every ::s::(s+l) using (\$1) with linespoints,
> "" binary format="%float%float" every ::s::(s+l) using (\$2*1-1) with points

The file contains binary float pairs. The first float in every pair is the symbol sample used by
the Mueller-Muller timing detector, and the second is the computed timing error.

> --error-concealment <repeat|lp|ar|none>

For EFM decoding, determines how residual erasures after the CIRC decoder are handled.

There are four options: "none" (no concealment), "repeat" (just repeat the previous sample
value), "li" (use linear interpolation), and "ar" (an autoregressive model).

The default is "li".

The autoregressive model is best at avoiding artifacts caused by missing samples, especially
for larger gaps.  For very high error rates, however, the algorithm takes very long to run.
This is the reason that it is not the default.

When decoding DTS audio, turn concealment off: the computed concealment values are likely worse
than the values in erasure positions, which are sometimes correct.

> --output-filename <filename>

Sets the output filename.  Default is to write the output to stdout.

> [filename]

Any other arguments are assumed to be filenames that are read and processed.  Multiple files can be read and are processed in order.
The most recent setting for input format, sample frequency, and output file is used.  If the
same output file is used for consecutive input files, the output is appended.

If no filename is given input is read from stdin.

### Examples

Pipe the output of the AC3RF demodulator to [ffplay](https://ffmpeg.org/ffplay.html):
> ./cmake-build-release/ac3rf-decode --sample-freq 24.583e6 --uint8 rf-ac3.24.583MHz.u8 | ffplay -f ac3 -i pipe:

Pipe the output of the EFM decoder to play (from the SoX package):
> ./cmake-build-release/ac3rf-decode --efm-rf --sample-freq 40e6 capture.lds | play -t raw -c 2 -r 44100 -b 16 -e signed-integer -

Pipe the output of the EFM decoder to ffplay:
> ./cmake-build-release/ac3rf-decode --efm-rf --sample-freq 40e6 capture.lds | ffplay -f s16le -ar 44100 -ch_layout stereo -i pipe:

Pipe the output of the EFM decoder to ffplay (for EFM encoded DTS):
> ./cmake-build-release/ac3rf-decode --efm-rf --sample-freq 40e6 --error-concealment none video.ldf | ffplay -f dts -i pipe:

Create a raw sample file using the EFM decoder, and convert it to a WAV file:
> ./cmake-build-release/ac3rf-decode --efm-rf --sample-freq 40e6 --output-filename output.pcm capture.lds
> ffmpeg -f s16le -ar 44100 -ch_layout stereo -i output.pcm output.wav

# README #

### C++ AC3RF demodulator / decoder ###

This directory contains a C++ implementation of an AC3RF demodulator.  It can be
run on large files and the code is probably considerably easier to understand than the original Scala code.

#### Building

The project uses cmake, and the following commands should build the project on Ubuntu.  This also installs most
of the dependencies, but you also need a C++ compiler, and I didn't want to pick one for you.  Recent versions of
g++ and clang should both work (the project uses C++23).  I'm using Ubuntu 25.04.  For macOS, modify as needed
(I used [Homebrew](https://brew.sh/) for the dependencies; they mostly have similar names, currently "gnuradio-dev" and "flac".)

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
> --ldf

The input format.  This option can be omitted if the input file ends with ".u8",
".s8", ".u16", ".s16", ".lds", or ".ldf" respectively.  For reading from stdin the format
always has to be specified.

"lds" and "ldf" are file formats used by the
[ld-decode](https://github.com/happycube/ld-decode) toolchain. "lds" files are
10 bits packed so that 4 10-bit samples occupies 5 bytes.
"ldf" files have 16-bit samples compressed using FLAC.

> --sample-freq [frequency]

The the sample frequency in Hz.  Default is 40 MHz. 

> --log [level]

Sets the log level.  Default is warn (2).  Supported levels are: 0 (off), 1 (error), 2 (warn), 3 (info), 4 (debug). 

> --simd

Use SIMD instructions to speed up FIR filtering.  Not available for all platforms/compilers.

> --efm

Tells the decoder that the input is EFM encoded.  Baseband EFM (as captured from the EFM
output of some players) is assumed.

> --efm-rf

Tells the decoder that the input is EFM encoded.  RF input, containing video and EFM is assumed.

> --adaptive-filter-size [size]

Enables adaptive filtering of the EFM signal.  Default is 0 (off).  
Reasonable values are 3 and up; over 20 is probably overkill.  This 
is an experimental feature but seems to help on same inputs.

> --reclock-debug-filename [filename]

Outputs a file containing resampled data from the clock recovery stage.  This can be shown,
e.g., using gnuplot with the following command:
> s=80000000; l=200;
> plot "reclock.bin" binary format="%float%float" every ::s::(s+l) using (\$1) with lines,
>      "reclock.bin" binary format="%float%float" every ::s::(s+l) using (\$2*1-1) with lines

> --output-filename [filename]

Sets the output filename.  Default is to print the output to stdout.`

> [filename]

Any other filenames are read and processed.  Multiple files can be read and are processed in order.
The most recent setting for input format, sample frequency, and output file is used.  If the
same output file is used for consecutive input files, the output is appended.

If no filename is given input is read from stdin.

### Examples

Pipe the output of the AC3RF demodulator to [ffplay](https://ffmpeg.org/ffplay.html):
> ./cmake-build-release/ac3rf-decode --sample-freq 24.583e6 --uint8 rf-ac3.24.583MHz.u8 | ffplay -f ac3 -i pipe:

Pipe the output of the EFM decoder to play (from the SoX package):
> ./cmake-build-release/ac3rf-decode --efm-rf --simd --sample-freq 40e6 --adaptive-filter-size 7 capture.lds | play -t raw -c 2 -r 44100 -b 16 -e signed-integer -

Pipe the output of the EFM decoder to ffplay (for EFM encoded DTS):
> ./cmake-build-release/ac3rf-decode --efm-rf --simd --sample-freq 40e6 --adaptive-filter-size 7 video.ldf | ffplay -f dts -i pipe:

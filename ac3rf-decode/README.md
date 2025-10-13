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
sudo apt install cmake gnuradio-dev libflac++-dev
git clone https://bitbucket.org/staffanulfberg/ldaudio.git
cd ldaudio/ac3rf-decode
cmake -DCMAKE_BUILD_TYPE=Release -B build-release .
cmake --build build-release
```

### Command line options

The following command line options are supported:

> --uint8
> --sint8
> --uint16
> --sint16
> --ldf

The input format.  Default is signed shorts (sint16). --ldf is for the LDF format used by the
[ld-decode](https://github.com/happycube/ld-decode) toolchain (FLAC compressed 16 bit samples).

> --sample-freq [frequency]

The the sample frequency in Hz.  Default is 40 MHz. 

> --log [level]

Sets the log level.  Default is warn (2).  Supported levels are: 0 (off), 1 (error), 2 (warn), 3 (info), 4 (debug). 
 
> --output-filename [filename]

Sets the output filename.  Default is to print the output to stdout.

> [filename]

Any other filenames are read and processed.  Multiple files can be read and are processed in order.
The most recent setting for input format, sample frequency, and output file is used.  If the
same output file is used for consecutive input files, the output is appended.

If no filename is given input is read from stdin.

### Example

> ./cmake-build-release/ac3rf-decode --log 4 --sample-freq 24.583e6 --uint8 rf-ac3.24.583MHz.u8 | ffplay -codec:a:0 ac3 -i pipe: 


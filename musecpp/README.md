## CPU/GPU Real-time MUSE Decoder ##

This C++ project contains a complete MUSE decoder that decodes MUSE video in real-time.  I've developed the
software on a Lenovo Carbon X1 Gen 9 (Intel Core i7-1185G7, 3.0 GHz, 4 cores, integrated graphics), and
tested that it works and on a MacBook Pro with an M2 processor, and also on an older Intel iMac.

I tried running it on an NVIDIA Jetson Nano but that proved way too slow.

The code is mostly written in C++, with video filtering done by the GPU, using Vulkan and GLSL.

The command line options aren't very well organized and have accumulated over time:

> --big-endian
> --little-endian
> --resample-bytes
> --resample-shorts

Determines the input format; these can not be combined and the names should probably change, possible
when making them more generic and/or refactoring into several independent options.

* --big-endian / -little-endian: inputs are unsigned shorts sampled at the correct phase at 16.2MHz, with only the 10 least significant bits used.
* --resample-bytes: inputs are samples at a higher frequency (specified by its own parameter) as unsigned bytes.
* --resample-shorts: inputs are samples at a higher frequency (specified by its own parameter) as full 16-bit signed little endian shorts.

I used the --big-endian format initially since I built a 10 bit phase correct sampler using an FPGA.  This is also the
format used by the scala tools.  For some reason I later switched to the little endian format, but still have some
files that are big endian.  I would recommend to stop using big endian files since support might be removed in the future.

The oversampled formats are used when reading data directly from, e.g., a digital oscilloscope, or from a 
modified Domesday Duplicator.

Notice that the input is always baseband, so the DC level is important.  This means that the input circuitry
of the Domesday Duplicator has to be modified.  It is possible to capture the RF data and decode it to baseband
using, for example, gnuradio: I've tried this to demonstrate that it works but quality was not great so
more effort is needed.  Notice that if capturing RF data, the Domesday Duplicator also needs its frequency range increased.
All my good captures so far have been captured using a digital USB oscilloscope.

Notice that filed in little endian 16 MHz format are smaller than raw captures.

> --sample-freq

Sets the sample frequency for the oversampled input formats.

> --write

Writes the input stream in the --little-endian format.  This is useful to create small video segments from larger
files, and also to re-code captures from one of the oversampled formats.

> --fifo

Indicates that the input file is really a FIFO (created using the mkfifo command).  Normally, the video is played
back at 30 frames/second (60 fields per second).  When reading from a FIFO, however, it is assumed that data is
being produced in real-time (e.g., by some other program reading from a digital USB oscilloscope), which means that
the input data rate matters.  The program then tries to adjust the playback speed to the incoming data rate,
which means that if data is not available it has to wait (instead of assuming end of file),
and if the input buffer fills up a field is dropped.  This also increases the OS default FIFO buffer size.

> --no-video --no-audio

Turns off video and audio output respectively. Useful for troubleshooting (if, for example, the program
doesn't recognize the audio playback device), and also when using the --write option to recode the input
stream; since no decoding has to be done this is faster.

> --no-sync

Frames are displayed as quick as possible after decoding, without waiting for the next vertical sync. 
Good for benchmarking.

> --full-frames-only --all-fields

Normally, the picture is updated after each field, i.e., 60 times per second.  Specifying --full-frames-only
skips updating every other field, which reduces decoding time at the expense of motion detail.  Try this if
the hardware is too slow for real-time decoding.

> --full-screen
> --seek <seconds>
> --pause

These options affect the initial appearance of the video. 

> --verbose
> --benchmark-shaders
> --help

Auxiliary options.

> filename

Any other option (not starting with !) is interpreted as the name of a file to play back. Several files can be listed
that are played one by one, and other options can appear between the video files.  Each file is played back with the
most recent options in effect.  So, for example, it is possible to play back two files with different input formats
back to back.

Any option starting with ! is ignored, which is practical if re-using long command lines to temporarily disable options.

When running, the following keys control playback:

> GLFW_KEY_Q

Quits the application.

> GLFW_KEY_TAB, GLFW_KEY_ESCAPE

Switches to full screen mode to windowed mode respectively.

> GLFW_KEY_SPACE, GLFW_KEY_N

Space toggles between paused and normal play, N steps one frame forward when paused.

> GLFW_KEY_LEFT, GLFW_KEY_RIGHT

Seeks 10 seconds backwards and forwards, respectively.

> GLFW_KEY_1, GLFW_KEY_2, GLFW_KEY_3

Switches how frame interpolation is done:
* 1: Normal, i.e., motion detection is performed to decide what portions of video are in motion and stationary, respectively.
* 2: Force intra-field interpolation, i.e., forces decoding as if everything is in motion.
* 3: Force inter-frame interpolation, i.e., forces decoding as if there is a still picture.

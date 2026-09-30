# ethadc stream headers

The receiving side of the [ethadc](../../../../ethadc) UDP sample stream, copied verbatim
from that repository's `stream/` directory by `update.sh` (`VERSION` says from which
commit).  `Protocol.h` is the wire format the capture hardware produces; the rest is the
reorder-and-conceal logic, the socket loop and the queue that museld's `udp://` input
(`src/input/EthadcByteSource.*`) is built on.

Do not edit these here: change them in the ethadc repository, where their tests live, and
run `update.sh`.  GPL-3.0-or-later, as museld.

# ethadc stream headers

The receiving side of the [ethadc](../../../../ethadc) UDP sample stream, copied verbatim
from that repository's `stream/` directory by `update.sh` (`VERSION.txt` says from which
commit).  `Protocol.h` is the wire format the capture hardware produces; the rest is the
reorder-and-conceal logic, the socket loop and the queue that museld's `udp://` input
(`src/input/EthadcByteSource.*`) is built on.

Include them as `"ethadc/Protocol.h"` with `third_party` as the include directory, never
with this directory itself on the include path: macOS and Windows file systems ignore case,
so a file here can answer to a standard header's name (a `VERSION` file once stood in for
`<version>` and broke every translation unit that included it).

Do not edit these here: change them in the ethadc repository, where their tests live, and
run `update.sh`.  GPL-3.0-or-later, as museld.

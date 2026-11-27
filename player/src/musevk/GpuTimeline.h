// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_GPUTIMELINE_H
#define MUSECPP_GPUTIMELINE_H

#include <cstdint>
#include <string>

namespace musevk {
    // Measurement hook: MUSELD_GPU_TIMELINE=1 makes every CommandBuffer write a timestamp at
    // its start and its end and report, when it is waited for, one line per submission:
    //
    //   gpu <label> sub <us> wait <us> done <us> gb <ns> ge <ns>
    //
    // sub/wait/done are the CPU's clock (microseconds since the first record: the submit, the
    // call of wait() and its return), gb/ge the GPU's own clock (nanoseconds since the first
    // GPU timestamp seen: the start and the end of the command buffer's execution).  All
    // threads report to stderr under one lock, so the lines of the demodulator (label demod),
    // the decoder's two stages (dec1, dec2) and the display's blit (show) interleave on the
    // one queue they share, and the GPU clock being common to them, the queue's occupancy,
    // the gaps on it and who waits for whom can be read off; tools/gpu-timeline.py does
    // that.  The offset between the two clocks is not known; estimate it from the
    // submissions that started at once (gb - sub at its smallest).  A line "gpu epoch <wall
    // clock>" first ties the CPU times to the log's own stamps.
    class GpuTimeline {
    public:
        static bool enabled();
        static void record(const std::string &label,
                           int64_t cpu_submit_ns, int64_t cpu_wait_ns, int64_t cpu_done_ns,
                           uint64_t gpu_begin_ticks, uint64_t gpu_end_ticks, double tick_ns);
        static int64_t nowNs();
    };
}

#endif //MUSECPP_GPUTIMELINE_H

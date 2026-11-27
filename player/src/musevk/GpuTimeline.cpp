// Copyright 2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <mutex>
#include "GpuTimeline.h"

namespace musevk {
    namespace {
        std::mutex g_mutex;
        bool g_started = false;
        int64_t g_cpu_epoch_ns = 0;
        uint64_t g_gpu_epoch_ticks = 0;

        void emit(const std::string &line) {
            std::fprintf(stderr, "%s\n", line.c_str());
        }
    }

    bool GpuTimeline::enabled() {
        static const bool on = getenv("MUSELD_GPU_TIMELINE") != nullptr;
        return on;
    }

    int64_t GpuTimeline::nowNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void GpuTimeline::record(const std::string &label,
                             int64_t cpu_submit_ns, int64_t cpu_wait_ns, int64_t cpu_done_ns,
                             uint64_t gpu_begin_ticks, uint64_t gpu_end_ticks, double tick_ns) {
        std::scoped_lock lock(g_mutex);
        if (!g_started) {
            g_started = true;
            g_cpu_epoch_ns = cpu_submit_ns;
            g_gpu_epoch_ticks = gpu_begin_ticks;
            const auto wall = std::chrono::system_clock::now();
            // the wall clock of this moment, which is nowNs() - cpu_epoch microseconds after the epoch
            emit(std::format("gpu epoch {:%Y-%m-%d %H:%M:%S} is {} us after the first submit",
                             std::chrono::floor<std::chrono::microseconds>(wall), (nowNs() - g_cpu_epoch_ns) / 1000));
        }
        emit(std::format("gpu {} sub {} wait {} done {} gb {} ge {}", label,
                         (cpu_submit_ns - g_cpu_epoch_ns) / 1000, (cpu_wait_ns - g_cpu_epoch_ns) / 1000,
                         (cpu_done_ns - g_cpu_epoch_ns) / 1000,
                         (int64_t)((double)((int64_t)(gpu_begin_ticks - g_gpu_epoch_ticks)) * tick_ns),
                         (int64_t)((double)((int64_t)(gpu_end_ticks - g_gpu_epoch_ticks)) * tick_ns)));
    }
}

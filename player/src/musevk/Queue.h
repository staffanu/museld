// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_QUEUE_H
#define MUSECPP_QUEUE_H

#include <vulkan/vulkan.hpp>
#include <mutex>

namespace musevk {
    // Every command below takes its arguments by pointer, which is what picks
    // vulkan.hpp's plain wrapper -- noexcept, and hands back the vk::Result. The
    // overloads taking references or values are the "enhanced" ones, which throw
    // on any result the command does not list as a success. They are the same
    // name and differ only in how an argument is passed, so the choice is easy to
    // make by accident: presentKHR used to, and a resized window (a normal
    // eErrorOutOfDateKHR) killed museld on Windows as a result.
    class Queue {
    public:
        explicit Queue(vk::Queue queue) :
        m_queue(queue),
        m_mutex() {}

        Queue(const Queue &) = delete;
        Queue &operator=(const Queue &) = delete;

        [[nodiscard]] vk::Result submit(vk::SubmitInfo &submit_info, vk::Fence &fence) {
            std::scoped_lock lock(m_mutex);
            return m_queue.submit(1, &submit_info, fence);
        }

        [[nodiscard]] vk::Result presentKHR(vk::PresentInfoKHR presentInfo) {
            std::scoped_lock lock(m_mutex);
            return m_queue.presentKHR(&presentInfo);
        }

    private:
        vk::Queue m_queue;
        std::mutex m_mutex;
    };
}

#endif //MUSECPP_QUEUE_H

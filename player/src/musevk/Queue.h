// Copyright 2024-2026 Staffan Ulfberg
// This file is licensed under the provisions of the GNU General Public License v3 or later (see gpl-3.0.txt)

#ifndef MUSECPP_QUEUE_H
#define MUSECPP_QUEUE_H

#include <vulkan/vulkan.hpp>
#include <mutex>

namespace musevk {
    class Queue {
    public:
        explicit Queue(vk::Queue queue) :
        m_queue(queue),
        m_mutex() {}

        Queue(const Queue &) = delete;
        Queue &operator=(const Queue &) = delete;

        void submit(vk::SubmitInfo &submit_info, vk::Fence &fence) {
            std::scoped_lock lock(m_mutex);
            m_queue.submit(submit_info, fence);
        }

        [[nodiscard]] vk::Result presentKHR(vk::PresentInfoKHR presentInfo) {
            std::scoped_lock lock(m_mutex);
            // Pass a pointer, not a reference: the reference overload is
            // vulkan.hpp's enhanced one, which throws eErrorOutOfDateKHR rather
            // than returning it. A resized window is normal, and the caller
            // handles it -- so take the noexcept overload and keep the result.
            return m_queue.presentKHR(&presentInfo);
        }

    private:
        vk::Queue m_queue;
        std::mutex m_mutex;
    };
}

#endif //MUSECPP_QUEUE_H

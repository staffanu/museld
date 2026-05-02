//
// Created by staffanu on 3/19/24.
//

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
            return m_queue.presentKHR(presentInfo);
        }

    private:
        vk::Queue m_queue;
        std::mutex m_mutex;
    };
}

#endif //MUSECPP_QUEUE_H

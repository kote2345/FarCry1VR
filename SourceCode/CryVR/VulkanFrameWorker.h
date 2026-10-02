#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace CryVR
{
// One persistent CPU worker. Jobs receive immutable frame snapshots and write
// disjoint memory; Vulkan queue and engine state remain on the render thread.
class VulkanFrameWorker
{
public:
    ~VulkanFrameWorker() { Stop(); }

    void Run(std::function<void()> job)
    {
        Wait();
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_thread.joinable())
        {
            m_stopping = false;
            m_thread = std::thread([this] { Work(); });
        }
        m_job = std::move(job);
        m_busy = true;
        m_ready.notify_one();
    }

    void Wait()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_done.wait(lock, [this] { return !m_busy; });
    }

    void Stop()
    {
        Wait();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
            m_ready.notify_one();
        }
        if (m_thread.joinable()) m_thread.join();
    }

private:
    void Work()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        for (;;)
        {
            m_ready.wait(lock, [this] { return m_busy || m_stopping; });
            if (m_stopping) return;
            auto job = std::move(m_job);
            lock.unlock();
            job();
            lock.lock();
            m_busy = false;
            m_done.notify_one();
        }
    }

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_ready, m_done;
    std::function<void()> m_job;
    bool m_busy = false, m_stopping = false;
};
}

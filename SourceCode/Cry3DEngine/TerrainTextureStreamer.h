#pragma once

#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

// Owns a separate loose-file handle. No CryPak, renderer or sector objects are
// accessed by the worker. Requests and completed data share a fixed byte cap.
class TerrainTextureStreamer
{
public:
    explicit TerrainTextureStreamer(const char* path) : m_file(std::fopen(path, "rb"))
    {
        if (m_file) m_thread = std::thread([this] { Work(); });
    }
    ~TerrainTextureStreamer()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
            m_ready.notify_one();
        }
        if (m_thread.joinable()) m_thread.join();
        if (m_file) std::fclose(m_file);
    }
    bool Available() const { return m_file != nullptr; }
    // 0: pending/queue full, 1: ready, -1: failed (caller may retry synchronously).
    int Read(unsigned offset, unsigned size, unsigned char* destination)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto key = std::make_pair(offset, size);
        auto found = m_requests.find(key);
        if (found != m_requests.end())
        {
            if (!found->second.done) return 0;
            const bool ok = found->second.ok;
            if (!destination) return ok ? 1 : -1;
            if (ok) std::memcpy(destination, found->second.bytes.data(), size);
            m_bytes -= found->second.bytes.size();
            m_requests.erase(found);
            return ok ? 1 : -1;
        }
        if (!size || size > kByteLimit) return -1;
        // Completed requests for sectors no longer visible may be discarded.
        for (auto it = m_requests.begin(); it != m_requests.end() &&
             (m_bytes + size > kByteLimit || m_requests.size() >= 32);)
        {
            if (it->second.done)
            {
                m_bytes -= it->second.bytes.size();
                it = m_requests.erase(it);
            }
            else ++it;
        }
        if (m_bytes + size > kByteLimit || m_requests.size() >= 32) return 0;
        auto& request = m_requests[key];
        request.bytes.resize(size);
        m_bytes += size;
        m_queue.push_back(key);
        m_ready.notify_one();
        return 0;
    }
private:
    struct Request { std::vector<unsigned char> bytes; bool done = false, ok = false; };
    void Work()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        for (;;)
        {
            m_ready.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_stop) return;
            const auto key = m_queue.front();
            m_queue.pop_front();
            auto& request = m_requests.find(key)->second;
            unsigned char* buffer = request.bytes.data();
            lock.unlock();
            const bool ok = std::fseek(m_file, key.first, SEEK_SET) == 0 &&
                std::fread(buffer, 1, key.second, m_file) == key.second;
            lock.lock();
            request.ok = ok;
            request.done = true;
        }
    }
    static constexpr size_t kByteLimit = 16 * 1024 * 1024;
    FILE* m_file = nullptr;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::map<std::pair<unsigned, unsigned>, Request> m_requests;
    std::deque<std::pair<unsigned, unsigned>> m_queue;
    size_t m_bytes = 0;
    bool m_stop = false;
};

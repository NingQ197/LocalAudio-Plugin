/*
 * 单生产者 / 单消费者 无锁环形缓冲区（int16 采样）。
 * 生产者：WASAPI 采集线程；消费者：TeamSpeak 音频线程。
 */
#ifndef LA_RING_BUFFER_H
#define LA_RING_BUFFER_H

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace la {

class SpscRing {
public:
    explicit SpscRing(size_t capacityPow2)
    {
        size_t cap = 1;
        while (cap < capacityPow2)
            cap <<= 1;
        buf_.assign(cap, 0);
        mask_ = cap - 1;
    }

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    size_t capacity() const { return buf_.size(); }

    size_t available() const
    {
        size_t w = writePos_.load(std::memory_order_acquire);
        size_t r = readPos_.load(std::memory_order_acquire);
        const size_t discard = discardBefore_.load(std::memory_order_acquire);
        if (discard > r) r = discard;
        return w >= r ? w - r : 0;
    }

    /* 写入，返回实际写入的采样数（空间不足时截断） */
    size_t write(const int16_t* src, size_t count)
    {
        const size_t w = writePos_.load(std::memory_order_relaxed);
        const size_t r = readPos_.load(std::memory_order_acquire);
        size_t space = buf_.size() - (w - r);
        if (count > space)
            count = space;
        for (size_t i = 0; i < count; ++i)
            buf_[(w + i) & mask_] = src[i];
        writePos_.store(w + count, std::memory_order_release);
        return count;
    }

    /* 读取，返回实际读到的采样数 */
    size_t read(int16_t* dst, size_t count)
    {
        size_t r = readPos_.load(std::memory_order_relaxed);
        const size_t discard = discardBefore_.load(std::memory_order_acquire);
        if (discard > r) r = discard;
        const size_t w = writePos_.load(std::memory_order_acquire);
        // Recover promptly after a paused TS callback instead of playing seconds
        // of queued music. Only this consumer advances readPos_.
        if (w - r > 12000) r = w - 4800;
        size_t avail = w - r;
        if (count > avail)
            count = avail;
        for (size_t i = 0; i < count; ++i)
            dst[i] = buf_[(r + i) & mask_];
        readPos_.store(r + count, std::memory_order_release);
        return count;
    }

    void clear()
    {
        // May be called by the producer/control thread during a consumer read.
        // Never reset the SPSC cursors from another thread: that can underflow
        // space calculations and make the producer overwrite in-use samples.
        discardBefore_.store(writePos_.load(std::memory_order_acquire), std::memory_order_release);
    }

private:
    std::vector<int16_t> buf_;
    size_t mask_ = 0;
    std::atomic<size_t> writePos_{0};
    std::atomic<size_t> readPos_{0};
    std::atomic<size_t> discardBefore_{0};
};

} // namespace la

#endif /* LA_RING_BUFFER_H */

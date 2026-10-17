#pragma once
// Lock-free single-producer / single-consumer ring of interleaved stereo float
// frames. Storage is allocated once at construction; push/consume never
// allocate or block, so both sides are safe to call from real-time threads.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dbdeck {

class StereoRing {
public:
    explicit StereoRing(size_t minFrames) {
        size_t cap = 16;
        while (cap < minFrames) cap <<= 1;
        capacity_ = cap;
        mask_ = cap - 1;
        data_.assign(cap * 2, 0.0f);
    }

    StereoRing(const StereoRing&) = delete;
    StereoRing& operator=(const StereoRing&) = delete;

    size_t capacity() const { return capacity_; }

    // Consumer side: frames available to read.
    size_t readable() const {
        return static_cast<size_t>(write_.load(std::memory_order_acquire) -
                                   read_.load(std::memory_order_relaxed));
    }

    // Producer side: free frames.
    size_t writable() const {
        return capacity_ - static_cast<size_t>(write_.load(std::memory_order_relaxed) -
                                               read_.load(std::memory_order_acquire));
    }

    // Producer: copies up to n frames, returns the number written.
    size_t push(const float* frames, size_t n) {
        const size_t space = writable();
        const size_t count = std::min(space, n);
        uint64_t w = write_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < count; ++i) {
            const size_t idx = static_cast<size_t>((w + i) & mask_) * 2;
            data_[idx] = frames[i * 2];
            data_[idx + 1] = frames[i * 2 + 1];
        }
        write_.store(w + count, std::memory_order_release);
        return count;
    }

    // Consumer: pointer to the frame at offset i past the read cursor. Only
    // valid for i < readable().
    const float* frameAt(size_t i) const {
        const uint64_t r = read_.load(std::memory_order_relaxed);
        return &data_[static_cast<size_t>((r + i) & mask_) * 2];
    }

    // Consumer: advance the read cursor.
    void consume(size_t n) {
        read_.store(read_.load(std::memory_order_relaxed) + n, std::memory_order_release);
    }

    // Consumer: copies up to n frames, returns the number read.
    size_t pop(float* out, size_t n) {
        const size_t count = std::min(readable(), n);
        for (size_t i = 0; i < count; ++i) {
            const float* f = frameAt(i);
            out[i * 2] = f[0];
            out[i * 2 + 1] = f[1];
        }
        consume(count);
        return count;
    }

private:
    std::vector<float> data_;
    size_t capacity_ = 0;
    uint64_t mask_ = 0;
    alignas(64) std::atomic<uint64_t> write_{0};
    alignas(64) std::atomic<uint64_t> read_{0};
};

}  // namespace dbdeck

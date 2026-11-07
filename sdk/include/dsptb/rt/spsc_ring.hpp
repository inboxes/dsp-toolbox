#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324) // padding from alignas(64) is intentional (avoids false sharing)
#endif

namespace dsptb::rt {

// Wait-free single-producer single-consumer ring buffer.
//
// Exactly one thread may call the producer functions (tryPush, write, freeSpace) and exactly one other
// thread the consumer functions (tryPop, read, size). Capacity is rounded up to a power of two so
// indices wrap with a mask. head_/tail_ only ever increase (64-bit, so they never wrap in practice).
//
// Memory ordering: the producer publishes data with a release store of tail_, the consumer observes it
// with an acquire load, and vice versa for head_ when the consumer frees slots.
// T must be trivially copyable so bulk transfers can use memcpy.
template <class T> class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>, "SpscRing<T> needs a trivially copyable T");

public:
    // Allocates on construction (non-RT). Never allocates afterwards.
    explicit SpscRing(size_t minCapacity)
        : capacity_(roundUpPow2(minCapacity < 2 ? 2 : minCapacity)), mask_(capacity_ - 1), buf_(capacity_) {}

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    size_t capacity() const noexcept { return capacity_; }

    // --- producer side ---
    size_t freeSpace() const noexcept {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        const uint64_t head = head_.load(std::memory_order_acquire);
        return capacity_ - static_cast<size_t>(tail - head);
    }

    bool tryPush(const T& v) noexcept {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        const uint64_t head = head_.load(std::memory_order_acquire);
        if (tail - head >= capacity_)
            return false;
        buf_[static_cast<size_t>(tail & mask_)] = v;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Writes as many of the n items as fit and returns how many were written.
    size_t write(const T* src, size_t n) noexcept {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        const uint64_t head = head_.load(std::memory_order_acquire);
        const size_t space = capacity_ - static_cast<size_t>(tail - head);
        const size_t count = n < space ? n : space;
        copyIn(static_cast<size_t>(tail & mask_), src, count);
        tail_.store(tail + count, std::memory_order_release);
        return count;
    }

    // --- consumer side ---
    size_t size() const noexcept {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        const uint64_t tail = tail_.load(std::memory_order_acquire);
        return static_cast<size_t>(tail - head);
    }

    bool tryPop(T& out) noexcept {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        const uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head == tail)
            return false;
        out = buf_[static_cast<size_t>(head & mask_)];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Reads up to n items and returns how many were read.
    size_t read(T* dst, size_t n) noexcept {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        const uint64_t tail = tail_.load(std::memory_order_acquire);
        const size_t avail = static_cast<size_t>(tail - head);
        const size_t count = n < avail ? n : avail;
        copyOut(static_cast<size_t>(head & mask_), dst, count);
        head_.store(head + count, std::memory_order_release);
        return count;
    }

private:
    static size_t roundUpPow2(size_t v) noexcept {
        size_t p = 1;
        while (p < v)
            p <<= 1;
        return p;
    }

    void copyIn(size_t start, const T* src, size_t count) noexcept {
        const size_t first = count < capacity_ - start ? count : capacity_ - start;
        if (first)
            std::memcpy(&buf_[start], src, first * sizeof(T));
        if (count > first)
            std::memcpy(&buf_[0], src + first, (count - first) * sizeof(T));
    }

    void copyOut(size_t start, T* dst, size_t count) noexcept {
        const size_t first = count < capacity_ - start ? count : capacity_ - start;
        if (first)
            std::memcpy(dst, &buf_[start], first * sizeof(T));
        if (count > first)
            std::memcpy(dst + first, &buf_[0], (count - first) * sizeof(T));
    }

    const size_t capacity_;
    const size_t mask_;
    std::vector<T> buf_;
    // Separate cache lines so producer and consumer do not false-share.
    alignas(64) std::atomic<uint64_t> head_{0}; // consumer-owned
    alignas(64) std::atomic<uint64_t> tail_{0}; // producer-owned
};

} // namespace dsptb::rt

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

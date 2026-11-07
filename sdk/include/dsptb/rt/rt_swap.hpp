#pragma once

#include "spsc_ring.hpp"

#include <atomic>
#include <memory>

namespace dsptb::rt {

// Publishes an immutable object from a non-real-time thread to the real-time thread.
//
//   publisher thread:  publish(std::make_unique<T>(...))   // allocates, may block: fine
//                      collect()                            // frees objects the RT thread has retired
//   RT thread:         T* cur = rtAcquire();                // block start; never allocates or frees
//
// The RT thread takes the newest pending object with one atomic exchange. The object it replaces is
// not freed there: it is pushed into a small "graveyard" ring and freed later by the publisher in
// collect(). If the graveyard is full, the RT thread simply keeps its current object and picks the
// pending one up at a later block, so it never blocks and never frees.
// One publisher thread and one RT thread only.
template <class T> class RtSwap {
public:
    explicit RtSwap(size_t graveyardCapacity = 64) : graveyard_(graveyardCapacity) {}

    ~RtSwap() {
        delete pending_.load(std::memory_order_acquire);
        delete current_;
        T* p = nullptr;
        while (graveyard_.tryPop(p))
            delete p;
    }

    RtSwap(const RtSwap&) = delete;
    RtSwap& operator=(const RtSwap&) = delete;

    // --- publisher thread ---
    void publish(std::unique_ptr<T> next) {
        // If the RT thread never picked up the previous pending object it is still ours to free.
        delete pending_.exchange(next.release(), std::memory_order_acq_rel);
        collect();
    }

    void collect() {
        T* p = nullptr;
        while (graveyard_.tryPop(p))
            delete p;
    }

    // --- RT thread ---
    // Call once per block. Returns the object to use for this block (null until the first publish).
    T* rtAcquire() noexcept {
        if (pending_.load(std::memory_order_relaxed) != nullptr &&
            (current_ == nullptr || graveyard_.freeSpace() > 0)) {
            T* next = pending_.exchange(nullptr, std::memory_order_acq_rel);
            if (next != nullptr) {
                if (current_ != nullptr)
                    graveyard_.tryPush(current_); // room was checked above
                current_ = next;
            }
        }
        return current_;
    }

    T* rtCurrent() const noexcept { return current_; }

private:
    std::atomic<T*> pending_{nullptr};
    T* current_ = nullptr; // RT-thread-owned
    SpscRing<T*> graveyard_;
};

} // namespace dsptb::rt

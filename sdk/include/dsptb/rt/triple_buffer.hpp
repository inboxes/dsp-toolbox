#pragma once

#include <atomic>

namespace dsptb::rt {

// Lock-free "always the latest value" hand-off from one writer to one reader.
// Used to move analyzer results to the UI: the reader never waits and never sees a half-written value.
//
// Three slots rotate between roles: the writer owns `back_`, the reader owns `front_`, and `middle_`
// is the shared mailbox. The writer swaps back_ into the mailbox (setting the dirty bit); the reader
// swaps the mailbox for front_ when the dirty bit is set. Slot indices are therefore always distinct.
// T must be default-constructible and copy-assignable. Not allocation-free to construct, but never
// allocates afterwards.
template <class T> class TripleBuffer {
public:
    TripleBuffer() = default;
    explicit TripleBuffer(const T& initial) {
        for (auto& s : slots_)
            s = initial;
    }

    // --- writer thread ---
    T& writeSlot() noexcept { return slots_[back_]; }
    // Makes the current write slot the latest value. Afterwards writeSlot() is a different slot, so write
    // the full value again (or copy what you need from the previous one).
    void publish() noexcept {
        const int previous = middle_.exchange(back_ | kDirty, std::memory_order_acq_rel);
        back_ = previous & kIndexMask;
    }

    // --- reader thread ---
    // Fetches the newest published value if there is one. Returns true if the value changed.
    bool update() noexcept {
        if ((middle_.load(std::memory_order_acquire) & kDirty) == 0)
            return false;
        const int previous = middle_.exchange(front_, std::memory_order_acq_rel);
        front_ = previous & kIndexMask;
        return true;
    }
    const T& read() const noexcept { return slots_[front_]; }

private:
    static constexpr int kDirty = 4;
    static constexpr int kIndexMask = 3;

    T slots_[3]{};
    int back_ = 0;               // writer-private
    int front_ = 2;              // reader-private
    std::atomic<int> middle_{1}; // shared, bit 2 = "new data"
};

} // namespace dsptb::rt

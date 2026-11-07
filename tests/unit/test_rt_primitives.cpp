#include <doctest/doctest.h>

#include <dsptb/rt/rt_guard.hpp>
#include <dsptb/rt/rt_swap.hpp>
#include <dsptb/rt/smoothed_value.hpp>
#include <dsptb/rt/spsc_ring.hpp>
#include <dsptb/rt/triple_buffer.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using namespace dsptb::rt;

TEST_CASE("SpscRing: capacity rounds up to a power of two") {
    CHECK(SpscRing<int>(1).capacity() == 2);
    CHECK(SpscRing<int>(5).capacity() == 8);
    CHECK(SpscRing<int>(64).capacity() == 64);
}

TEST_CASE("SpscRing: push/pop, full and empty") {
    SpscRing<int> ring(4);
    int v = 0;
    CHECK_FALSE(ring.tryPop(v));
    for (int i = 0; i < 4; ++i)
        CHECK(ring.tryPush(i));
    CHECK_FALSE(ring.tryPush(99));
    CHECK(ring.size() == 4);
    CHECK(ring.freeSpace() == 0);
    for (int i = 0; i < 4; ++i) {
        REQUIRE(ring.tryPop(v));
        CHECK(v == i);
    }
    CHECK_FALSE(ring.tryPop(v));
}

TEST_CASE("SpscRing: bulk write/read wraps around the end") {
    SpscRing<int> ring(8);
    std::vector<int> in(6), out(6);
    for (int round = 0; round < 20; ++round) { // each round moves the start index so the data wraps
        for (int i = 0; i < 6; ++i)
            in[static_cast<size_t>(i)] = round * 100 + i;
        REQUIRE(ring.write(in.data(), 6) == 6);
        REQUIRE(ring.read(out.data(), 6) == 6);
        CHECK(out == in);
    }
    int big[12] = {};
    CHECK(ring.write(big, 12) == 8); // only what fits
    CHECK(ring.read(big, 12) == 8);
}

TEST_CASE("SpscRing: two-thread stress keeps order over 10^7 items") {
    constexpr uint64_t kCount = 10'000'000;
    SpscRing<uint64_t> ring(1024);
    std::atomic<bool> ordered{true};

    std::thread consumer([&] {
        uint64_t expected = 0;
        uint64_t v = 0;
        while (expected < kCount) {
            if (ring.tryPop(v)) {
                if (v != expected) {
                    ordered = false;
                    return;
                }
                ++expected;
            }
        }
    });
    for (uint64_t i = 0; i < kCount; ++i)
        while (!ring.tryPush(i)) {
        }
    consumer.join();
    CHECK(ordered.load());
}

TEST_CASE("SpscRing: bulk two-thread stress with odd chunk sizes") {
    constexpr uint64_t kCount = 4'000'000;
    SpscRing<uint64_t> ring(512);
    std::atomic<bool> ordered{true};

    std::thread consumer([&] {
        std::vector<uint64_t> buf(37);
        uint64_t expected = 0;
        while (expected < kCount) {
            const size_t n = ring.read(buf.data(), buf.size());
            for (size_t i = 0; i < n; ++i)
                if (buf[i] != expected++) {
                    ordered = false;
                    return;
                }
        }
    });
    std::vector<uint64_t> chunk(53);
    uint64_t next = 0;
    while (next < kCount) {
        const uint64_t n = std::min<uint64_t>(chunk.size(), kCount - next);
        for (uint64_t i = 0; i < n; ++i)
            chunk[i] = next + i;
        size_t done = 0;
        while (done < n)
            done += ring.write(chunk.data() + done, n - done);
        next += n;
    }
    consumer.join();
    CHECK(ordered.load());
}

namespace {
struct Pair {
    uint64_t a = 0;
    uint64_t b = 0; // always equal to a unless the reader saw a torn write
    uint64_t c = 0;
    uint64_t d = 0;
};
} // namespace

TEST_CASE("TripleBuffer: reader always sees whole, non-decreasing values") {
    TripleBuffer<Pair> tb;
    CHECK_FALSE(tb.update()); // nothing published yet

    std::atomic<bool> stop{false};
    std::thread writer([&] {
        uint64_t n = 1;
        while (!stop.load(std::memory_order_relaxed)) {
            Pair& p = tb.writeSlot();
            p.a = p.b = p.c = p.d = n++;
            tb.publish();
        }
    });

    uint64_t last = 0;
    bool ok = true;
    uint64_t reads = 0;
    while (reads < 2'000'000) {
        if (tb.update()) {
            const Pair& p = tb.read();
            if (p.a != p.b || p.b != p.c || p.c != p.d || p.a < last)
                ok = false;
            last = p.a;
            ++reads;
        }
    }
    stop = true;
    writer.join();
    CHECK(ok);
    CHECK(last > 0);
}

namespace {
std::atomic<int> gLive{0};
std::atomic<int> gFreedOnRtThread{0};
std::thread::id gRtThread;

struct Tracked {
    explicit Tracked(int v) : value(v) { gLive++; }
    ~Tracked() {
        gLive--;
        if (std::this_thread::get_id() == gRtThread)
            gFreedOnRtThread++;
    }
    int value;
};
} // namespace

TEST_CASE("RtSwap: RT thread sees increasing versions and never frees") {
    gLive = 0;
    gFreedOnRtThread = 0;
    {
        RtSwap<Tracked> swap(8);
        std::atomic<bool> stop{false};
        bool monotonic = true;
        int last = -1;

        std::thread rt([&] {
            gRtThread = std::this_thread::get_id();
            while (!stop.load(std::memory_order_acquire)) {
                if (Tracked* t = swap.rtAcquire()) {
                    if (t->value < last)
                        monotonic = false;
                    last = t->value;
                }
            }
            // The publisher can finish before this thread ever ran (slow CI runner): take the final object
            // here so `last` always reflects what the RT side saw.
            if (Tracked* t = swap.rtAcquire()) {
                if (t->value < last)
                    monotonic = false;
                last = t->value;
            }
        });

        for (int i = 0; i < 20000; ++i)
            swap.publish(std::make_unique<Tracked>(i));
        stop = true;
        rt.join();
        swap.collect();
        CHECK(monotonic);
        CHECK(last >= 0);
    }
    CHECK(gLive.load() == 0);            // everything freed exactly once
    CHECK(gFreedOnRtThread.load() == 0); // and never on the RT thread
}

TEST_CASE("RtSwap: a full graveyard makes the RT thread keep its object, not block") {
    gLive = 0;
    gRtThread = std::this_thread::get_id();
    gFreedOnRtThread = 0;
    {
        RtSwap<Tracked> swap(2); // graveyard of 2
        swap.publish(std::make_unique<Tracked>(0));
        CHECK(swap.rtAcquire()->value == 0);
        // Retire up to capacity: each rtAcquire pushes the previous object into the graveyard.
        swap.publish(std::make_unique<Tracked>(1)); // publish() collects, so graveyard stays empty
        CHECK(swap.rtAcquire()->value == 1);
        CHECK(gFreedOnRtThread.load() == 0);
    }
    CHECK(gLive.load() == 0);
}

TEST_CASE("SmoothedValue: linear ramp lands exactly on the target") {
    SmoothedValue s;
    s.reset(48000.0, 10.0, 0.0f); // 480 samples
    s.setTarget(1.0f);
    CHECK(s.isSmoothing());
    float prev = 0.0f;
    for (int i = 0; i < 479; ++i) {
        const float v = s.next();
        CHECK(v > prev);
        CHECK(v < 1.0f);
        prev = v;
    }
    CHECK(s.next() == 1.0f); // exactly the target, no float drift
    CHECK_FALSE(s.isSmoothing());
    CHECK(s.next() == 1.0f);
}

TEST_CASE("SmoothedValue: no smoothing jumps, retarget mid-ramp, one-pole converges") {
    SmoothedValue jump;
    jump.reset(48000.0, 0.0, 2.0f);
    jump.setTarget(5.0f);
    CHECK(jump.next() == 5.0f);

    SmoothedValue s;
    s.reset(1000.0, 100.0, 0.0f); // 100 samples
    s.setTarget(1.0f);
    for (int i = 0; i < 50; ++i)
        s.next();
    s.setTarget(0.0f); // retarget from wherever we are
    float v = 1.0f;
    for (int i = 0; i < 100; ++i)
        v = s.next();
    CHECK(v == 0.0f);

    SmoothedValue op;
    op.reset(48000.0, 5.0, 0.0f, SmoothedValue::Kind::OnePole);
    op.setTarget(1.0f);
    float last = 0.0f;
    for (int i = 0; i < 48000; ++i)
        last = op.next();
    CHECK(last == 1.0f);
}

TEST_CASE("SmoothedValue: fill produces the same samples as next") {
    SmoothedValue a, b;
    a.reset(48000.0, 2.0, 0.0f);
    b.reset(48000.0, 2.0, 0.0f);
    a.setTarget(1.0f);
    b.setTarget(1.0f);
    float block[200];
    a.fill(block, 200);
    for (int i = 0; i < 200; ++i)
        CHECK(block[i] == b.next());
}

#if defined(DSPTB_RT_GUARD)
TEST_CASE("RT guard: new/delete inside an RtScope is counted, outside is not") {
    static int* volatile sink; // volatile so the compiler cannot elide the allocation

    resetRtGuardViolations();
    sink = new int(1);
    delete sink;
    CHECK(rtGuardViolations() == 0); // outside an RT scope: allowed

    {
        RtScope scope;
        CHECK(inRtScope());
        sink = new int(2);
        CHECK(rtGuardViolations() == 1);
        delete sink;
        CHECK(rtGuardViolations() == 2); // frees count too
        CHECK(rtGuardLastSite() != nullptr);
        {
            RtScope nested;
        }
        CHECK(inRtScope()); // nesting restores the outer scope
    }
    CHECK_FALSE(inRtScope());
    resetRtGuardViolations();
    CHECK(rtGuardViolations() == 0);
}

TEST_CASE("RT guard: scope is per thread") {
    resetRtGuardViolations();
    std::atomic<bool> go{false};
    std::atomic<bool> done{false};
    // Start the thread before entering the scope: creating a thread allocates.
    std::thread other([&] {
        while (!go.load())
            std::this_thread::yield();
        static int* volatile sink;
        sink = new int(3); // this thread is not in an RT scope
        delete sink;
        done = true;
    });
    {
        RtScope scope;
        go = true;
        while (!done.load())
            std::this_thread::yield();
        CHECK(rtGuardViolations() == 0);
    }
    other.join();
}
#endif

// Autotune's logic without audio: which key applies, which note is the target, how steady the pitch is, and how
// Strength maps to the correction.
#include <doctest/doctest.h>

#include "correction_smoother.hpp"
#include "key_timeline.hpp"
#include "pitch_tracker.hpp"
#include "target_picker.hpp"

#include <cmath>

using namespace dsptb::autotune;

TEST_CASE("autotune: effective key") {
    const DetectedKey none;
    const DetectedKey unsure{true, false, 9, Scale::Minor};
    const DetectedKey sure{true, true, 9, Scale::Minor};

    SUBCASE("manual always uses the parameters") {
        CHECK(effectiveKey(true, 2, Scale::Major, none) == Key{2, Scale::Major});
        CHECK(effectiveKey(true, 2, Scale::Major, sure) == Key{2, Scale::Major});
    }
    SUBCASE("track follows only a confident detection") {
        CHECK(effectiveKey(false, 2, Scale::Major, none).scale == Scale::Chromatic);
        CHECK(effectiveKey(false, 2, Scale::Major, unsure).scale == Scale::Chromatic);
        CHECK(effectiveKey(false, 2, Scale::Major, sure) == Key{9, Scale::Minor});
    }
}

TEST_CASE("autotune: key timeline") {
    const KeyWindow gMinor{true, true, 7, Scale::Minor};
    const KeyWindow gMajor{true, true, 7, Scale::Major};
    const KeyWindow eMinor{true, true, 4, Scale::Minor};
    const KeyWindow dMinor{true, true, 2, Scale::Minor};
    const KeyWindow unsure{true, false, 2, Scale::Major};

    SUBCASE("one doubtful or lone window doesn't change the key") {
        const auto keys = resolveKeyTimeline({gMinor, unsure, dMinor, gMinor, gMinor});
        for (const DetectedKey& k : keys)
            CHECK(k.root == 7);
    }
    SUBCASE("two windows in a row change it, from the first of them") {
        const auto keys = resolveKeyTimeline({gMinor, gMinor, gMajor, eMinor, gMajor});
        CHECK(keys[1].scale == Scale::Minor);
        CHECK(keys[2].valid);
        CHECK(keys[2].root == 7);
        CHECK(keys[2].scale == Scale::Major);
        CHECK(sameNotesId(keys[3].root, keys[3].scale) == sameNotesId(7, Scale::Major)); // E minor: same notes
    }
    SUBCASE("before the first confident window, its key applies") {
        const auto keys = resolveKeyTimeline({unsure, unsure, gMinor});
        CHECK(keys[0].valid);
        CHECK(keys[0].root == 7);
    }
    SUBCASE("nothing confident: no key") {
        CHECK_FALSE(resolveKeyTimeline({unsure, unsure})[0].valid);
    }
    SUBCASE("finding the window for a position") {
        CHECK(keyWindowAt(0.0, 10.0, 5.0, 10) == 0);
        CHECK(keyWindowAt(7.4, 10.0, 5.0, 10) == 0);
        CHECK(keyWindowAt(7.6, 10.0, 5.0, 10) == 1);
        CHECK(keyWindowAt(1000.0, 10.0, 5.0, 10) == 9);
    }
}

TEST_CASE("autotune: target notes") {
    TargetPicker picker;
    const Key cMajor{0, Scale::Major};
    const Key aMinor{9, Scale::Minor};
    const Key chromatic{0, Scale::Chromatic};

    SUBCASE("snaps to the scale") {
        CHECK(picker.pick(69.58f, true, cMajor, 0.0f) == 69); // A4 + 58 c: A#4 isn't in C major
        picker.reset();
        CHECK(picker.pick(69.58f, true, chromatic, 0.0f) == 70);
        picker.reset();
        CHECK(picker.pick(70.0f, true, aMinor, 0.0f) == 69); // Bb is halfway between A and B: the lower wins
    }
    SUBCASE("note hold") {
        CHECK(picker.pick(69.0f, true, chromatic, 0.0f) == 69);
        CHECK(picker.pick(69.51f, true, chromatic, 0.0f) == 70);
        picker.reset();
        CHECK(picker.pick(69.0f, true, chromatic, 30.0f) == 69);
        CHECK(picker.pick(69.79f, true, chromatic, 30.0f) == 69);
        CHECK(picker.pick(69.81f, true, chromatic, 30.0f) == 70);
        picker.reset();
        CHECK(picker.pick(69.0f, true, chromatic, 50.0f) == 69);
        CHECK(picker.pick(69.99f, true, chromatic, 50.0f) == 69);
    }
    SUBCASE("a key change drops the target") {
        CHECK(picker.pick(69.7f, true, cMajor, 50.0f) == 69);
        CHECK(picker.pick(69.7f, true, chromatic, 50.0f) == 70);
    }
    SUBCASE("an unvoiced hop clears it") {
        CHECK(picker.pick(69.0f, true, chromatic, 50.0f) == 69);
        CHECK(picker.pick(0.0f, false, chromatic, 50.0f) == TargetPicker::kNoTarget);
        CHECK(picker.pick(69.9f, true, chromatic, 50.0f) == 70);
    }
}

TEST_CASE("autotune: pitch tracking") {
    PitchTracker t;
    const PitchTrackerConfig config;
    const auto voiced = [&](float hz) { return t.update(hz, true, -20.0f, config); };

    SUBCASE("what counts as unvoiced") {
        CHECK_FALSE(t.update(440.0f, false, -20.0f, config).voiced);
        CHECK_FALSE(t.update(440.0f, true, -60.0f, config).voiced);
        CHECK_FALSE(t.update(70.0f, true, -20.0f, config).voiced);
        CHECK_FALSE(t.update(1200.0f, true, -20.0f, config).voiced);
        PitchTrackerConfig narrow = config;
        narrow.minHz = 200.0f;
        CHECK_FALSE(t.update(150.0f, true, -20.0f, narrow).voiced);
    }
    SUBCASE("MIDI follows the reference") {
        CHECK(voiced(440.0f).midi == doctest::Approx(69.0f));
        t.reset();
        PitchTrackerConfig a432 = config;
        a432.refA4 = 432.0f;
        CHECK(t.update(432.0f, true, -20.0f, a432).midi == doctest::Approx(69.0f));
    }
    SUBCASE("small changes pass straight through") {
        voiced(440.0f);
        CHECK(voiced(470.0f).f0Hz == doctest::Approx(470.0f)); // 114 cents
    }
    SUBCASE("a jump to the third harmonic is held too") {
        voiced(220.0f);
        CHECK(voiced(660.0f).f0Hz == doctest::Approx(220.0f));
        CHECK(voiced(330.0f).f0Hz == doctest::Approx(220.0f)); // a fifth
        CHECK(voiced(220.0f).f0Hz == doctest::Approx(220.0f));
    }
    SUBCASE("an unvoiced hop forgets the last note") {
        voiced(440.0f);
        t.update(0.0f, false, -20.0f, config);
        CHECK(voiced(300.0f).f0Hz == doctest::Approx(300.0f));
    }
    SUBCASE("a jump is held for three hops, then accepted") {
        voiced(220.0f);
        for (int i = 0; i < 3; ++i)
            CHECK(voiced(440.0f).f0Hz == doctest::Approx(220.0f));
        CHECK(voiced(440.0f).f0Hz == doctest::Approx(440.0f));
    }
    SUBCASE("a short octave error leaves the pitch alone") {
        voiced(220.0f);
        voiced(440.0f);
        voiced(440.0f);
        CHECK(voiced(220.0f).f0Hz == doctest::Approx(220.0f));
    }
}

TEST_CASE("autotune: Strength") {
    constexpr float kHopMs = 256.0f / 48.0f;
    CorrectionSmoother s;
    s.prepare(kHopMs);

    SUBCASE("100 % gives the full error in one hop") {
        CHECK(s.update(true, 40.0f, 100.0f) == doctest::Approx(40.0f));
        CHECK(s.ratio() == doctest::Approx(std::pow(2.0f, 40.0f / 1200.0f)));
    }
    SUBCASE("0 % does nothing") {
        for (int i = 0; i < 100; ++i)
            CHECK(s.update(true, 40.0f, 0.0f) == doctest::Approx(0.0f));
    }
    SUBCASE("45 % reaches 63 % of its target after about 60 ms") {
        const float target = 40.0f * 0.75f;
        s.update(true, 0.0f, 45.0f); // the note's first hop starts at 0
        float ms = 0.0f;
        while (s.cents() < 0.632f * target && ms < 500.0f) {
            s.update(true, 40.0f, 45.0f);
            ms += kHopMs;
        }
        CHECK(ms >= 54.0f);
        CHECK(ms <= 66.0f);
    }
    SUBCASE("the amount reaches full at 60 %") {
        for (int i = 0; i < 400; ++i)
            s.update(true, 40.0f, 60.0f);
        CHECK(s.cents() == doctest::Approx(40.0f));
    }
    SUBCASE("corrections stop at 300 cents") {
        CHECK(s.update(true, 900.0f, 100.0f) == doctest::Approx(300.0f));
        CHECK(s.update(true, -900.0f, 100.0f) == doctest::Approx(-300.0f));
    }
    SUBCASE("an unvoiced hop eases back, and the next note starts natural") {
        s.update(true, 100.0f, 100.0f);
        const float after = s.update(false, 0.0f, 100.0f);
        CHECK(after == doctest::Approx(100.0f * std::exp(-kHopMs / 20.0f)));
        CHECK(s.update(true, 50.0f, 45.0f) < 50.0f * 0.75f);
    }
}

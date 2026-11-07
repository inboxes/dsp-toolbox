// Tuning maths, note names and fftSizeForRate.

#include <doctest/doctest.h>

#include <dsptb/dsp_common/freq_map.hpp>
#include <dsptb/dsp_common/stft.hpp>
#include <dsptb/dsp_common/tuning.hpp>

#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

using namespace dsptb::dsp;

TEST_CASE("hz, MIDI and cents conversions") {
    CHECK(hzToMidi(440.0) == doctest::Approx(69.0));
    CHECK(hzToMidi(261.6256) == doctest::Approx(60.0).epsilon(1e-6));
    CHECK(hzToMidi(432.0, Tuning{432.0}) == doctest::Approx(69.0));
    CHECK(std::isinf(hzToMidi(0.0)));
    CHECK(std::isinf(hzToMidi(-1.0)));
    CHECK(std::isinf(hzToMidi(std::numeric_limits<double>::quiet_NaN())));
    for (double hz = 20.0; hz <= 20000.0; hz *= 1.07)
        CHECK(std::abs(midiToHz(hzToMidi(hz)) / hz - 1.0) < 1e-9);
    CHECK(centsBetween(880.0, 440.0) == doctest::Approx(1200.0));
    CHECK(centsBetween(0.0, 440.0) == 0.0);
    CHECK(centsBetween(440.0, -1.0) == 0.0);
}

TEST_CASE("nearestNote") {
    const NoteInfo a = nearestNote(450.3);
    CHECK(a.valid);
    CHECK(a.midi == 69);
    CHECK(a.cents == doctest::Approx(40.0).epsilon(0.0025));

    const NoteInfo high = nearestNote(452.89);
    CHECK(high.midi == 69);
    CHECK(std::abs(high.cents - 49.99f) <= 0.05f);

    const NoteInfo tie = nearestNote(midiToHz(69.5));
    CHECK(tie.midi == 70);
    CHECK(tie.pitchClass == 10);
    CHECK(tie.cents == doctest::Approx(-50.0));

    CHECK_FALSE(nearestNote(-1.0).valid);
    CHECK_FALSE(nearestNote(0.0).valid);
    CHECK_FALSE(nearestNote(std::numeric_limits<double>::infinity()).valid);
}

TEST_CASE("pitch class and octave for edge MIDI numbers") {
    struct Row {
        int midi, pitchClass, octave;
    };
    const Row rows[] = {{-1, 11, -2}, {0, 0, -1}, {11, 11, -1}, {12, 0, 0}, {127, 7, 9}};
    for (const Row& r : rows) {
        const NoteInfo n = nearestNote(midiToHz(static_cast<double>(r.midi)));
        CHECK(n.midi == r.midi);
        CHECK(n.pitchClass == r.pitchClass);
        CHECK(n.octave == r.octave);
    }
    CHECK(std::strcmp(pitchClassName(0), "C") == 0);
    CHECK(std::strcmp(pitchClassName(1), "C#") == 0);
    CHECK(std::strcmp(pitchClassName(11), "B") == 0);
    CHECK(std::strcmp(pitchClassName(12), "?") == 0);
    CHECK(std::strcmp(pitchClassName(-1), "?") == 0);
}

TEST_CASE("noteName with a tuning, and no overrun") {
    char text[32];
    noteName(432.0, Tuning{432.0}, text, sizeof(text));
    CHECK(std::strcmp(text, "A4 +0c") == 0);
    noteName(-5.0, Tuning{}, text, sizeof(text));
    CHECK(std::strcmp(text, "--") == 0);

    // The old overload prints exactly what the tuned one prints at 440.
    char oldName[32];
    for (double hz = 30.0; hz < 8000.0; hz *= 1.13) {
        noteName(hz, oldName, sizeof(oldName));
        noteName(hz, Tuning{}, text, sizeof(text));
        CHECK(std::strcmp(oldName, text) == 0);
    }

    for (std::size_t cap = 1; cap <= 8; ++cap) {
        char guarded[16];
        std::memset(guarded, 'x', sizeof(guarded));
        noteName(261.63, Tuning{}, guarded, cap);
        CHECK(guarded[cap] == 'x');
        CHECK(std::strlen(guarded) < cap);
    }
}

TEST_CASE("Tuning range") {
    CHECK_THROWS_AS(Tuning{414.9}, std::invalid_argument);
    CHECK_THROWS_AS(Tuning{466.1}, std::invalid_argument);
    CHECK_NOTHROW(Tuning{415.0});
    CHECK_NOTHROW(Tuning{466.0});
    CHECK(Tuning::fromOffsetCents(1200.0).a4Hz == doctest::Approx(Tuning::kMaxA4Hz));
    CHECK(Tuning::fromOffsetCents(-1200.0).a4Hz == doctest::Approx(Tuning::kMinA4Hz));
    CHECK(Tuning::fromOffsetCents(0.0).a4Hz == doctest::Approx(440.0));
    CHECK(Tuning{452.0}.offsetCents() == doctest::Approx(1200.0 * std::log2(452.0 / 440.0)));
}

TEST_CASE("fftSizeForRate") {
    CHECK(fftSizeForRate(44100.0, 8192) == 8192);
    CHECK(fftSizeForRate(48000.0, 8192) == 8192);
    CHECK(fftSizeForRate(88200.0, 8192) == 16384);
    CHECK(fftSizeForRate(96000.0, 8192) == 16384);
    CHECK(fftSizeForRate(192000.0, 8192) == 32768);
    CHECK(fftSizeForRate(22050.0, 8192) == 4096);
    CHECK(fftSizeForRate(8000.0, 512) == 256);
    CHECK(fftSizeForRate(48000.0, 128) == 256);
}

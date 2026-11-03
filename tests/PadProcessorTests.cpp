// SPDX-License-Identifier: GPL-3.0-or-later

#include "PluginProcessor.h"
#include "ProcessorTestSupport.h"
#include "engine/AllocationCounter.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

namespace
{
struct TestPlayHead final : juce::AudioPlayHead
{
    juce::Optional<PositionInfo> getPosition () const override { return info; }
    PositionInfo info;
};

void playFromBarOne (TestPlayHead& head)
{
    head.info.setIsPlaying (true);
    head.info.setPpqPosition (0.0);
    head.info.setPpqPositionOfLastBarStart (0.0);
    head.info.setBarCount (0);
    head.info.setBpm (120.0);
    head.info.setTimeSignature (juce::AudioPlayHead::TimeSignature {4, 4});
}

void set (LivecutAudioProcessor& processor, const char* id, float value)
{
    auto* parameter = processor.getParameterState ().getParameter (id);
    REQUIRE (parameter != nullptr);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

float signal (int frame, int channel)
{
    return std::sin (.013f * static_cast<float> (frame + 37 * channel));
}

struct PadSession
{
    TestPlayHead head;
    LivecutAudioProcessor processor;
    juce::MidiBuffer midi;
    int frame = 0;
    std::vector<float> output, input;
    double rate = 48000.0; // in 4/4
    double bpm = 120.0;

    explicit PadSession (bool spectral = false, double sampleRate = 48000.0, double tempo = 120.0)
        : rate (sampleRate), bpm (tempo)
    {
        if (spectral)
        {
            set (processor, livecut::ids::resonanceSpectralEnabled, 1);
            processor.setNonRealtime (true);
        }
        playFromBarOne (head);
        head.info.setBpm (bpm);
        processor.setPlayHead (&head);
        processor.prepareToPlay (rate, 4096);
    }

    void render (int count, int bufferSize = 512)
    {
        while (count > 0)
        {
            const int size = std::min (count, bufferSize);
            const auto beat = rate * 60.0 / bpm;
            const auto bars = std::floor (frame / beat / 4.0);
            head.info.setPpqPosition (frame / beat);
            head.info.setPpqPositionOfLastBarStart (bars * 4.0);
            head.info.setBarCount (static_cast<int> (bars));
            juce::AudioBuffer<float> buffer (2, size);
            for (int i = 0; i < size; ++i)
                for (int channel = 0; channel < 2; ++channel)
                {
                    const auto value = signal (frame + i, channel);
                    buffer.setSample (channel, i, value);
                    input.push_back (value);
                }
            processor.processBlock (buffer, midi);
            for (int i = 0; i < size; ++i)
                for (int channel = 0; channel < 2; ++channel)
                    output.push_back (buffer.getSample (channel, i));
            frame += size;
            count -= size;
        }
    }

    bool dryFrom (int sample = 0) const
    {
        return dryBetween (sample, static_cast<int> (input.size () / 2));
    }

    // Bitwise, over the samples [from, to).
    bool dryBetween (int from, int to) const
    {
        return sameBetween (output, input, from, to);
    }

    static bool sameBetween (const std::vector<float>& a, const std::vector<float>& b, int from, int to)
    {
        const auto offset = static_cast<std::size_t> (from) * 2;
        return std::memcmp (a.data () + offset, b.data () + offset, static_cast<std::size_t> (to - from) * 2 * sizeof (float)) == 0;
    }
};
} // namespace

TEST_CASE ("An untouched pad outputs the input exactly", "[pad][processor]")
{
    PadSession session;
    session.render (192000);
    CHECK (session.dryFrom ());
    CHECK_FALSE (session.processor.getCutter ().isCutting ());
}

TEST_CASE ("Touching engages on the next unit boundary", "[pad][processor]")
{
    PadSession session;
    session.render (5000);
    set (session.processor, livecut::ids::padTouch, 1);
    session.render (999);
    CHECK_FALSE (session.processor.getCutter ().isCutting ());
    session.render (2);
    CHECK (session.processor.getCutter ().isCutting ());
}

TEST_CASE ("Touching on a unit boundary starts cutting on that sample", "[pad][processor]")
{
    PadSession session;
    set (session.processor, livecut::ids::padTouch, 1);
    session.render (1);
    CHECK (session.processor.getCutter ().isCutting ());
}

TEST_CASE ("Releasing fades out on the next beat", "[pad][processor]")
{
    PadSession session;
    set (session.processor, livecut::ids::padTouch, 1);
    session.render (31200); // beat 1.3
    set (session.processor, livecut::ids::padTouch, 0);
    session.render (48000 + 238 - session.frame);
    CHECK (session.processor.getCutter ().isCutting ());
    session.render (1);
    CHECK (session.processor.getCutter ().isCutting ());
    session.render (1);
    CHECK_FALSE (session.processor.getCutter ().isCutting ());
    session.render (12000);
    CHECK (session.dryFrom (48000 + 239));
}

TEST_CASE ("With Bar quantize, a release fades from the next bar line", "[pad][processor]")
{
    PadSession session;
    set (session.processor, livecut::ids::padQuantize, 1); // Bar
    set (session.processor, livecut::ids::padTouch, 1);
    session.render (31200); // beat 1.3
    set (session.processor, livecut::ids::padTouch, 0);
    session.render (96000 + 238 - session.frame);
    CHECK (session.processor.getCutter ().isCutting ());
    session.render (1);
    CHECK (session.processor.getCutter ().isCutting ());
    session.render (1);
    CHECK_FALSE (session.processor.getCutter ().isCutting ());
    session.render (12000);
    CHECK_FALSE (session.dryBetween (48000 + 240, 96000)); // the beat passes without a release
    CHECK (session.dryFrom (96000 + 239));
}

TEST_CASE ("Hold survives a loop wrap", "[pad][processor]")
{
    PadSession session;
    set (session.processor, livecut::ids::padHold, 1);
    set (session.processor, livecut::ids::padTouch, 1);
    session.render (2400);
    set (session.processor, livecut::ids::padTouch, 0);
    session.render (36000 - session.frame); // beat 2.5, latched
    REQUIRE (session.processor.getCutter ().isCutting ());
    REQUIRE (session.processor.getDisplayState ().padEngaged.load ());
    session.frame = 0; // the host loops back to bar 1
    session.render (1);
    CHECK (session.processor.getDisplayState ().padEngaged.load ());
    CHECK (session.processor.getCutter ().isCutting ()); // bar 1 starts on a unit
    session.render (24000);
    CHECK (session.processor.getCutter ().isCutting ());
    CHECK (session.processor.getDisplayState ().status.load () == livecut::DisplayState::Status::cutting);
}

// Going idle resets the effect rack; a latched Hold never goes idle, so nothing resets mid-sound.
TEST_CASE ("A latched Hold keeps the rack running", "[pad][processor]")
{
    constexpr int end = 72000; // well past the beat-1 release and its fade
    PadSession held, touched, released;
    set (held.processor, livecut::ids::padHold, 1);
    for (auto* session : { &held, &touched, &released })
    {
        set (session->processor, livecut::ids::padTouch, 1);
        session->render (2400);
        if (session != &touched)
            set (session->processor, livecut::ids::padTouch, 0);
    }

    // The rack counts every cut since its last reset.
    auto cuts = [] (const PadSession& session) { return session.processor.getCutter ().routeDiagnostics ().cutsStarted; };
    auto counted = cuts (held);
    REQUIRE (counted > 0);
    bool runningThroughout = true, neverReset = true;
    while (held.frame < end)
    {
        held.render (1000);
        runningThroughout &= held.processor.getCutter ().isCutting ();
        neverReset &= cuts (held) >= counted;
        counted = cuts (held);
    }
    touched.render (end - touched.frame);
    released.render (end - released.frame);

    CHECK (runningThroughout);
    CHECK (neverReset);
    CHECK (counted == cuts (touched));
    CHECK (PadSession::sameBetween (held.output, touched.output, 0, end));
    // Without the latch, the pad went idle and the rack forgot its cuts.
    CHECK_FALSE (released.processor.getCutter ().isCutting ());
    CHECK (cuts (released) == 0);
}

TEST_CASE ("Switching from Classic to Pad while cutting releases on the next beat", "[pad][processor]")
{
    constexpr int switched = 31200, beat = 48000, fade = 240;
    PadSession released, held;
    for (auto* session : { &released, &held })
    {
        useClassicSurface (session->processor);
        session->render (switched); // beat 1.3, cutting on the Classic pages
        REQUIRE (session->processor.getCutter ().isCutting ());
        set (session->processor, livecut::ids::surface, 0); // Pad
        if (session == &held)
            set (session->processor, livecut::ids::padTouch, 1); // the same cuts, heard in full
        session->render (beat + fade + 12000 - session->frame);
    }
    CHECK (held.processor.getCutter ().isCutting ());
    CHECK_FALSE (released.processor.getCutter ().isCutting ());

    // The cuts carry on to the beat, exactly as under a held touch.
    CHECK (PadSession::sameBetween (released.output, held.output, switched, beat));
    CHECK_FALSE (released.dryBetween (switched, beat));

    // Then the 5 ms fade back to the input.
    int mismatches = 0;
    for (int i = 0; i < fade; ++i)
    {
        const auto gain = 1.f - static_cast<float> (i + 1) / static_cast<float> (fade);
        for (int channel = 0; channel < 2; ++channel)
        {
            const auto k = static_cast<std::size_t> (beat + i) * 2 + static_cast<std::size_t> (channel);
            const auto dry = released.input[k];
            if (std::abs (released.output[k] - (dry + gain * (held.output[k] - dry))) > 1.0e-6f)
                ++mismatches;
        }
    }
    CHECK (mismatches == 0);
    CHECK (released.dryFrom (beat + fade - 1));
}

TEST_CASE ("A program switch restarts on the beat sample", "[pad][processor]")
{
    PadSession session;
    set (session.processor, livecut::ids::padTouch, 1);
    session.render (9600);
    set (session.processor, livecut::ids::padProgram, 1); // Bounce
    session.render (24000 - session.frame);
    CHECK (session.processor.getDisplayState ().padActiveProgram.load () == 0);
    CHECK (session.processor.getDisplayState ().padPendingProgram.load () == 1);
    CHECK (session.processor.getDisplayState ().padEngaged.load ());
    CHECK (session.processor.getCutter ().activeProcedure () == livecut::engine::Procedure::cutProc11);
    session.render (1);
    CHECK (session.processor.getCutter ().activeProcedure () == livecut::engine::Procedure::bounce);
    CHECK (session.processor.getDisplayState ().padActiveProgram.load () == 1);
    CHECK (session.processor.getDisplayState ().padPendingProgram.load () == -1);
}

TEST_CASE ("With Spectral configured, touching engages and releasing returns to idle", "[pad][processor]")
{
    PadSession session (true);
    session.render (5000);
    CHECK_FALSE (session.processor.getCutter ().isCutting ());
    set (session.processor, livecut::ids::padTouch, 1);
    session.render (12000);
    CHECK (session.processor.getCutter ().isCutting ());
    set (session.processor, livecut::ids::padTouch, 0);
    session.render (24000);
    CHECK_FALSE (session.processor.getCutter ().isCutting ());
    CHECK (session.processor.getDisplayState ().spectralFault.load () == livecut::spectral::Fault::none);
}

TEST_CASE ("Legacy Audio Hold doesn't hold on the Pad surface", "[pad][processor]")
{
    const bool spectral = GENERATE (false, true);
    PadSession session (spectral);
    set (session.processor, livecut::ids::hold, 1);
    session.render (1000);
    CHECK (session.processor.getDisplayState ().hold.load () == livecut::engine::WetHold::State::live);
    set (session.processor, livecut::ids::padTouch, 1);
    session.render (192000);
    CHECK (session.processor.getDisplayState ().hold.load () == livecut::engine::WetHold::State::live);
    set (session.processor, livecut::ids::padTouch, 0);
    session.render (96000);
    CHECK (session.processor.getDisplayState ().hold.load () == livecut::engine::WetHold::State::live);
}

TEST_CASE ("Output is identical for every host buffer size", "[pad][processor]")
{
    const auto program = GENERATE (0, 3); // Cut, Suction
    CAPTURE (program);
    const auto render = [program] (int size)
    {
        PadSession session;
        set (session.processor, livecut::ids::padProgram, static_cast<float> (program));
        set (session.processor, livecut::ids::padY, .75f);
        session.render (5000, size);
        set (session.processor, livecut::ids::padTouch, 1);
        session.render (26200, size);
        set (session.processor, livecut::ids::padTouch, 0);
        session.render (28800, size);
        CHECK_FALSE (session.dryFrom ()); // The touched timeline must actually change the audio.
        return session.output;
    };
    const auto expected = render (1);
    for (const int size : {37, 512, 4096})
    {
        const auto actual = render (size);
        CHECK (actual.size () == expected.size ());
        CHECK (std::memcmp (actual.data (), expected.data (), expected.size () * sizeof (float)) == 0);
    }
}

TEST_CASE ("Programs not yet built stay dry and say so", "[pad][processor]")
{
    PadSession session;
    set (session.processor, livecut::ids::padProgram, 6); // Loops
    set (session.processor, livecut::ids::padTouch, 1);
    session.render (96000);
    CHECK (session.dryFrom ());
    CHECK (session.processor.getDisplayState ().padUnavailable.load ());
}

TEST_CASE ("Suction engages, releases and switches without a fault", "[pad][processor]")
{
    constexpr int beat = 24000, fade = 240;
    PadSession session;
    const auto& cutter = session.processor.getCutter ();
    const auto& display = session.processor.getDisplayState ();
    set (session.processor, livecut::ids::padProgram, 3); // Suction
    set (session.processor, livecut::ids::padY, .75f);
    set (session.processor, livecut::ids::padTouch, 1);
    session.render (beat);
    REQUIRE (cutter.isCutting ());
    CHECK (display.padActiveProgram.load () == 3);
    CHECK_FALSE (display.padUnavailable.load ());
    CHECK (cutter.activeProcedure () == livecut::engine::Procedure::cutProc11);
    CHECK (cutter.activeUnitsPerBar () == 16);
    CHECK (cutter.impact ().diagnostics ().filterActive);
    CHECK (cutter.impact ().diagnostics ().bandLowHz > 30.0); // squeezed
    CHECK (cutter.impact ().diagnostics ().bandHighHz < 18000.0);
    CHECK_FALSE (session.dryFrom ());

    // Moving the pad reshapes the band without restarting the cuts.
    set (session.processor, livecut::ids::padX, 1);
    set (session.processor, livecut::ids::padY, 0);
    session.render (beat);
    CHECK (cutter.isCutting ());

    // Switching to Cut lands on the beat, and the band switches off.
    set (session.processor, livecut::ids::padProgram, 0);
    session.render (3 * beat - session.frame + 1);
    CHECK (display.padActiveProgram.load () == 0);
    CHECK (cutter.isCutting ());
    session.render (beat);
    CHECK_FALSE (cutter.impact ().diagnostics ().filterActive);

    // And back to Suction.
    set (session.processor, livecut::ids::padProgram, 3);
    session.render (5 * beat - session.frame + 1);
    CHECK (display.padActiveProgram.load () == 3);
    session.render (beat / 2);
    CHECK (cutter.impact ().diagnostics ().filterActive);

    // Releasing fades out on the next beat and returns to the input exactly.
    set (session.processor, livecut::ids::padTouch, 0);
    session.render (6 * beat + fade + 12000 - session.frame);
    CHECK_FALSE (cutter.isCutting ());
    CHECK (session.dryFrom (6 * beat + fade - 1));
    CHECK (cutter.error () == livecut::engine::Cutter::Error::none);
}

// The band keeps its shape while its wet ramps out over 2 ms at the first Cut
// cut, so the switch adds no click of its own. The bar is 3x the larger
// reference, not 1x: the restart is a hard cut splice, and it rings through
// the fading band. The click this guards against, the band switching type
// before its wet ramped out, measured about 80x the reference.
TEST_CASE ("Switching Suction to Cut while engaged doesn't click", "[pad][processor]")
{
    constexpr int switched = 30000, beat = 48000, window = 2400;
    // The largest step around the beat, playing `from` and then `to` from beat 1.25.
    const auto largestStep = [] (int from, int to)
    {
        PadSession session;
        set (session.processor, livecut::ids::padProgram, static_cast<float> (from));
        set (session.processor, livecut::ids::padY, 1); // fully squeezed, the most resonant band
        set (session.processor, livecut::ids::padTouch, 1);
        session.render (switched);
        REQUIRE (session.processor.getCutter ().isCutting ());
        set (session.processor, livecut::ids::padProgram, static_cast<float> (to));
        session.render (beat + window - session.frame);
        REQUIRE (session.processor.getDisplayState ().padActiveProgram.load () == to);

        float largest = 0.f;
        for (int k = 2 * (beat - window); k < 2 * (beat + window); ++k)
            largest = std::max (largest, std::abs (session.output[static_cast<std::size_t> (k)]
                                                   - session.output[static_cast<std::size_t> (k - 2)]));
        return largest;
    };
    const auto suctionToCut = largestStep (3, 0);
    const auto suctionThroughout = largestStep (3, 3);
    const auto cutThroughout = largestStep (0, 0);
    CAPTURE (suctionToCut, suctionThroughout, cutThroughout);
    CHECK (suctionToCut <= 3.f * std::max (suctionThroughout, cutThroughout));
}

// A Pad to Classic switch with Classic's Impact filter on: Suction's band keeps
// its shape, makeup included, while its wet ramps out at Classic's next cut,
// and Classic's low-pass comes in at the cut after. Adopted at once, the band
// stepped 1.07 at that first cut, against 0.16 for Suction throughout. The
// window ends before the second cut: that is a splice to silence with Classic's
// filter off too, and the engine tests cover the low-pass coming in. (The
// other way, the pad turns Classic's filter off at the switch, and Suction's
// band starts at the beat restart, after it has faded.)
TEST_CASE ("Switching Suction to Classic's filter doesn't click", "[pad][processor]")
{
    constexpr int switched = 31200, secondCut = 66000, window = 2400;
    const auto largestStep = [] (int run)
    {
        PadSession session;
        auto& processor = session.processor;
        const auto& impact = processor.getCutter ().impact ().diagnostics ();
        set (processor, livecut::ids::impactFilterEnabled, 1);
        set (processor, livecut::ids::impactFilterCutoff, 800);
        set (processor, livecut::ids::impactFilterQ, 2);
        set (processor, livecut::ids::padProgram, 3); // Suction
        set (processor, livecut::ids::padY, 1); // fully squeezed, the most resonant band
        if (run == 0)
            useClassicSurface (processor);
        else
            set (processor, livecut::ids::padTouch, 1);
        session.render (switched);
        REQUIRE (processor.getCutter ().isCutting ());
        if (run == 1)
        {
            useClassicSurface (processor);
            session.render (secondCut - session.frame);
            CHECK_FALSE (impact.filterActive); // the band faded at the first cut
            CHECK (impact.bandLowHz == 0.0);
            session.render (window);
            CHECK (impact.filterActive); // and the low-pass came in at the second
            CHECK (impact.bandLowHz == 0.0);
        }
        else
            session.render (secondCut - session.frame);

        float largest = 0.f;
        for (int k = 2 * (switched - window); k < 2 * secondCut; ++k)
            largest = std::max (largest, std::abs (session.output[static_cast<std::size_t> (k)]
                                                   - session.output[static_cast<std::size_t> (k - 2)]));
        return largest;
    };
    const auto classicThroughout = largestStep (0);
    const auto suctionToClassic = largestStep (1);
    const auto suctionThroughout = largestStep (2);
    CAPTURE (suctionToClassic, suctionThroughout, classicThroughout);
    CHECK (suctionToClassic <= 3.f * std::max (suctionThroughout, classicThroughout));
}

// A tone inside the pad's tightest band, at its resonant low edge, the loudest
// place for it. The makeup follows the band's shape, so the cuts' breathing
// can't swell it the way a lagging level match did. The bar is the spec's: at
// most twice the band's own resonant peak for this tone with no makeup, from
// the engine alone, breathing at Suction's 16ths. The makeup there is below 1,
// and the cutter's splices ring the resonant edge harder than a steady tone.
TEST_CASE ("Suction's breathing never swells a tone in its band", "[pad][processor]")
{
    const auto band = livecut::engine::mapPad (livecut::engine::PadProgram::suction, 0.f, 1.f, {}).impact;
    constexpr double tone = .013 * 48000.0 / (2.0 * std::numbers::pi); // the session's signal, 99.3 Hz
    REQUIRE (tone > band.filter.bandLowHz);
    REQUIRE (tone < band.filter.bandHighHz);
    livecut::engine::ImpactEffects effects;
    effects.prepare (48000.0);
    effects.setParameters (band);
    double resonance = 0.0;
    for (int i = 0; i < 96000; ++i)
    {
        if (i % 6000 == 0)
            effects.startCut ({ 404, 0, 0.0, 0.0, static_cast<std::size_t> (i / 6000), 16 });
        auto left = signal (i, 0), right = left;
        effects.process (left, right);
        resonance = std::max (resonance, std::abs (left / effects.diagnostics ().levelHoldGain));
    }

    PadSession session;
    set (session.processor, livecut::ids::padProgram, 3); // Suction
    set (session.processor, livecut::ids::padX, 0);
    set (session.processor, livecut::ids::padY, 1);
    set (session.processor, livecut::ids::padTouch, 1);
    session.render (96000);
    REQUIRE (session.processor.getCutter ().isCutting ());
    float peak = 0.f;
    for (const auto sample : session.output)
        peak = std::max (peak, std::abs (sample));
    CAPTURE (peak, resonance);
    CHECK (peak <= 2.0 * resonance); // A lagging level match swelled a tone below the band to 8.5.
}

TEST_CASE ("Touching while stopped stays dry, and playing with the touch held starts on the grid", "[pad][processor]")
{
    PadSession session;
    session.head.info.setIsPlaying (false);
    set (session.processor, livecut::ids::padTouch, 1);
    session.render (5000);
    CHECK (session.dryFrom ());
    CHECK_FALSE (session.processor.getCutter ().isCutting ());
    session.head.info.setIsPlaying (true);
    session.render (999);
    CHECK_FALSE (session.processor.getCutter ().isCutting ());
    session.render (2);
    CHECK (session.processor.getCutter ().isCutting ());
}

TEST_CASE ("Toggling Straight while engaged takes effect without a fault", "[pad][processor]")
{
    PadSession session;
    set (session.processor, livecut::ids::padProgram, 2); // Glitch
    set (session.processor, livecut::ids::padTouch, 1);
    session.render (24000); // one beat
    REQUIRE (session.processor.getCutter ().isCutting ());
    set (session.processor, livecut::ids::padStraight, 1);
    session.render (4 * 96000);
    CHECK (session.processor.getCutter ().error () == livecut::engine::Cutter::Error::none);
    CHECK (session.processor.getCutter ().isCutting ());
    CHECK (session.processor.getDisplayState ().status.load () == livecut::DisplayState::Status::cutting);
}

TEST_CASE ("Seeking while engaged resumes cutting without a fault", "[pad][processor]")
{
    PadSession session;
    set (session.processor, livecut::ids::padTouch, 1);
    session.render (12000);
    REQUIRE (session.processor.getCutter ().isCutting ());
    session.frame = 97000;
    session.render (4999);
    CHECK_FALSE (session.processor.getCutter ().isCutting ());
    session.render (2);
    CHECK (session.processor.getCutter ().isCutting ());
    CHECK (session.processor.getDisplayState ().status.load () == livecut::DisplayState::Status::cutting);
}

TEST_CASE ("Pad processing never allocates", "[pad][processor]")
{
    PadSession session;
    juce::AudioBuffer<float> buffer (2, 4096);
    std::size_t allocations = 0;
    bool suctionFiltered = false;
    for (int step = 0; step < 40; ++step)
    {
        // Cut, then Suction, then Bounce while its band still sounds.
        set (session.processor, livecut::ids::padTouch, step < 20 ? 1 : 0);
        set (session.processor, livecut::ids::padProgram, step < 7 ? 0 : step < 14 ? 3 : 1);
        session.head.info.setPpqPosition (step * 4096 / 24000.0);
        session.head.info.setPpqPositionOfLastBarStart (std::floor (step * 4096 / 96000.0) * 4.0);
        session.head.info.setBarCount (step * 4096 / 96000);
        buffer.clear ();
        {
            const livecut::engine::testing::AllocationCounter counter;
            session.processor.processBlock (buffer, session.midi);
            allocations += counter.count ();
        }
        suctionFiltered = suctionFiltered || session.processor.getCutter ().impact ().diagnostics ().bandLowHz > 0.0;
    }
    CHECK (suctionFiltered);
    CHECK (allocations == 0);
}

TEST_CASE ("Discrete pad movement restarts at the current grid's next unit", "[pad][processor]")
{
    PadSession session;
    set (session.processor, livecut::ids::padTouch, 1);
    session.render (5000);
    set (session.processor, livecut::ids::padX, 0);
    session.render (1000);
    CHECK (session.processor.getCutter ().isCutting ());
    CHECK (session.processor.getCutter ().activeUnitsPerBar () == 16);
    session.render (1);
    CHECK_FALSE (session.processor.getCutter ().isCutting ());
    CHECK (session.processor.getCutter ().activeUnitsPerBar () == 8);
    session.render (6000);
    CHECK (session.processor.getCutter ().isCutting ());
}

TEST_CASE ("Unsupported timing clears the pad latch and resumes idle", "[pad][processor]")
{
    PadSession session;
    set (session.processor, livecut::ids::padHold, 1);
    set (session.processor, livecut::ids::padTouch, 1);
    session.render (1);
    set (session.processor, livecut::ids::padTouch, 0);
    session.render (1);
    REQUIRE (session.processor.getDisplayState ().padEngaged.load ());
    session.head.info.setBpm (20.0); // 12 seconds per bar exceeds Cutter's supported duration.
    session.render (1);
    CHECK_FALSE (session.processor.getDisplayState ().padEngaged.load ());
    CHECK (session.dryFrom (2));
    session.head.info.setBpm (120.0);
    session.render (12000);
    CHECK_FALSE (session.processor.getCutter ().isCutting ());
    CHECK (session.dryFrom (2));
    set (session.processor, livecut::ids::padTouch, 1);
    session.render (6000);
    CHECK (session.processor.getCutter ().isCutting ());
}

TEST_CASE ("Pad preparation and mode Apply ignore legacy Audio Hold", "[pad][processor]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    set (processor, livecut::ids::hold, 1);
    processor.prepareToPlay (48000, 1);
    CHECK (processor.getDisplayState ().hold.load () == livecut::engine::WetHold::State::live);
    TestPlayHead head;
    playFromBarOne (head);
    head.info.setIsPlaying (false);
    processor.setPlayHead (&head);
    juce::AudioBuffer<float> buffer (2, 1);
    buffer.clear ();
    juce::MidiBuffer midi;
    processor.processBlock (buffer, midi);
    set (processor, livecut::ids::mode, 1);
    processor.applyRequestedMode ();
    CHECK (processor.activeMode () == livecut::ProcessingMode::studio);
    CHECK (processor.getDisplayState ().hold.load () == livecut::engine::WetHold::State::live);
}

namespace
{
constexpr int filterProgram = 4, suctionProgram = 3;
constexpr int beatFrames = 24000, window = 2400;

// Filter at the top of its pad, touched from the start, renders in 512-frame
// blocks until a tail rings: the frame where it first does, and its note.
struct FirstTail
{
    int frame = -1;
    double hz = 0.0;
};

FirstTail firstFilterTail()
{
    PadSession session;
    set (session.processor, livecut::ids::padProgram, filterProgram);
    set (session.processor, livecut::ids::padY, 1);
    set (session.processor, livecut::ids::padTouch, 1);
    while (session.frame < 4 * 96000)
    {
        session.render (512);
        const auto& impact = session.processor.getCutter ().impact ().diagnostics ();
        if (impact.tailAmplitude > 0.0)
            return { session.frame, impact.tailHz };
    }
    return {};
}

float largestStepBetween (const std::vector<float>& output, int from, int to)
{
    float largest = 0.f;
    for (int k = 2 * from; k < 2 * to; ++k)
        largest = std::max (largest, std::abs (output[static_cast<std::size_t> (k)] - output[static_cast<std::size_t> (k - 2)]));
    return largest;
}

// The largest step within the window around the beat a switch lands on,
// playing `from` at the top of its pad until `switched`, then `to`.
float largestStepAtSwitch (int from, int to, int switched, bool tailRings = false)
{
    const auto beat = (switched / beatFrames + 1) * beatFrames;
    PadSession session;
    set (session.processor, livecut::ids::padProgram, static_cast<float> (from));
    set (session.processor, livecut::ids::padY, 1);
    set (session.processor, livecut::ids::padTouch, 1);
    session.render (switched);
    REQUIRE (session.processor.getCutter ().isCutting ());
    set (session.processor, livecut::ids::padProgram, static_cast<float> (to));
    session.render (beat - 64 - session.frame);
    if (tailRings)
        REQUIRE (session.processor.getCutter ().impact ().diagnostics ().tailAmplitude > 0.0);
    session.render (beat + window - session.frame);
    REQUIRE (session.processor.getDisplayState ().padActiveProgram.load () == to);
    return largestStepBetween (session.output, beat - window, beat + window);
}

// Suction's bar was 3x. Filter's measure at most 1.9x, the restart's splice
// ringing through the fading sweep, so 2.25x keeps it close.
void checkSwitch (int from, int to, int switched, bool tailRings)
{
    const auto switchedStep = largestStepAtSwitch (from, to, switched, tailRings);
    const auto fromThroughout = largestStepAtSwitch (from, from, switched);
    const auto toThroughout = largestStepAtSwitch (to, to, switched);
    CAPTURE (from, to, switched, switchedStep, fromThroughout, toThroughout);
    CHECK (switchedStep <= 2.25f * std::max (fromThroughout, toThroughout));
}
} // namespace

TEST_CASE ("Filter engages, releases and switches without a fault", "[pad][processor]")
{
    constexpr int beat = 24000, fade = 240;
    PadSession session;
    const auto& cutter = session.processor.getCutter ();
    const auto& display = session.processor.getDisplayState ();
    set (session.processor, livecut::ids::padProgram, filterProgram);
    set (session.processor, livecut::ids::padY, .75f);
    set (session.processor, livecut::ids::padTouch, 1);
    session.render (beat);
    REQUIRE (cutter.isCutting ());
    CHECK (display.padActiveProgram.load () == filterProgram);
    CHECK_FALSE (display.padUnavailable.load ());
    CHECK (cutter.activeProcedure () == livecut::engine::Procedure::cutProc11);
    CHECK (cutter.activeUnitsPerBar () == 16);
    CHECK (cutter.impact ().diagnostics ().filterActive);
    CHECK (cutter.impact ().diagnostics ().bandLowHz == 30.0); // the sweep's fixed high-pass
    CHECK (cutter.impact ().diagnostics ().bandHighHz < 18000.0); // and its low-pass, at X 0.5
    CHECK_FALSE (session.dryFrom ());

    // Moving the pad moves the cutoff without restarting the cuts.
    set (session.processor, livecut::ids::padX, 0);
    set (session.processor, livecut::ids::padY, 0);
    session.render (beat);
    CHECK (cutter.isCutting ());
    CHECK (cutter.impact ().diagnostics ().bandHighHz < 300.0);

    // Switching to Cut lands on the beat, and the sweep switches off.
    set (session.processor, livecut::ids::padProgram, 0);
    session.render (3 * beat - session.frame + 1);
    CHECK (display.padActiveProgram.load () == 0);
    CHECK (cutter.isCutting ());
    session.render (beat);
    CHECK_FALSE (cutter.impact ().diagnostics ().filterActive);

    // And back to Filter.
    set (session.processor, livecut::ids::padProgram, filterProgram);
    session.render (5 * beat - session.frame + 1);
    CHECK (display.padActiveProgram.load () == filterProgram);
    session.render (beat / 2);
    CHECK (cutter.impact ().diagnostics ().filterActive);

    // Releasing fades out on the next beat and returns to the input exactly.
    set (session.processor, livecut::ids::padTouch, 0);
    session.render (6 * beat + fade + 12000 - session.frame);
    CHECK_FALSE (cutter.isCutting ());
    CHECK (session.dryFrom (6 * beat + fade - 1));
    CHECK (cutter.error () == livecut::engine::Cutter::Error::none);
    CHECK (cutter.impact ().diagnostics ().tailAmplitude == 0.0); // the rack forgot its tail on idle
}

TEST_CASE ("Filter rings a tuned tail after a roll", "[pad][processor]")
{
    const auto tail = firstFilterTail ();
    REQUIRE (tail.frame > 0);
    // X 0.5 puts the cutoff at 1897 Hz, nearest A#6, and the tail an octave down on A#5.
    CHECK_THAT (tail.hz, Catch::Matchers::WithinRel (440.0 * std::exp2 (13.0 / 12.0), 1.0e-12));
}

TEST_CASE ("Filter at the same spot in the same bar sounds the same", "[pad][processor]")
{
    // One session plays only from bar 3; the other played elsewhere on the pad
    // first and let go. From bar 3 they are the same, tails included.
    constexpr int engage = 2 * 96000 + 6000, end = engage + 4 * 96000;
    PadSession fresh, played;
    for (auto* session : { &fresh, &played })
        set (session->processor, livecut::ids::padProgram, filterProgram);
    set (played.processor, livecut::ids::padX, .9f);
    set (played.processor, livecut::ids::padY, .4f);
    set (played.processor, livecut::ids::padTouch, 1);
    played.render (96000 + 30000);
    set (played.processor, livecut::ids::padTouch, 0);
    played.render (engage - played.frame);
    fresh.render (engage);
    REQUIRE_FALSE (played.processor.getCutter ().isCutting ());
    for (auto* session : { &fresh, &played })
    {
        set (session->processor, livecut::ids::padX, .3f);
        set (session->processor, livecut::ids::padY, 1);
        set (session->processor, livecut::ids::padTouch, 1);
    }
    bool rang = false;
    while (fresh.frame < end)
    {
        fresh.render (512);
        played.render (512);
        rang = rang || fresh.processor.getCutter ().impact ().diagnostics ().tailAmplitude > 0.0;
    }
    CHECK (rang);
    CHECK_FALSE (fresh.dryBetween (engage, end));
    CHECK (PadSession::sameBetween (fresh.output, played.output, engage, end));
}

// While a tail rings, the sweep keeps its shape, tail included, while its wet
// ramps out at the first cut of the new program, so the switch adds no click
// of its own. The bar is Suction's: 3x the larger reference, for the hard
// splice of the restart ringing through the fading filter.
TEST_CASE ("Switching Filter to Cut or Suction while a tail rings doesn't click", "[pad][processor]")
{
    const auto tail = firstFilterTail ();
    REQUIRE (tail.frame > 0);
    REQUIRE (tail.frame % beatFrames != 0);
    checkSwitch (filterProgram, 0, tail.frame, true);
    checkSwitch (filterProgram, suctionProgram, tail.frame, true);
}

TEST_CASE ("Switching Suction to Filter doesn't click", "[pad][processor]")
{
    checkSwitch (suctionProgram, filterProgram, 30000, false);
}

// A Pad to Classic switch with Classic's Impact filter on, while a tail rings:
// at Classic's first block the sweep, tail included, keeps its shape while its
// wet ramps out, and Classic's low-pass comes in at a later cut. The window
// runs from the cut where the sweep starts to fade until it has cleared. It
// used to start 2400 samples before the switch, where a Filter splice the
// same in both runs set the bar at 0.98, 62 times Classic's own step. (The
// outputs part before that cut: the switch hands the cutter all of Classic's
// settings at once, so there is no exact twin to compare with.)
TEST_CASE ("Switching Filter to Classic's filter while a tail rings doesn't click", "[pad][processor]")
{
    const auto tail = firstFilterTail ();
    REQUIRE (tail.frame > 0);
    const auto switched = tail.frame;
    int fading = 0, cleared = 0;
    const auto largestStep = [&] (int run)
    {
        PadSession session;
        auto& processor = session.processor;
        const auto& impact = processor.getCutter ().impact ().diagnostics ();
        set (processor, livecut::ids::impactFilterEnabled, 1);
        set (processor, livecut::ids::impactFilterCutoff, 800);
        set (processor, livecut::ids::impactFilterQ, 2);
        set (processor, livecut::ids::padProgram, filterProgram);
        set (processor, livecut::ids::padY, 1);
        if (run == 0)
            useClassicSurface (processor);
        else
            set (processor, livecut::ids::padTouch, 1);
        session.render (switched);
        REQUIRE (processor.getCutter ().isCutting ());
        if (run == 1)
        {
            REQUIRE (impact.tailAmplitude > 0.0);
            useClassicSurface (processor);
            while (impact.filterActive && session.frame < switched + 96000)
                session.render (1);
            REQUIRE_FALSE (impact.filterActive); // the cut where the sweep starts to fade
            fading = session.frame - 1;
            while (impact.bandLowHz != 0.0 && session.frame < switched + 96000)
                session.render (16);
            REQUIRE (impact.bandLowHz == 0.0); // the sweep faded and cleared,
            CHECK (impact.tailAmplitude == 0.0); // its tail with it
            cleared = session.frame;
        }
        session.render (cleared + window - session.frame);
        return largestStepBetween (session.output, fading, cleared + window);
    };
    const auto filterToClassic = largestStep (1);
    const auto filterThroughout = largestStep (2);
    const auto classicThroughout = largestStep (0);
    CAPTURE (switched, fading, cleared, filterToClassic, filterThroughout, classicThroughout);
    CHECK (filterToClassic <= 1.25f * std::max (filterThroughout, classicThroughout)); // measured 0.89
}

TEST_CASE ("Filter's tails are identical for every host buffer size", "[pad][processor]")
{
    const auto tail = firstFilterTail ();
    REQUIRE (tail.frame > 0);
    const auto render = [&tail] (int size)
    {
        PadSession session;
        set (session.processor, livecut::ids::padProgram, filterProgram);
        set (session.processor, livecut::ids::padY, 1);
        session.render (5000, size);
        set (session.processor, livecut::ids::padTouch, 1);
        session.render (tail.frame + 20000 - session.frame, size);
        CHECK (session.processor.getCutter ().impact ().diagnostics ().tailAmplitude > 0.0);
        set (session.processor, livecut::ids::padTouch, 0);
        session.render (30000, size);
        CHECK_FALSE (session.dryFrom ());
        return session.output;
    };
    const auto expected = render (1);
    for (const int size : { 37, 512, 4096 })
    {
        CAPTURE (size);
        const auto actual = render (size);
        CHECK (actual.size () == expected.size ());
        CHECK (std::memcmp (actual.data (), expected.data (), expected.size () * sizeof (float)) == 0);
    }
}

TEST_CASE ("Filter never allocates, tails included", "[pad][processor]")
{
    PadSession session;
    juce::AudioBuffer<float> buffer (2, 512);
    std::size_t allocations = 0;
    bool rang = false;
    set (session.processor, livecut::ids::padProgram, filterProgram);
    set (session.processor, livecut::ids::padY, 1);
    set (session.processor, livecut::ids::padTouch, 1);
    for (int frame = 0; frame < 4 * 96000; frame += 512)
    {
        if (frame == 3 * 96000) // then to Suction while a tail may ring, and let go
        {
            set (session.processor, livecut::ids::padProgram, suctionProgram);
            set (session.processor, livecut::ids::padTouch, 0);
        }
        session.head.info.setPpqPosition (frame / 24000.0);
        session.head.info.setPpqPositionOfLastBarStart (std::floor (frame / 96000.0) * 4.0);
        session.head.info.setBarCount (frame / 96000);
        for (int i = 0; i < 512; ++i)
            for (int channel = 0; channel < 2; ++channel)
                buffer.setSample (channel, i, signal (frame + i, channel));
        {
            const livecut::engine::testing::AllocationCounter counter;
            session.processor.processBlock (buffer, session.midi);
            allocations += counter.count ();
        }
        rang = rang || session.processor.getCutter ().impact ().diagnostics ().tailAmplitude > 0.0;
    }
    CHECK (rang);
    CHECK (allocations == 0);
}

TEST_CASE ("Filter rings in tune at 44.1, 48 and 96 kHz", "[pad][processor]")
{
    const auto rate = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (rate);
    PadSession session (false, rate);
    const auto& cutter = session.processor.getCutter ();
    set (session.processor, livecut::ids::padProgram, filterProgram);
    set (session.processor, livecut::ids::padY, 1);
    set (session.processor, livecut::ids::padTouch, 1);
    double hz = 0.0;
    while (hz == 0.0 && session.frame < static_cast<int> (8.0 * rate))
    {
        session.render (512);
        hz = cutter.impact ().diagnostics ().tailHz;
    }
    CHECK_THAT (hz, Catch::Matchers::WithinRel (440.0 * std::exp2 (13.0 / 12.0), 1.0e-12)); // A#5
    CHECK (cutter.isCutting ());
    CHECK (cutter.error () == livecut::engine::Cutter::Error::none);
    CHECK (std::all_of (session.output.begin (), session.output.end (), [] (float sample) { return std::isfinite (sample); }));
}

namespace
{
// FNV-1a over the raw bytes of each float.
std::uint64_t outputHash (const std::vector<float>& output)
{
    auto value = UINT64_C (0xcbf29ce484222325);
    for (const auto sample : output)
    {
        const auto bits = std::bit_cast<std::uint32_t> (sample);
        for (int shift = 0; shift < 32; shift += 8)
        {
            value ^= (bits >> shift) & 0xffu;
            value *= UINT64_C (0x100000001b3);
        }
    }
    return value;
}
} // namespace

TEST_CASE ("Every built program is bit-identical", "[pad][processor][pinned]")
{
    // Measured before Tape existed: each program touched at (0.3, 0.75) for
    // two and a half bars, then let go for a bar.
    const auto [program, expected] = GENERATE (table<int, std::uint64_t> ({
        { 0, UINT64_C (0xc3552b8eade6fafe) },
        { 1, UINT64_C (0x24143edda0fe92b3) },
        { 2, UINT64_C (0x01895a9e6e6d8aad) },
        { 3, UINT64_C (0x76768e46ff115f21) },
        { 4, UINT64_C (0x86d884687164bc1b) },
    }));
    CAPTURE (program);
    PadSession session;
    set (session.processor, livecut::ids::padProgram, static_cast<float> (program));
    set (session.processor, livecut::ids::padX, .3f);
    set (session.processor, livecut::ids::padY, .75f);
    set (session.processor, livecut::ids::padTouch, 1);
    session.render (240000);
    set (session.processor, livecut::ids::padTouch, 0);
    session.render (96000);
    CHECK_FALSE (session.dryFrom ());
    CHECK (outputHash (session.output) == expected);
}

namespace
{
constexpr int tapeProgram = 5, cutProgram = 0;
constexpr int barFrames = 96000;

// Bar 3 of a Tape session at X 1 and Y 0 is one four-beat stop, so a switch
// requested here lands on its third beat, halfway through the stop.
constexpr int midStop = 3 * barFrames + 30000, midStopBeat = 3 * barFrames + 2 * beatFrames;

// The largest step within the window around the beat a switch lands on,
// playing `from` at (x, y) until `switched`, then `to`.
float largestStepSwitching (int from, int to, int switched, float x, float y)
{
    const auto beat = (switched / beatFrames + 1) * beatFrames;
    PadSession session;
    set (session.processor, livecut::ids::padProgram, static_cast<float> (from));
    set (session.processor, livecut::ids::padX, x);
    set (session.processor, livecut::ids::padY, y);
    set (session.processor, livecut::ids::padTouch, 1);
    session.render (switched);
    REQUIRE (session.processor.getCutter ().isCutting ());
    if (from == tapeProgram && switched == midStop)
        REQUIRE (session.processor.getCutter ().fractureDiagnostics ().tapeSpeed < 1.0); // mid-stop
    set (session.processor, livecut::ids::padProgram, static_cast<float> (to));
    session.render (beat + window - session.frame);
    REQUIRE (session.processor.getDisplayState ().padActiveProgram.load () == to);
    CHECK (session.processor.getCutter ().error () == livecut::engine::Cutter::Error::none);
    return largestStepBetween (session.output, beat - window, beat + window);
}

struct SwitchSteps
{
    float switched, from, to;
};

SwitchSteps switchSteps (int from, int to, int switched, float x, float y)
{
    return { largestStepSwitching (from, to, switched, x, y), largestStepSwitching (from, from, switched, x, y),
             largestStepSwitching (to, to, switched, x, y) };
}
} // namespace

TEST_CASE ("Engaging Tape is seamless: the input until its first stop", "[pad][processor][tape]")
{
    // X 0.5 and Y 0: one two-beat stop, at the end of bar 3, then its spin-up.
    constexpr int stopStart = 3 * barFrames + 2 * beatFrames, spun = 4 * barFrames + 3000;
    PadSession session;
    set (session.processor, livecut::ids::padProgram, tapeProgram);
    set (session.processor, livecut::ids::padY, 0);
    session.render (5000);
    set (session.processor, livecut::ids::padTouch, 1); // mid-16th, mid-bar
    session.render (5 * barFrames - session.frame);
    REQUIRE (session.processor.getCutter ().isCutting ());
    CHECK (session.processor.getCutter ().fractureDiagnostics ().family == livecut::engine::FractureProcedure::tapeCut);
    CHECK (session.dryBetween (0, stopStart));
    CHECK_FALSE (session.dryBetween (stopStart, stopStart + 2400)); // it brakes
    CHECK (session.output[2 * (4 * barFrames - 1)] == 0.f);         // to silence on the bar line
    CHECK (session.output[2 * (4 * barFrames - 1) + 1] == 0.f);
    CHECK (session.dryBetween (spun, 5 * barFrames)); // and spins back up onto the track
    CHECK (session.processor.getCutter ().error () == livecut::engine::Cutter::Error::none);
}

TEST_CASE ("Tape engages, releases and switches without a fault", "[pad][processor][tape]")
{
    constexpr int fade = 240;
    PadSession session;
    const auto& cutter = session.processor.getCutter ();
    const auto& display = session.processor.getDisplayState ();
    set (session.processor, livecut::ids::padProgram, tapeProgram);
    set (session.processor, livecut::ids::padY, .75f); // a stop every two beats
    set (session.processor, livecut::ids::padTouch, 1);
    session.render (barFrames);
    REQUIRE (cutter.isCutting ());
    CHECK (display.padActiveProgram.load () == tapeProgram);
    CHECK_FALSE (display.padUnavailable.load ());
    CHECK (cutter.fractureDiagnostics ().family == livecut::engine::FractureProcedure::tapeCut);
    CHECK (cutter.activeUnitsPerBar () == 16);
    CHECK_FALSE (session.dryFrom ());

    // Moving the pad changes the stops without restarting the cuts.
    set (session.processor, livecut::ids::padX, 1);
    set (session.processor, livecut::ids::padY, 1);
    session.render (barFrames);
    CHECK (cutter.isCutting ());

    // Switching to Cut lands on the beat, and Fracture goes back to Classic.
    set (session.processor, livecut::ids::padProgram, cutProgram);
    session.render (9 * beatFrames - session.frame + 1);
    CHECK (display.padActiveProgram.load () == cutProgram);
    CHECK (cutter.isCutting ());
    CHECK (cutter.fractureDiagnostics ().family == livecut::engine::FractureProcedure::classic);

    // And back to Tape.
    set (session.processor, livecut::ids::padProgram, tapeProgram);
    session.render (11 * beatFrames - session.frame + 1);
    CHECK (display.padActiveProgram.load () == tapeProgram);
    session.render (beatFrames);
    CHECK (cutter.fractureDiagnostics ().family == livecut::engine::FractureProcedure::tapeCut);

    // Releasing fades out on the next beat and returns to the input exactly.
    set (session.processor, livecut::ids::padTouch, 0);
    session.render (13 * beatFrames + fade + 12000 - session.frame);
    CHECK_FALSE (cutter.isCutting ());
    CHECK (session.dryFrom (13 * beatFrames + fade - 1));
    CHECK (cutter.error () == livecut::engine::Cutter::Error::none);
}

// A switch out of Tape mid-stop lets the stop play on under the new program's
// first cut, fading out over 5 ms. Spliced straight to the new cut, the slowed
// tape stepped 0.99, 75 times the larger reference: the two programs played
// throughout in the same window. Measured now: 1.01x to Cut and 1.11x to Filter.
TEST_CASE ("Switching Tape to Cut or Filter in the middle of a stop doesn't click", "[pad][processor][tape]")
{
    for (const auto to : { cutProgram, filterProgram })
    {
        const auto steps = switchSteps (tapeProgram, to, midStop, 1.f, 0.f);
        CAPTURE (to, steps.switched, steps.from, steps.to);
        CHECK (steps.switched <= 1.25f * std::max (steps.from, steps.to));
    }
}

// On the beat a stop ends, the switch fades the new program in from the stop's
// silence over the same 5 ms, rather than starting hard. Measured 0.31x the references.
TEST_CASE ("Switching Tape to Cut where a stop ends doesn't click", "[pad][processor][tape]")
{
    const auto steps = switchSteps (tapeProgram, cutProgram, 30000, .5f, 1.f);
    CAPTURE (steps.switched, steps.from, steps.to);
    CHECK (steps.switched <= 1.25f * std::max (steps.from, steps.to));
}

// Tape's first cut is the input itself, so the switch is Cut's last cut
// meeting the track. Measured 1.0x the references.
TEST_CASE ("Switching Cut to Tape doesn't click", "[pad][processor][tape]")
{
    const auto steps = switchSteps (cutProgram, tapeProgram, midStop, 1.f, 0.f);
    CAPTURE (steps.switched, steps.from, steps.to);
    CHECK (steps.switched <= 1.25f * std::max (steps.from, steps.to));
}

// Pad to Classic in the middle of a four-beat stop: the stop finishes in
// silence on the bar line, where Classic's family lands, and Classic's first cut
// starts from that silence. Measured 0.64x Classic's own steps in the window.
TEST_CASE ("Switching Tape to Classic in the middle of a stop doesn't click", "[pad][processor][tape]")
{
    constexpr int barLine = 4 * barFrames;
    const auto largestStep = [] (int run)
    {
        PadSession session;
        auto& processor = session.processor;
        const auto& fracture = processor.getCutter ().fractureDiagnostics ();
        set (processor, livecut::ids::padProgram, tapeProgram);
        set (processor, livecut::ids::padX, 1);
        set (processor, livecut::ids::padY, 0);
        if (run == 0)
            useClassicSurface (processor);
        else
            set (processor, livecut::ids::padTouch, 1);
        session.render (midStop);
        REQUIRE (processor.getCutter ().isCutting ());
        if (run == 1)
        {
            REQUIRE (fracture.tapeSpeed < 1.0);
            useClassicSurface (processor);
            session.render (barLine - 1 - session.frame);
            CHECK (fracture.family == livecut::engine::FractureProcedure::tapeCut); // the stop finishes
            session.render (2);
            CHECK (fracture.family == livecut::engine::FractureProcedure::classic); // Classic on the bar line
        }
        session.render (barLine + window - session.frame);
        CHECK (processor.getCutter ().error () == livecut::engine::Cutter::Error::none);
        return largestStepBetween (session.output, barLine - window, barLine + window);
    };
    const auto tapeToClassic = largestStep (1);
    const auto tapeThroughout = largestStep (2);
    const auto classicThroughout = largestStep (0);
    CAPTURE (tapeToClassic, tapeThroughout, classicThroughout);
    CHECK (tapeToClassic <= std::max (tapeThroughout, classicThroughout));
}

TEST_CASE ("Tape at the same spot in the same bar sounds the same", "[pad][processor][tape]")
{
    // One session plays only from bar 3; the other played elsewhere on the pad
    // first and let go. From bar 3 they are the same, stops and spin-ups included.
    constexpr int engage = 2 * barFrames + 6000, end = engage + 3 * barFrames;
    PadSession fresh, played;
    for (auto* session : { &fresh, &played })
        set (session->processor, livecut::ids::padProgram, tapeProgram);
    set (played.processor, livecut::ids::padX, .9f);
    set (played.processor, livecut::ids::padY, 1);
    set (played.processor, livecut::ids::padTouch, 1);
    played.render (barFrames + 30000);
    set (played.processor, livecut::ids::padTouch, 0);
    played.render (engage - played.frame);
    fresh.render (engage);
    REQUIRE_FALSE (played.processor.getCutter ().isCutting ());
    for (auto* session : { &fresh, &played })
    {
        set (session->processor, livecut::ids::padX, .3f);
        set (session->processor, livecut::ids::padY, .6f);
        set (session->processor, livecut::ids::padTouch, 1);
    }
    while (fresh.frame < end)
    {
        fresh.render (512);
        played.render (512);
    }
    CHECK_FALSE (fresh.dryBetween (engage, end));
    CHECK (PadSession::sameBetween (fresh.output, played.output, engage, end));
}

TEST_CASE ("Tape's stops land on the grid, the same for every host buffer size, at 44.1, 48 and 96 kHz", "[pad][processor][tape]")
{
    const auto rate = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (rate);
    const auto beat = static_cast<int> (rate / 2.0);
    const auto render = [rate, beat] (int size)
    {
        PadSession session (false, rate);
        set (session.processor, livecut::ids::padProgram, tapeProgram);
        set (session.processor, livecut::ids::padX, .3f);
        set (session.processor, livecut::ids::padY, 1); // a half-beat stop on every beat
        session.render (beat, size);
        set (session.processor, livecut::ids::padTouch, 1); // on a beat, so the restart is a whole sample
        session.render (6 * beat, size);
        set (session.processor, livecut::ids::padTouch, 0);
        session.render (beat, size);
        const auto& cutter = session.processor.getCutter ();
        CHECK (cutter.error () == livecut::engine::Cutter::Error::none);
        CHECK_FALSE (cutter.isCutting ());
        // Each stop ends in silence on its beat, the first at beat 2.
        for (int b = 2; b <= 7; ++b)
        {
            CAPTURE (b);
            CHECK (session.output[static_cast<std::size_t> (2 * (b * beat - 1))] == 0.f);
            CHECK (session.output[static_cast<std::size_t> (2 * (b * beat - 1) + 1)] == 0.f);
            CHECK (session.output[static_cast<std::size_t> (2 * (b * beat - beat / 4))] != 0.f); // braking, not silent
        }
        CHECK (session.dryBetween (0, beat + beat / 2)); // the track until the first stop
        CHECK (std::all_of (session.output.begin (), session.output.end (), [] (float sample) { return std::isfinite (sample); }));
        return session.output;
    };
    const auto expected = render (1);
    for (const int size : { 37, 512, 4096 })
    {
        CAPTURE (size);
        const auto actual = render (size);
        CHECK (actual.size () == expected.size ());
        CHECK (std::memcmp (actual.data (), expected.data (), expected.size () * sizeof (float)) == 0);
    }
}

TEST_CASE ("Tape never allocates, stops and switches included", "[pad][processor][tape]")
{
    PadSession session;
    juce::AudioBuffer<float> buffer (2, 512);
    std::size_t allocations = 0;
    bool stopped = false;
    set (session.processor, livecut::ids::padProgram, tapeProgram);
    set (session.processor, livecut::ids::padX, 1);
    set (session.processor, livecut::ids::padY, .7f);
    set (session.processor, livecut::ids::padTouch, 1);
    for (int frame = 0; frame < 4 * barFrames; frame += 512)
    {
        if (frame == 2 * barFrames + 512 * 20) // to Cut, then back, and let go
            set (session.processor, livecut::ids::padProgram, cutProgram);
        if (frame == 3 * barFrames)
            set (session.processor, livecut::ids::padProgram, tapeProgram);
        if (frame == 3 * barFrames + 512 * 100)
            set (session.processor, livecut::ids::padTouch, 0);
        session.head.info.setPpqPosition (frame / 24000.0);
        session.head.info.setPpqPositionOfLastBarStart (std::floor (frame / 96000.0) * 4.0);
        session.head.info.setBarCount (frame / 96000);
        for (int i = 0; i < 512; ++i)
            for (int channel = 0; channel < 2; ++channel)
                buffer.setSample (channel, i, signal (frame + i, channel));
        {
            const livecut::engine::testing::AllocationCounter counter;
            session.processor.processBlock (buffer, session.midi);
            allocations += counter.count ();
        }
        stopped = stopped || session.processor.getCutter ().fractureDiagnostics ().tapeSpeed < 1.0;
    }
    CHECK (stopped);
    CHECK (allocations == 0);
}

TEST_CASE ("At 93, 124 and 140 bpm, engaged Tape is the track until its first stop", "[pad][processor][tape][tempo]")
{
    // A bar of fractional samples used to leave one silent frame at some bar lines.
    const auto rate = GENERATE (44100.0, 48000.0, 96000.0);
    const auto bpm = GENERATE (93.0, 124.0, 140.0);
    CAPTURE (rate, bpm);
    const auto beat = rate * 60.0 / bpm;
    const auto stopStart = static_cast<int> (14.0 * beat); // X 0.5, Y 0: beats 2 to 4 of bar 3
    PadSession session (false, rate, bpm);
    set (session.processor, livecut::ids::padProgram, tapeProgram);
    set (session.processor, livecut::ids::padY, 0);
    session.render (777);
    set (session.processor, livecut::ids::padTouch, 1);
    session.render (stopStart + static_cast<int> (beat) - session.frame);
    REQUIRE (session.processor.getCutter ().isCutting ());
    CHECK (session.dryBetween (0, stopStart - 2));
    CHECK_FALSE (session.dryBetween (stopStart, stopStart + static_cast<int> (beat)));
    CHECK (session.processor.getCutter ().error () == livecut::engine::Cutter::Error::none);
}

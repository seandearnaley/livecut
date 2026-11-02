// SPDX-License-Identifier: GPL-3.0-or-later

#include "PluginProcessor.h"
#include "ProcessorTestSupport.h"
#include "engine/AllocationCounter.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <cstring>
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

    explicit PadSession (bool spectral = false)
    {
        if (spectral)
        {
            set (processor, livecut::ids::resonanceSpectralEnabled, 1);
            processor.setNonRealtime (true);
        }
        playFromBarOne (head);
        processor.setPlayHead (&head);
        processor.prepareToPlay (48000, 4096);
    }

    void render (int count, int bufferSize = 512)
    {
        while (count > 0)
        {
            const int size = std::min (count, bufferSize);
            head.info.setPpqPosition (frame / 24000.0);
            head.info.setPpqPositionOfLastBarStart (std::floor (frame / 96000.0) * 4.0);
            head.info.setBarCount (frame / 96000);
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
    const auto render = [] (int size)
    {
        PadSession session;
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
    set (session.processor, livecut::ids::padProgram, 3); // Suction
    set (session.processor, livecut::ids::padTouch, 1);
    session.render (96000);
    CHECK (session.dryFrom ());
    CHECK (session.processor.getDisplayState ().padUnavailable.load ());
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
    for (int step = 0; step < 40; ++step)
    {
        set (session.processor, livecut::ids::padTouch, step < 20 ? 1 : 0);
        set (session.processor, livecut::ids::padProgram, step < 10 ? 0 : 1);
        session.head.info.setPpqPosition (step * 4096 / 24000.0);
        session.head.info.setPpqPositionOfLastBarStart (std::floor (step * 4096 / 96000.0) * 4.0);
        session.head.info.setBarCount (step * 4096 / 96000);
        buffer.clear ();
        {
            const livecut::engine::testing::AllocationCounter counter;
            session.processor.processBlock (buffer, session.midi);
            allocations += counter.count ();
        }
    }
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

// SPDX-License-Identifier: GPL-3.0-or-later

#include "Parameters.h"
#include "PluginProcessor.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstring>

namespace
{
juce::AudioProcessor::BusesLayout makeLayout (const juce::AudioChannelSet& input,
                                              const juce::AudioChannelSet& output)
{
    juce::AudioProcessor::BusesLayout layout;
    layout.inputBuses.add (input);
    layout.outputBuses.add (output);
    return layout;
}

void fillWithTestSignal (juce::AudioBuffer<float>& buffer)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            buffer.setSample (channel, sample, std::sin (0.01f * (float) (sample + 100 * channel)));
}

bool sameAudio (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    for (int channel = 0; channel < a.getNumChannels(); ++channel)
        if (std::memcmp (a.getReadPointer (channel), b.getReadPointer (channel), sizeof (float) * (size_t) a.getNumSamples()) != 0)
            return false;

    return true;
}

struct TestPlayHead final : juce::AudioPlayHead
{
    juce::Optional<PositionInfo> getPosition() const override { return info; }

    PositionInfo info;
};

void playFromBarOne (TestPlayHead& playHead)
{
    playHead.info.setIsPlaying (true);
    playHead.info.setPpqPosition (0.0);
    playHead.info.setPpqPositionOfLastBarStart (0.0);
    playHead.info.setBarCount (0);
    playHead.info.setBpm (120.0);
    playHead.info.setTimeSignature (juce::AudioPlayHead::TimeSignature { 4, 4 });
}
} // namespace

TEST_CASE ("Processor reports its identity", "[processor]")
{
    LivecutAudioProcessor processor;

    CHECK (processor.getName() == "Livecut");
    CHECK_FALSE (processor.acceptsMidi());
    CHECK_FALSE (processor.producesMidi());
}

// Cuts replay up to a bar of input after the input itself has stopped.
TEST_CASE ("Processor reports a tail as long as the longest bar it cuts", "[processor]")
{
    LivecutAudioProcessor processor;

    CHECK_THAT (processor.getTailLengthSeconds(), Catch::Matchers::WithinAbs (livecut::engine::Cutter::maxBarSeconds, 0.0));
}

// Steinberg's VST3 validator rejects a program list containing an unnamed program.
TEST_CASE ("Processor exposes exactly one named program", "[processor][programs]")
{
    LivecutAudioProcessor processor;

    REQUIRE (processor.getNumPrograms() == 1);
    CHECK (processor.getProgramName (0).isNotEmpty());
}

TEST_CASE ("Processor defaults to a stereo input and output bus", "[processor][buses]")
{
    LivecutAudioProcessor processor;

    CHECK (processor.getMainBusNumInputChannels() == 2);
    CHECK (processor.getMainBusNumOutputChannels() == 2);
}

TEST_CASE ("Processor accepts only a stereo-to-stereo layout", "[processor][buses]")
{
    LivecutAudioProcessor processor;
    const auto stereo = juce::AudioChannelSet::stereo();
    const auto mono = juce::AudioChannelSet::mono();
    const auto surround = juce::AudioChannelSet::create5point1();

    CHECK (processor.checkBusesLayoutSupported (makeLayout (stereo, stereo)));
    CHECK_FALSE (processor.checkBusesLayoutSupported (makeLayout (mono, mono)));
    CHECK_FALSE (processor.checkBusesLayoutSupported (makeLayout (mono, stereo)));
    CHECK_FALSE (processor.checkBusesLayoutSupported (makeLayout (stereo, mono)));
    CHECK_FALSE (processor.checkBusesLayoutSupported (makeLayout (surround, surround)));
}

TEST_CASE ("Processor passes audio through when the host has no transport", "[processor][audio]")
{
    constexpr int blockSize = 512;
    LivecutAudioProcessor processor;
    processor.prepareToPlay (48000.0, blockSize);

    juce::AudioBuffer<float> buffer (2, blockSize);
    fillWithTestSignal (buffer);
    const juce::AudioBuffer<float> expected (buffer);
    juce::MidiBuffer midi;

    processor.processBlock (buffer, midi);

    CHECK (sameAudio (buffer, expected));
    processor.releaseResources();
}

TEST_CASE ("Processor passes audio through while the host is stopped", "[processor][audio]")
{
    LivecutAudioProcessor processor;
    TestPlayHead playHead;
    playFromBarOne (playHead);
    playHead.info.setIsPlaying (false);
    processor.setPlayHead (&playHead);
    processor.prepareToPlay (48000.0, 512);

    juce::AudioBuffer<float> buffer (2, 512);
    fillWithTestSignal (buffer);
    const juce::AudioBuffer<float> expected (buffer);
    juce::MidiBuffer midi;

    processor.processBlock (buffer, midi);

    CHECK (sameAudio (buffer, expected));
    CHECK_FALSE (processor.getCutter().isCutting());
}

TEST_CASE ("Processor cuts in time with a playing host", "[processor][audio]")
{
    LivecutAudioProcessor processor;
    TestPlayHead playHead;
    playFromBarOne (playHead);
    processor.setPlayHead (&playHead);
    processor.prepareToPlay (48000.0, 512);

    juce::AudioBuffer<float> buffer (2, 512);
    fillWithTestSignal (buffer);
    juce::MidiBuffer midi;

    processor.processBlock (buffer, midi);

    CHECK (processor.getCutter().isCutting());
    CHECK (processor.getCutter().phraseStartBar() == 0);
}

TEST_CASE ("Processor applies parameter changes to the cutter", "[processor][parameters]")
{
    LivecutAudioProcessor processor;
    TestPlayHead playHead;
    playFromBarOne (playHead);
    processor.setPlayHead (&playHead);
    processor.prepareToPlay (48000.0, 512);
    auto* subdivision = processor.getParameterState().getParameter (livecut::ids::subdivision);
    REQUIRE (subdivision != nullptr);
    subdivision->setValueNotifyingHost (subdivision->convertTo0to1 (5.0f)); // "24"

    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;
    processor.processBlock (buffer, midi);

    CHECK (processor.getCutter().activeUnitsPerBar() == 24);
}

// Livecut's getStateInformation and setStateInformation were empty, so no host ever saved its settings.
TEST_CASE ("Processor state restores every parameter", "[processor][state]")
{
    LivecutAudioProcessor original;
    REQUIRE (original.getParameters().size() > 0);

    // A legal, non-default value for every parameter, switches included.
    for (auto* parameter : original.getParameters())
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter);
        REQUIRE (ranged != nullptr);
        ranged->setValueNotifyingHost (ranged->convertTo0to1 (ranged->convertFrom0to1 (0.63f)));
    }

    juce::MemoryBlock state;
    original.getStateInformation (state);

    LivecutAudioProcessor restored;
    restored.setStateInformation (state.getData(), static_cast<int> (state.getSize()));

    const auto originalParameters = original.getParameters();
    const auto restoredParameters = restored.getParameters();
    REQUIRE (originalParameters.size() == restoredParameters.size());

    for (int i = 0; i < originalParameters.size(); ++i)
    {
        INFO (originalParameters[i]->getName (64));
        CHECK_THAT (restoredParameters[i]->getValue(), Catch::Matchers::WithinAbs (originalParameters[i]->getValue(), 1.0e-6));
    }

}

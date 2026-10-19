// SPDX-License-Identifier: GPL-3.0-or-later

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
} // namespace

TEST_CASE ("Processor reports its identity", "[processor]")
{
    LivecutAudioProcessor processor;

    CHECK (processor.getName() == "Livecut");
    CHECK_FALSE (processor.acceptsMidi());
    CHECK_FALSE (processor.producesMidi());
    CHECK_THAT (processor.getTailLengthSeconds(), Catch::Matchers::WithinAbs (0.0, 0.0));
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

// Scaffold behaviour: until the cutting engine lands, Livecut is a transparent pass-through.
TEST_CASE ("Processor passes stereo audio through unchanged", "[processor][audio]")
{
    constexpr int blockSize = 512;
    LivecutAudioProcessor processor;
    processor.prepareToPlay (48000.0, blockSize);

    juce::AudioBuffer<float> buffer (2, blockSize);
    fillWithTestSignal (buffer);
    const juce::AudioBuffer<float> expected (buffer);
    juce::MidiBuffer midi;

    processor.processBlock (buffer, midi);

    for (int channel = 0; channel < 2; ++channel)
        CHECK (std::memcmp (buffer.getReadPointer (channel),
                            expected.getReadPointer (channel),
                            sizeof (float) * blockSize) == 0);

    processor.releaseResources();
}

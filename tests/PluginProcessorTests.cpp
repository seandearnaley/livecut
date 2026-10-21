// SPDX-License-Identifier: GPL-3.0-or-later

#include "Parameters.h"
#include "PluginProcessor.h"
#include "engine/AllocationCounter.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
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

TEST_CASE ("A set saved by 0.1.0 reloads with its procedure and Bounce's defaults", "[processor][state]")
{
    LivecutAudioProcessor original;
    auto* procedure = original.getParameterState().getParameter (livecut::ids::procedure);
    REQUIRE (procedure != nullptr);
    const auto sqPusher = static_cast<float> (livecut::engine::Procedure::sqPusher);
    procedure->setValueNotifyingHost (procedure->convertTo0to1 (sqPusher));

    juce::MemoryBlock state;
    original.getStateInformation (state);
    auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), static_cast<int> (state.getSize()));
    REQUIRE (xml != nullptr);

    // A 0.1.0 set has no Bounce parameters.
    for (const auto* id : { livecut::ids::bounceChance, livecut::ids::ratchetChance,
                           livecut::ids::climb, livecut::ids::halftimeChance })
    {
        auto* child = xml->getChildByAttribute ("id", id);
        REQUIRE (child != nullptr);
        REQUIRE (child->hasTagName ("PARAM"));
        xml->removeChildElement (child, true);
    }
    juce::AudioProcessor::copyXmlToBinary (*xml, state);

    LivecutAudioProcessor restored;
    restored.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
    const auto& restoredState = restored.getParameterState();
    CHECK_THAT (restoredState.getRawParameterValue (livecut::ids::procedure)->load(), Catch::Matchers::WithinAbs (sqPusher, 1.0e-6));
    CHECK_THAT (restoredState.getRawParameterValue (livecut::ids::bounceChance)->load(), Catch::Matchers::WithinAbs (0.4, 1.0e-6));
    CHECK_THAT (restoredState.getRawParameterValue (livecut::ids::ratchetChance)->load(), Catch::Matchers::WithinAbs (0.7, 1.0e-6));
    CHECK_THAT (restoredState.getRawParameterValue (livecut::ids::climb)->load(), Catch::Matchers::WithinAbs (7.0, 1.0e-6));
    CHECK_THAT (restoredState.getRawParameterValue (livecut::ids::halftimeChance)->load(), Catch::Matchers::WithinAbs (0.15, 1.0e-6));
}

TEST_CASE ("The display says the host isn't playing", "[processor][display]")
{
    LivecutAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;

    processor.processBlock (buffer, midi);

    CHECK (processor.getDisplayState().status.load() == livecut::DisplayState::Status::waitingForHost);
}

TEST_CASE ("The display follows the phrase while cutting", "[processor][display]")
{
    LivecutAudioProcessor processor;
    TestPlayHead playHead;
    playFromBarOne (playHead);
    processor.setPlayHead (&playHead);
    processor.prepareToPlay (48000.0, 512);
    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;

    processor.processBlock (buffer, midi);

    const auto& display = processor.getDisplayState();
    CHECK (display.status.load() == livecut::DisplayState::Status::cutting);
    CHECK (display.phraseBars.load() >= 1);
    CHECK (display.unitsPerBar.load() == 8);
    CHECK (display.blockEnd.load() > display.blockStart.load());
    CHECK (display.position.load() > 0.0);
}

TEST_CASE ("The display says when a bar is too long to cut", "[processor][display]")
{
    LivecutAudioProcessor processor;
    TestPlayHead playHead;
    playFromBarOne (playHead);
    playHead.info.setBpm (20.0);
    processor.setPlayHead (&playHead);
    processor.prepareToPlay (48000.0, 512);
    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;

    processor.processBlock (buffer, midi);

    CHECK (processor.getDisplayState().status.load() == livecut::DisplayState::Status::barTooLong);
}

TEST_CASE ("Saved state identifies the schema and unchanged cutting algorithm", "[processor][state][capture]")
{
    LivecutAudioProcessor processor;
    juce::MemoryBlock state;
    processor.getStateInformation (state);
    auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), static_cast<int> (state.getSize()));
    REQUIRE (xml != nullptr);
    CHECK (xml->getIntAttribute ("schemaVersion") == 3);
    CHECK (xml->getIntAttribute ("algorithmVersion") == 2);
}

TEST_CASE ("Old state resets new controls even on a previously adjusted instance", "[processor][state][capture]")
{
    LivecutAudioProcessor processor;
    juce::MemoryBlock state;
    processor.getStateInformation (state);
    auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), static_cast<int> (state.getSize()));
    REQUIRE (xml != nullptr);
    xml->removeAttribute ("schemaVersion");
    xml->removeAttribute ("algorithmVersion");
    for (const auto* id : { livecut::ids::wet, livecut::ids::outputTrim })
    {
        auto* child = xml->getChildByAttribute ("id", id);
        REQUIRE (child != nullptr);
        xml->removeChildElement (child, true);
        auto* parameter = processor.getParameterState().getParameter (id);
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (id == livecut::ids::wet ? 0.0f : 12.0f));
    }
    juce::AudioProcessor::copyXmlToBinary (*xml, state);
    processor.setStateInformation (state.getData(), static_cast<int> (state.getSize()));
    CHECK (processor.getParameterState().getRawParameterValue (livecut::ids::wet)->load() == 1.0f);
    CHECK (processor.getParameterState().getRawParameterValue (livecut::ids::outputTrim)->load() == 0.0f);
}

TEST_CASE ("Default mixer preserves exact classic output across host buffers", "[processor][capture][mixer]")
{
    LivecutAudioProcessor processor;
    TestPlayHead playHead;
    playFromBarOne (playHead);
    processor.setPlayHead (&playHead);
    processor.prepareToPlay (8000.0, 137);
    const auto procedure = GENERATE (0.0f, 1.0f, 2.0f, 3.0f);
    auto* choice = processor.getParameterState().getParameter (livecut::ids::procedure);
    choice->setValueNotifyingHost (choice->convertTo0to1 (procedure));
    livecut::engine::Cutter classic;
    classic.prepare (8000.0);
    const auto settings = livecut::SettingsReader (processor.getParameterState()).read();
    livecut::applySettings (classic, settings);
    juce::MidiBuffer midi;
    for (int block = 0; block < 500; ++block)
    {
        const auto beats = block * 137.0 / 4000.0;
        const auto bar = static_cast<std::int64_t> (std::floor (beats / 4.0));
        playHead.info.setPpqPosition (beats);
        playHead.info.setPpqPositionOfLastBarStart (static_cast<double> (bar) * 4.0);
        playHead.info.setBarCount (bar);
        juce::AudioBuffer<float> actual (2, 137);
        fillWithTestSignal (actual);
        juce::AudioBuffer<float> expected (actual);
        classic.process (expected.getWritePointer (0), expected.getWritePointer (1), 137, { 120.0, 4.0 }, { bar, beats - static_cast<double> (bar) * 4.0 });
        processor.processBlock (actual, midi);
        REQUIRE (sameAudio (actual, expected));
    }
}

TEST_CASE ("Final output capture spans multiple phrases in one host buffer and survives stop", "[processor][capture]")
{
    LivecutAudioProcessor processor;
    auto& state = processor.getParameterState();
    for (const auto* id : { livecut::ids::minPhraseBars, livecut::ids::maxPhraseBars })
    {
        auto* parameter = state.getParameter (id);
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (1.0f));
    }
    auto* wet = state.getParameter (livecut::ids::wet);
    auto* trim = state.getParameter (livecut::ids::outputTrim);
    wet->setValueNotifyingHost (wet->convertTo0to1 (0.0f));
    trim->setValueNotifyingHost (trim->convertTo0to1 (-6.0f));
    TestPlayHead playHead;
    playFromBarOne (playHead);
    processor.setPlayHead (&playHead);
    processor.prepareToPlay (100.0, 550); // 200 samples/bar, two boundaries inside the host buffer
    juce::AudioBuffer<float> buffer (2, 550);
    fillWithTestSignal (buffer);
    juce::MidiBuffer midi;
    processor.processBlock (buffer, midi);
    auto phrase = processor.getCaptureService().rolling().acquireLatest();
    REQUIRE (phrase);
    REQUIRE (phrase.channel (0).size() == 200);
    for (int i = 0; i < 200; ++i)
    {
        REQUIRE (phrase.channel (0)[static_cast<std::size_t> (i)] == buffer.getSample (0, 200 + i));
        REQUIRE (phrase.channel (1)[static_cast<std::size_t> (i)] == buffer.getSample (1, 200 + i));
    }
    phrase = {};
    playHead.info.setIsPlaying (false);
    fillWithTestSignal (buffer);
    processor.processBlock (buffer, midi);
    CHECK (processor.getCaptureService().rolling().hasLatest());
    CHECK_THAT (buffer.getSample (0, 100), Catch::Matchers::WithinAbs (std::sin (1.0f) * juce::Decibels::decibelsToGain (-6.0f), 1.0e-6));
    auto stopped = processor.getCaptureService().rolling().acquireLatest();
    REQUIRE (stopped);
    CHECK (stopped.channel (0).size() == 200);
}

TEST_CASE ("A phrase completed at the buffer end is retained before stop or loop", "[processor][capture][exact-end]")
{
    for (const auto blockSize : { 1, 73, 200 })
    {
        INFO (blockSize);
        LivecutAudioProcessor processor;
        auto& state = processor.getParameterState();
        for (const auto* id : { livecut::ids::minPhraseBars, livecut::ids::maxPhraseBars })
        {
            auto* parameter = state.getParameter (id);
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (1.0f));
        }
        TestPlayHead playHead;
        playFromBarOne (playHead);
        processor.setPlayHead (&playHead);
        processor.prepareToPlay (100.0, blockSize);
        juce::MidiBuffer midi;
        for (int done = 0; done < 200; done += blockSize)
        {
            const auto count = std::min (blockSize, 200 - done);
            playHead.info.setPpqPosition (done / 50.0);
            juce::AudioBuffer<float> buffer (2, count);
            fillWithTestSignal (buffer);
            processor.processBlock (buffer, midi);
        }
        CHECK (processor.getCaptureService().rolling().hasLatest());
        juce::AudioBuffer<float> buffer (2, 32);
        playHead.info.setIsPlaying (false);
        processor.processBlock (buffer, midi);
        auto stopped = processor.getCaptureService().rolling().acquireLatest();
        REQUIRE (stopped);
        CHECK (stopped.channel (0).size() == 200);
        stopped = {};
        playHead.info.setIsPlaying (true);
        playHead.info.setPpqPosition (0.25); // loop/seek into a partial phrase
        processor.processBlock (buffer, midi);
        auto looped = processor.getCaptureService().rolling().acquireLatest();
        REQUIRE (looped);
        CHECK (looped.channel (0).size() == 200);
    }
}

TEST_CASE ("Processing with mix capture boundaries and a leased export never allocates", "[processor][capture][allocations]")
{
    LivecutAudioProcessor processor;
    auto& state = processor.getParameterState();
    for (const auto* id : { livecut::ids::minPhraseBars, livecut::ids::maxPhraseBars })
    {
        auto* parameter = state.getParameter (id);
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (1.0f));
    }
    processor.prepareToPlay (1000.0, 127);
    TestPlayHead playHead;
    playFromBarOne (playHead);
    processor.setPlayHead (&playHead);
    juce::AudioBuffer<float> buffer (2, 127);
    fillWithTestSignal (buffer);
    juce::MidiBuffer midi;
    livecut::PhraseCapture::Lease exportLease;
    std::size_t allocationCount = 0;
    for (int block = 0; block < 100; ++block)
    {
        const auto ppq = block * 127.0 / 500.0;
        playHead.info.setPpqPosition (ppq);
        playHead.info.setPpqPositionOfLastBarStart (std::floor (ppq / 4.0) * 4.0);
        playHead.info.setBarCount (static_cast<std::int64_t> (ppq / 4.0));
        if (block == 25)
            exportLease = processor.getCaptureService().rolling().acquireLatest();
        const livecut::engine::testing::AllocationCounter allocations;
        processor.processBlock (buffer, midi);
        allocationCount += allocations.count();
    }
    CHECK (allocationCount == 0);
    CHECK (processor.getCaptureService().rolling().status() == livecut::PhraseCapture::Status::busy);
    REQUIRE (exportLease);
    CHECK (exportLease.channel (0).size() == 2000);
}

TEST_CASE ("Capture follows real-rate tempo changes and discards meter-interrupted candidates", "[processor][capture][timing]")
{
    const auto rate = GENERATE (44100, 48000, 96000);
    LivecutAudioProcessor processor;
    auto& state = processor.getParameterState();
    for (const auto* id : { livecut::ids::minPhraseBars, livecut::ids::maxPhraseBars })
    {
        auto* parameter = state.getParameter (id);
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (1.0f));
    }
    TestPlayHead playHead;
    playFromBarOne (playHead);
    processor.setPlayHead (&playHead);
    processor.prepareToPlay (rate, 511);
    juce::MidiBuffer midi;
    double ppq = 0.0;
    std::array<std::vector<float>, 2> heard;
    auto render = [&] (int count, double bpm, int meter, double meterOrigin, int originBar, bool remember)
    {
        for (int done = 0; done < count; done += 511)
        {
            const auto frames = std::min (511, count - done);
            const auto bars = std::floor ((ppq - meterOrigin) / meter);
            playHead.info.setBpm (bpm);
            playHead.info.setTimeSignature (juce::AudioPlayHead::TimeSignature { meter, 4 });
            playHead.info.setPpqPosition (ppq);
            playHead.info.setPpqPositionOfLastBarStart (meterOrigin + bars * meter);
            playHead.info.setBarCount (originBar + static_cast<std::int64_t> (bars));
            juce::AudioBuffer<float> buffer (2, frames);
            fillWithTestSignal (buffer);
            processor.processBlock (buffer, midi);
            if (remember)
                for (std::size_t channel = 0; channel < 2; ++channel)
                    heard[channel].insert (heard[channel].end(), buffer.getReadPointer (static_cast<int> (channel)),
                                          buffer.getReadPointer (static_cast<int> (channel)) + frames);
            ppq += frames * bpm / 60.0 / rate;
        }
    };
    render (rate, 120.0, 4, 0.0, 0, true); // first two beats
    render (rate * 4 / 3, 90.0, 4, 0.0, 0, true); // remaining two beats at the new tempo
    auto complete = processor.getCaptureService().rolling().acquireLatest();
    REQUIRE (complete);
    REQUIRE (complete.channel (0).size() == heard[0].size());
    CHECK (complete.sampleRate() == rate);
    for (std::size_t channel = 0; channel < 2; ++channel)
        CHECK (std::memcmp (complete.channel (channel).data(), heard[channel].data(), heard[channel].size() * sizeof (float)) == 0);
    complete = {};

    ppq = 4.0;
    render (rate / 2, 120.0, 4, 0.0, 0, false); // first beat of the next candidate
    ppq = 5.0;
    render (rate, 120.0, 3, 4.0, 1, false); // meter changes mid-phrase; discard the candidate
    auto retained = processor.getCaptureService().rolling().acquireLatest();
    REQUIRE (retained);
    CHECK (retained.channel (0).size() == heard[0].size());
    retained = {};
    ppq = 7.0;
    render (rate * 3 / 2, 120.0, 3, 4.0, 1, false); // next whole 3/4 phrase
    auto newMeter = processor.getCaptureService().rolling().acquireLatest();
    REQUIRE (newMeter);
    CHECK (newMeter.channel (0).size() == static_cast<std::size_t> (rate * 3 / 2));
}

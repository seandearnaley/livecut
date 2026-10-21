// SPDX-License-Identifier: GPL-3.0-or-later

#include "PluginProcessor.h"
#include "engine/AllocationCounter.h"
#include "engine/PerformanceEngine.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

namespace
{
using Action = LivecutAudioProcessor::PerformanceAction;
using PerformanceState = livecut::engine::PerformanceEngine::State;
constexpr std::array axisIds { "performanceVariation", "performanceDensity", "performanceGrain", "performanceCharacter" };
constexpr std::array newIds { "performanceEnabled", "performanceVariation", "performanceDensity", "performanceGrain",
                              "performanceCharacter", "performanceFill", "performanceSeed" };

void setPerformanceValue (LivecutAudioProcessor& processor, const char* id, float value)
{
    auto* parameter = processor.getParameterState().getParameter (id);
    REQUIRE (parameter != nullptr);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

float performanceValue (LivecutAudioProcessor& processor, const char* id)
{
    const auto* parameter = processor.getParameterState().getRawParameterValue (id);
    REQUIRE (parameter != nullptr);
    return parameter->load();
}

void setAxes (LivecutAudioProcessor& processor, const std::array<float, 4>& axes)
{
    for (std::size_t i = 0; i < axes.size(); ++i)
        setPerformanceValue (processor, axisIds[i], axes[i]);
}

struct PerformanceHead final : juce::AudioPlayHead
{
    juce::Optional<PositionInfo> getPosition() const override { return info; }
    void at (std::int64_t frame, double rate, bool running = true, double bpm = 120.0)
    {
        const auto ppq = static_cast<double> (frame) * bpm / (60.0 * rate);
        const auto bar = static_cast<std::int64_t> (std::floor (ppq / 4.0));
        info.setIsPlaying (running);
        info.setBpm (bpm);
        info.setTimeSignature (TimeSignature { 4, 4 });
        info.setPpqPosition (ppq);
        info.setPpqPositionOfLastBarStart (static_cast<double> (bar) * 4.0);
        info.setBarCount (bar);
    }
    PositionInfo info;
};

using Audio = std::array<std::vector<float>, 2>;

Audio signal (int frames)
{
    Audio result;
    for (auto& channel : result)
        channel.resize (static_cast<std::size_t> (frames));
    for (int frame = 0; frame < frames; ++frame)
    {
        result[0][static_cast<std::size_t> (frame)] = .21f * std::sin (static_cast<float> (frame) * .071f);
        result[1][static_cast<std::size_t> (frame)] = .13f * std::cos (static_cast<float> (frame) * .037f);
    }
    return result;
}

struct Gesture
{
    int frame = 0;
    std::optional<std::array<float, 4>> axes;
    std::optional<Action> action;
    int fill = -1;
};

// Actions are delivered at the same physical sample for every partition. The
// callback under test contains neither test-buffer allocation nor host edits.
Audio renderPerformance (LivecutAudioProcessor& processor, const Audio& input, double rate,
                         std::span<const int> blocks, std::span<const Gesture> gestures = {},
                         bool bypass = false, std::int64_t hostStart = 0, double bpm = 120.0)
{
    REQUIRE (! blocks.empty());
    REQUIRE (input[0].size() == input[1].size());
    REQUIRE (input[0].size() <= static_cast<std::size_t> (std::numeric_limits<int>::max()));
    PerformanceHead head;
    processor.setPlayHead (&head);
    Audio output;
    for (auto& channel : output)
        channel.resize (input[0].size());
    juce::MidiBuffer midi;
    std::size_t block = 0, event = 0;
    const auto total = static_cast<int> (input[0].size());
    for (int first = 0; first < total;)
    {
        while (event < gestures.size() && gestures[event].frame == first)
        {
            const auto& gesture = gestures[event++];
            if (gesture.axes)
                setAxes (processor, *gesture.axes);
            if (gesture.fill >= 0)
                setPerformanceValue (processor, "performanceFill", static_cast<float> (gesture.fill));
            if (gesture.action)
                processor.requestPerformanceAction (*gesture.action);
        }
        auto count = std::min (blocks[block++ % blocks.size()], total - first);
        REQUIRE (count > 0);
        if (event < gestures.size())
        {
            REQUIRE (gestures[event].frame > first);
            count = std::min (count, gestures[event].frame - first);
        }
        head.at (hostStart + first, rate, true, bpm);
        juce::AudioBuffer<float> buffer (2, count);
        for (int channel = 0; channel < 2; ++channel)
            std::copy_n (input[static_cast<std::size_t> (channel)].data() + first, count, buffer.getWritePointer (channel));
        std::size_t allocations = 0;
        {
            const livecut::engine::testing::AllocationCounter counter;
            if (bypass)
                processor.processBlockBypassed (buffer, midi);
            else
                processor.processBlock (buffer, midi);
            allocations = counter.count();
        }
        REQUIRE (allocations == 0);
        for (int channel = 0; channel < 2; ++channel)
            std::copy_n (buffer.getReadPointer (channel), count, output[static_cast<std::size_t> (channel)].data() + first);
        first += count;
    }
    processor.setPlayHead (nullptr);
    return output;
}

void preparePerformance (LivecutAudioProcessor& processor, double rate, int block)
{
    setPerformanceValue (processor, "performanceEnabled", 1);
    processor.setNonRealtime (true);
    processor.prepareToPlay (rate, block);
    REQUIRE (processor.modeState() == LivecutAudioProcessor::ModeState::active);
    REQUIRE (processor.activeMode() == livecut::ProcessingMode::perform);
    REQUIRE_FALSE (processor.activeSpectral());
    REQUIRE (processor.reportedLatencySamples() == 0);
}

bool equalAudio (const Audio& first, const Audio& second, std::size_t start = 0)
{
    for (std::size_t channel = 0; channel < 2; ++channel)
    {
        if (first[channel].size() != second[channel].size() || start > first[channel].size())
            return false;
        if (std::memcmp (first[channel].data() + start, second[channel].data() + start,
                         (first[channel].size() - start) * sizeof (float)) != 0)
            return false;
    }
    return true;
}
} // namespace

TEST_CASE ("Performance controls append without changing the saved Live Studio mode contract", "[processor][performance][parameters]")
{
    LivecutAudioProcessor processor;
    auto& state = processor.getParameterState();
    REQUIRE (state.getParameter ("mode") != nullptr);
    CHECK (state.getParameter ("mode")->getNumSteps() == 2);
    CHECK (performanceValue (processor, "mode") == 0);
    CHECK (performanceValue (processor, "performanceEnabled") == 0);
    CHECK_FALSE (state.getParameter ("performanceEnabled")->isAutomatable());
    std::array<int, newIds.size()> indices {};
    for (std::size_t i = 0; i < newIds.size(); ++i)
    {
        INFO (newIds[i]);
        const auto* parameter = state.getParameter (newIds[i]);
        REQUIRE (parameter != nullptr);
        indices[i] = parameter->getParameterIndex();
    }
    for (std::size_t i = 0; i < indices.size(); ++i)
        CHECK (indices[i] == 106 + static_cast<int> (i));
    CHECK (processor.getParameters().size() == 113);
    constexpr std::array defaults { 0.0f, .5f, 0.0f, .5f };
    for (std::size_t i = 0; i < defaults.size(); ++i)
        CHECK (performanceValue (processor, axisIds[i]) == defaults[i]);
    CHECK (performanceValue (processor, "performanceSeed") == 1);
    CHECK (performanceValue (processor, "performanceFill") == 0);
}

TEST_CASE ("Performance targets remain inert in the legacy Live renderer", "[processor][performance][legacy]")
{
    constexpr int rate = 8000;
    const auto input = signal (rate * 6);
    constexpr std::array blocks { 137, 512 };
    LivecutAudioProcessor reference, adjusted;
    setAxes (adjusted, { .91f, .13f, 1.0f, .02f });
    setPerformanceValue (adjusted, "performanceSeed", 65535);
    setPerformanceValue (adjusted, "performanceFill", 1);
    for (auto* processor : { &reference, &adjusted })
    {
        processor->setNonRealtime (true);
        processor->prepareToPlay (rate, 512);
        REQUIRE (processor->activeMode() == livecut::ProcessingMode::live);
    }
    const auto expected = renderPerformance (reference, input, rate, blocks);
    const auto actual = renderPerformance (adjusted, input, rate, blocks);
    CHECK (equalAudio (actual, expected));
}

TEST_CASE ("Performance is zero latency and incompatible Spectral requests fail explicitly", "[processor][performance][prepare]")
{
    const auto rate = GENERATE (8000, 44100, 48000, 96000);
    LivecutAudioProcessor processor;
    preparePerformance (processor, rate, 137);
    CHECK (processor.getLatencySamples() == 0);
    CHECK (processor.spectralAddedLatencySamples() == 0);
    CHECK (processor.performanceStatusText().isNotEmpty());

    LivecutAudioProcessor incompatible;
    setPerformanceValue (incompatible, "performanceEnabled", 1);
    setPerformanceValue (incompatible, "resonanceSpectralEnabled", 1);
    incompatible.prepareToPlay (rate, 137);
    CHECK (incompatible.modeState() == LivecutAudioProcessor::ModeState::error);
    CHECK_FALSE (incompatible.activeSpectral());
    CHECK (incompatible.reportedLatencySamples() == 0);
    CHECK (incompatible.modeStatusText().containsIgnoreCase ("Spectral"));
}

TEST_CASE ("Prepare Performance edits the public configuration and respects playing transport", "[processor][performance][apply]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    processor.prepareToPlay (8000, 64);
    PerformanceHead head;
    processor.setPlayHead (&head);
    head.at (0, 8000);
    juce::AudioBuffer<float> buffer (2, 64);
    buffer.clear();
    juce::MidiBuffer midi;
    processor.processBlock (buffer, midi);
    setPerformanceValue (processor, "resonanceSpectralEnabled", 1);
    processor.preparePerformanceMode();
    CHECK (performanceValue (processor, "performanceEnabled") == 1);
    CHECK (performanceValue (processor, "resonanceSpectralEnabled") == 0);
    CHECK (processor.activeMode() == livecut::ProcessingMode::live);
    CHECK (processor.modeState() == LivecutAudioProcessor::ModeState::pendingPlaying);
    head.at (64, 8000, false);
    processor.processBlock (buffer, midi);
    processor.applyRequestedMode();
    CHECK (processor.activeMode() == livecut::ProcessingMode::perform);
    CHECK_FALSE (processor.activeSpectral());
    CHECK (processor.reportedLatencySamples() == 0);
}

TEST_CASE ("Keep Return and Mutate expose their complete targets and survive saved home recall", "[processor][performance][state][actions]")
{
    constexpr std::array home { .23f, .74f, .31f, .62f };
    LivecutAudioProcessor original;
    setAxes (original, home);
    setPerformanceValue (original, "performanceSeed", 73);
    original.requestPerformanceAction (Action::keep);
    setAxes (original, { .91f, .08f, .83f, .12f });
    original.requestPerformanceAction (Action::mutate);
    CHECK (performanceValue (original, "performanceSeed") == 74);
    juce::MemoryBlock saved;
    original.getStateInformation (saved);
    const auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), static_cast<int> (saved.getSize()));
    REQUIRE (xml != nullptr);
    CHECK (xml->getIntAttribute ("schemaVersion") == 9);
    CHECK (xml->getIntAttribute ("algorithmVersion") == 2);

    LivecutAudioProcessor restored;
    restored.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
    for (const auto* id : axisIds)
        CHECK (performanceValue (restored, id) == Catch::Approx (performanceValue (original, id)).margin (1.0e-6f));
    CHECK (performanceValue (restored, "performanceSeed") == 74);
    restored.requestPerformanceAction (Action::returnToKept);
    for (std::size_t i = 0; i < home.size(); ++i)
        CHECK (performanceValue (restored, axisIds[i]) == Catch::Approx (home[i]).margin (1.0e-6f));
    CHECK (performanceValue (restored, "performanceSeed") == 73);
    setPerformanceValue (restored, "performanceSeed", 65535);
    restored.requestPerformanceAction (Action::mutate);
    CHECK (performanceValue (restored, "performanceSeed") == 1);
}

TEST_CASE ("Pre Performance state clears adjusted new controls and retains legacy Studio", "[processor][performance][state]")
{
    LivecutAudioProcessor processor;
    setPerformanceValue (processor, "mode", 1);
    setPerformanceValue (processor, "seed", 13);
    juce::MemoryBlock saved;
    processor.getStateInformation (saved);
    auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), static_cast<int> (saved.getSize()));
    REQUIRE (xml != nullptr);
    xml->setAttribute ("schemaVersion", 8);
    for (const auto* id : newIds)
    {
        auto* child = xml->getChildByAttribute ("id", id);
        REQUIRE (child != nullptr);
        xml->removeChildElement (child, true);
        auto* parameter = processor.getParameterState().getParameter (id);
        parameter->setValueNotifyingHost (parameter->getDefaultValue() < .5f ? 1.0f : 0.0f);
    }
    juce::AudioProcessor::copyXmlToBinary (*xml, saved);
    processor.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
    for (const auto* id : newIds)
    {
        const auto* parameter = processor.getParameterState().getParameter (id);
        CHECK (parameter->getValue() == Catch::Approx (parameter->getDefaultValue()).margin (1.0e-6f));
    }
    CHECK (performanceValue (processor, "mode") == 1);
    CHECK (performanceValue (processor, "seed") == 13);
}

TEST_CASE ("Performance monitors its first full bar and retains that source until requested capture", "[processor][performance][source]")
{
    constexpr int rate = 8000, bar = 16000;
    auto firstInput = signal (3 * bar + 1), differentFuture = firstInput;
    for (std::size_t channel = 0; channel < 2; ++channel)
        std::fill (differentFuture[channel].begin() + bar, differentFuture[channel].end(), channel == 0 ? .61f : -.37f);
    LivecutAudioProcessor first, second;
    preparePerformance (first, rate, 512);
    preparePerformance (second, rate, 512);
    constexpr std::array blocks { 137, 512 };
    const auto expected = renderPerformance (first, firstInput, rate, blocks);
    const auto actual = renderPerformance (second, differentFuture, rate, blocks);
    for (std::size_t channel = 0; channel < 2; ++channel)
        CHECK (std::memcmp (actual[channel].data(), differentFuture[channel].data(), bar * sizeof (float)) == 0);
    CHECK (equalAudio (actual, expected, bar));
    CHECK (second.getDisplayState().performanceHasSource.load());
    CHECK (second.getDisplayState().performanceState.load() == PerformanceState::playing);
}

TEST_CASE ("Performance output Capture contains the actual heard mixed and trimmed bar", "[processor][performance][capture]")
{
    constexpr int rate = 8000, bar = 16000;
    LivecutAudioProcessor processor;
    setAxes (processor, { .8f, .72f, .6f, .39f });
    setPerformanceValue (processor, "wet", .37f);
    setPerformanceValue (processor, "outputTrim", -6);
    preparePerformance (processor, rate, 512);
    constexpr std::array blocks { 137, 512 };
    // Completion belongs to the exclusive end, even when no following callback
    // or next-bar sample arrives to publish the phrase.
    const auto heard = renderPerformance (processor, signal (3 * bar), rate, blocks);
    auto phrase = processor.getCaptureService().rolling().acquireLatest();
    REQUIRE (phrase);
    CHECK (phrase.sampleRate() == rate);
    REQUIRE (phrase.channel (0).size() == bar);
    REQUIRE (phrase.channel (1).size() == bar);
    for (std::size_t channel = 0; channel < 2; ++channel)
        CHECK (std::memcmp (phrase.channel (channel).data(), heard[channel].data() + 2 * bar, bar * sizeof (float)) == 0);
    CHECK_FALSE (phrase.spectralWarmup());
}

TEST_CASE ("Grab replaces the source only after the requested next full bar completes", "[processor][performance][source][actions]")
{
    constexpr int rate = 8000, bar = 16000;
    auto input = signal (5 * bar + 1);
    for (std::size_t channel = 0; channel < 2; ++channel)
        std::fill (input[channel].begin() + bar, input[channel].end(), channel == 0 ? .61f : -.37f);
    LivecutAudioProcessor reference, recaptured;
    preparePerformance (reference, rate, 512);
    preparePerformance (recaptured, rate, 512);
    constexpr std::array blocks { 137, 512 };
    const std::array gestures { Gesture { 2 * bar + 7, {}, Action::capture, -1 } };
    const auto oldSource = renderPerformance (reference, input, rate, blocks);
    const auto newSource = renderPerformance (recaptured, input, rate, blocks, gestures);
    for (std::size_t channel = 0; channel < 2; ++channel)
        CHECK (std::memcmp (oldSource[channel].data(), newSource[channel].data(), 4 * bar * sizeof (float)) == 0);
    CHECK_FALSE (equalAudio (oldSource, newSource, 4 * bar));
    CHECK_FALSE (recaptured.getDisplayState().performanceCaptureArmed.load());
    CHECK (recaptured.getDisplayState().performanceState.load() == PerformanceState::playing);
}

TEST_CASE ("Performance gestures and actions render identically across host partitions", "[processor][performance][partition]")
{
    const auto bpm = GENERATE (120.0, 174.0);
    constexpr int rate = 8000;
    const auto bar = static_cast<int> (std::llround (rate * 240.0 / bpm));
    const auto input = signal (static_cast<int> (std::ceil (6 * rate * 240.0 / bpm)) + 1);
    const std::array gestures {
        Gesture { bar + 31, std::array { .78f, .83f, .61f, .18f }, {}, -1 },
        Gesture { 2 * bar - 17, {}, Action::keep, -1 },
        Gesture { 2 * bar + 41, std::array { .22f, .29f, .91f, .82f }, Action::mutate, 1 },
        Gesture { 3 * bar + 47, {}, {}, 0 },
        Gesture { 4 * bar - 13, {}, Action::returnToKept, -1 },
        Gesture { 4 * bar + 7, {}, Action::capture, -1 }
    };
    constexpr std::array referenceBlocks { 1 };
    LivecutAudioProcessor reference;
    preparePerformance (reference, rate, 512);
    const auto expected = renderPerformance (reference, input, rate, referenceBlocks, gestures, false, 0, bpm);
    for (const auto blocks : { std::vector { 64 }, std::vector { 137 }, std::vector { 512 }, std::vector { 137, 512 } })
    {
        LivecutAudioProcessor processor;
        preparePerformance (processor, rate, 512);
        const auto actual = renderPerformance (processor, input, rate, blocks, gestures, false, 0, bpm);
        double maximumDelta = 0;
        auto firstMismatch = input[0].size();
        for (std::size_t channel = 0; channel < 2; ++channel)
            for (std::size_t frame = 0; frame < actual[channel].size(); ++frame)
            {
                maximumDelta = std::max (maximumDelta, std::abs (static_cast<double> (actual[channel][frame]) - expected[channel][frame]));
                if (std::memcmp (&actual[channel][frame], &expected[channel][frame], sizeof (float)) != 0)
                    firstMismatch = std::min (firstMismatch, frame);
            }
        INFO ("BPM " << bpm << "; first block " << blocks.front() << "; first mismatch frame " << firstMismatch
                    << "; maximum absolute PCM delta " << maximumDelta);
        CHECK (equalAudio (actual, expected));
        CHECK (processor.getDisplayState().performanceState.load() == reference.getDisplayState().performanceState.load());
    }
}

TEST_CASE ("Performance bypass is immediate unity and stop and seek discard old source", "[processor][performance][reset]")
{
    constexpr int rate = 8000, bar = 16000;
    constexpr std::array blocks { 137, 512 };
    LivecutAudioProcessor processor;
    setAxes (processor, { .9f, .8f, .7f, .6f });
    setPerformanceValue (processor, "wet", .17f);
    setPerformanceValue (processor, "outputTrim", -12);
    preparePerformance (processor, rate, 512);
    const auto input = signal (2 * bar + 1);
    const auto bypassed = renderPerformance (processor, input, rate, blocks, {}, true);
    CHECK (equalAudio (bypassed, input));

    // Resume at an explicit new timeline location, then populate a fresh source.
    setPerformanceValue (processor, "wet", 1);
    setPerformanceValue (processor, "outputTrim", 0);
    renderPerformance (processor, signal (2 * bar + 1), rate, blocks, {}, false, 8 * bar);
    REQUIRE (processor.getDisplayState().performanceHasSource.load());
    PerformanceHead head;
    processor.setPlayHead (&head);
    head.at (10 * bar + 1, rate, false);
    juce::AudioBuffer<float> buffer (2, 64);
    juce::MidiBuffer midi;
    juce::FloatVectorOperations::fill (buffer.getWritePointer (0), .38f, 64);
    juce::FloatVectorOperations::fill (buffer.getWritePointer (1), -.17f, 64);
    processor.processBlock (buffer, midi);
    CHECK_FALSE (processor.getDisplayState().performanceHasSource.load());
    CHECK (processor.getDisplayState().performanceState.load() == PerformanceState::waitingForHost);
    CHECK (buffer.getSample (0, 31) == .38f);
    CHECK (buffer.getSample (1, 31) == -.17f);
    head.at (17 * bar, rate);
    processor.processBlock (buffer, midi);
    CHECK_FALSE (processor.getDisplayState().performanceHasSource.load());
    CHECK (processor.getDisplayState().performanceState.load() == PerformanceState::capturing);
}

TEST_CASE ("Leaving Performance restores the saved legacy mode after stopped Apply and reprepare", "[processor][performance][apply][lifecycle]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    const auto legacyMode = GENERATE (0, 1);
    constexpr int rate = 8000, bar = 16000;
    LivecutAudioProcessor processor;
    setPerformanceValue (processor, "mode", static_cast<float> (legacyMode));
    setPerformanceValue (processor, "wet", 0);
    preparePerformance (processor, rate, 512);
    constexpr std::array blocks { 137, 512 };
    renderPerformance (processor, signal (2 * bar + 1), rate, blocks);
    processor.leavePerformanceMode();
    CHECK (performanceValue (processor, "performanceEnabled") == 0);
    CHECK (performanceValue (processor, "mode") == static_cast<float> (legacyMode));
    CHECK (processor.activeMode() == livecut::ProcessingMode::perform);
    CHECK (processor.modeState() == LivecutAudioProcessor::ModeState::pendingPlaying);
    PerformanceHead head;
    processor.setPlayHead (&head);
    head.at (2 * bar + 1, rate, false);
    juce::AudioBuffer<float> buffer (2, 64);
    buffer.clear();
    juce::MidiBuffer midi;
    processor.processBlock (buffer, midi);
    processor.applyRequestedMode();
    const auto expectedMode = legacyMode == 0 ? livecut::ProcessingMode::live : livecut::ProcessingMode::studio;
    REQUIRE (processor.activeMode() == expectedMode);
    CHECK (processor.reportedLatencySamples() == legacyMode * 4000);
    CHECK_FALSE (processor.activeSpectral());
    processor.prepareToPlay (rate, 512);
    CHECK (processor.activeMode() == expectedMode);
    const auto delay = legacyMode * 4000;
    Audio impulse;
    for (auto& channel : impulse)
        channel.resize (static_cast<std::size_t> (delay + 512));
    impulse[0][37] = .75f;
    impulse[1][37] = -.25f;
    const auto output = renderPerformance (processor, impulse, rate, blocks);
    Audio expected;
    for (auto& channel : expected)
        channel.resize (impulse[0].size());
    expected[0][static_cast<std::size_t> (delay + 37)] = .75f;
    expected[1][static_cast<std::size_t> (delay + 37)] = -.25f;
    CHECK (equalAudio (output, expected));
}

TEST_CASE ("Active state recall and sample rate reprepare discard captured Performance source", "[processor][performance][state][lifecycle]")
{
    constexpr int rate = 8000, bar = 16000;
    LivecutAudioProcessor processor;
    preparePerformance (processor, rate, 512);
    constexpr std::array blocks { 137, 512 };
    renderPerformance (processor, signal (2 * bar + 1), rate, blocks);
    REQUIRE (processor.getDisplayState().performanceHasSource.load());
    juce::MemoryBlock saved;
    processor.getStateInformation (saved);
    setAxes (processor, { .91f, .82f, .73f, .64f });
    processor.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
    Audio raw;
    raw[0].assign (64, .51f);
    raw[1].assign (64, -.27f);
    const auto monitored = renderPerformance (processor, raw, rate, blocks, {}, false, 2 * bar + 1);
    CHECK (equalAudio (raw, monitored));
    CHECK_FALSE (processor.getDisplayState().performanceHasSource.load());
    CHECK (processor.getDisplayState().performanceState.load() == PerformanceState::waitingForBar);
    CHECK (performanceValue (processor, "performanceVariation") == 0);
    raw[0].assign (2 * bar - 64, .51f);
    raw[1].assign (2 * bar - 64, -.27f);
    renderPerformance (processor, raw, rate, blocks, {}, false, 2 * bar + 65);
    REQUIRE (processor.getDisplayState().performanceHasSource.load());
    processor.prepareToPlay (16000, 512);
    raw[0].assign (64, -.19f);
    raw[1].assign (64, .42f);
    const auto reprepared = renderPerformance (processor, raw, 16000, blocks);
    CHECK (equalAudio (raw, reprepared));
    CHECK_FALSE (processor.getDisplayState().performanceHasSource.load());
    CHECK (processor.getDisplayState().performanceState.load() == PerformanceState::capturing);
    CHECK (processor.getLatencySamples() == 0);
}

TEST_CASE ("A one sample host seek invalidates Performance source within the legacy epoch tolerance", "[processor][performance][reset][precision]")
{
    constexpr int rate = 8000, bar = 16000;
    constexpr std::array blocks { 137, 512 };
    LivecutAudioProcessor processor;
    preparePerformance (processor, rate, 512);
    renderPerformance (processor, signal (2 * bar + 1), rate, blocks);
    REQUIRE (processor.getDisplayState().performanceHasSource.load());
    const auto raw = signal (64);
    // Physical input continues at 2*bar+1. Host PPQ moves one sample farther,
    // below the legacy .001 PPQ epoch threshold, but above half a sample.
    const auto monitored = renderPerformance (processor, raw, rate, blocks, {}, false, 2 * bar + 2);
    CHECK (equalAudio (raw, monitored));
    CHECK_FALSE (processor.getDisplayState().performanceHasSource.load());
    CHECK (processor.getDisplayState().performanceState.load() == PerformanceState::waitingForBar);
}

namespace
{
float fixtureNoise (std::uint32_t frame)
{
    frame ^= frame >> 16;
    frame *= 0x7feb352du;
    frame ^= frame >> 15;
    frame *= 0x846ca68bu;
    frame ^= frame >> 16;
    return static_cast<float> (frame & 0x00ffffffu) / 8388607.5f - 1.0f;
}

// Original procedural percussion, not a recording or an artist imitation.
// A two-bar pattern includes syncopated kicks, ghost snares and swung hats.
Audio demonstrationDrums (int rate, double bpm, int bars)
{
    const auto sixteenth = rate * 60.0 / bpm / 4.0;
    const auto frames = static_cast<int> (std::llround (sixteenth * 16 * bars));
    Audio audio;
    for (auto& channel : audio)
        channel.resize (static_cast<std::size_t> (frames));
    constexpr std::array kick { 1.0f, 0.f, 0.f, 0.f, 0.f, 0.f, .73f, 0.f,
                               0.f, 0.f, .62f, 0.f, 0.f, 0.f, 0.f, 0.f,
                               1.0f, 0.f, 0.f, .65f, 0.f, 0.f, 0.f, 0.f,
                               0.f, 0.f, .78f, 0.f, 0.f, 0.f, 0.f, 0.f };
    constexpr std::array snare { 0.f, 0.f, 0.f, 0.f, .92f, 0.f, 0.f, 0.f,
                                0.f, 0.f, 0.f, .22f, 1.0f, 0.f, 0.f, .29f,
                                0.f, 0.f, 0.f, 0.f, 1.0f, 0.f, 0.f, .26f,
                                0.f, 0.f, 0.f, 0.f, .94f, 0.f, .19f, 0.f };
    for (int step = 0; step < bars * 16; ++step)
    {
        const auto pattern = static_cast<std::size_t> (step % 32);
        const auto swung = (step % 2 != 0 ? .035 : 0.0) * sixteenth;
        const auto first = static_cast<int> (std::llround (step * sixteenth + swung));
        const auto open = step % 16 == 7 || step % 16 == 15;
        const auto hat = step % 2 == 0 ? .15f : (step % 4 == 3 ? .085f : .045f);
        const auto leftHat = step % 4 == 0 ? .78f : .43f;
        const auto rightHat = 1.21f - leftHat;
        for (int age = 0; age < rate / 4 && first + age < frames; ++age)
        {
            const auto time = static_cast<double> (age) / rate;
            const auto noiseId = static_cast<std::uint32_t> (first + age + 1);
            const auto white = fixtureNoise (noiseId);
            const auto bright = .5 * (white - fixtureNoise (noiseId - 1));
            const auto kickPhase = 2 * std::numbers::pi * (46 * time + 2.2 * (1 - std::exp (-time / .025)));
            const auto bass = kick[pattern] * (.58 * std::sin (kickPhase) * std::exp (-time / .073)
                                              + .12 * bright * std::exp (-time / .0013));
            const auto snareBody = snare[pattern] * (.19 * std::sin (2 * std::numbers::pi * 183 * time) * std::exp (-time / .035)
                                                    + .29 * white * std::exp (-time / .045));
            const auto metal = hat * bright * std::exp (-time / (open ? .064 : .012));
            const auto index = static_cast<std::size_t> (first + age);
            audio[0][index] += static_cast<float> (bass + snareBody + metal * leftHat);
            audio[1][index] += static_cast<float> (bass + snareBody + metal * rightHat);
        }
    }
    return audio;
}

struct AudioLevels
{
    double rms = 0.0, peak = 0.0, dc = 0.0, derivativePeak = 0.0;
};

AudioLevels audioLevels (const Audio& audio, std::size_t first)
{
    REQUIRE (first < audio[0].size());
    AudioLevels result;
    double energy = 0.0, sum = 0.0;
    bool finite = true;
    for (const auto& channel : audio)
        for (auto frame = first; frame < channel.size(); ++frame)
        {
            const auto value = static_cast<double> (channel[frame]);
            finite &= std::isfinite (value);
            energy += value * value;
            sum += value;
            result.peak = std::max (result.peak, std::abs (value));
            if (frame > first)
                result.derivativePeak = std::max (result.derivativePeak, std::abs (value - channel[frame - 1]));
        }
    const auto count = static_cast<double> (2 * (audio[0].size() - first));
    REQUIRE (finite);
    result.rms = std::sqrt (energy / count);
    result.dc = sum / count;
    return result;
}

juce::var object()
{
    return juce::var (new juce::DynamicObject());
}

const char* actionName (Action action)
{
    switch (action)
    {
        case Action::capture: return "capture_next_full_bar";
        case Action::keep: return "keep_public_targets";
        case Action::mutate: return "mutate_seed_next_bar";
        case Action::returnToKept: return "return_public_targets";
    }
    return "unknown";
}

std::vector<Gesture> demonstrationGestures (const juce::String& profile, double framesPerBar)
{
    std::vector<Gesture> gestures;
    if (profile == "neutral")
        return gestures;
    if (profile == "actions")
    {
        const auto frame = [framesPerBar] (int bar) { return static_cast<int> (std::llround (bar * framesPerBar)); };
        gestures = {
            { frame (2), std::array { .35f, .55f, .2f, .45f }, Action::keep, -1 },
            { frame (4), {}, {}, 1 },
            { frame (5), {}, {}, 0 },
            { frame (6), {}, Action::mutate, -1 },
            { frame (8), std::array { .82f, .86f, .7f, .9f }, {}, -1 },
            { frame (10), {}, Action::returnToKept, -1 },
            { frame (12), {}, Action::capture, -1 },
            { frame (14), {}, Action::mutate, 1 },
            { frame (15), {}, {}, 0 }
        };
        return gestures;
    }
    for (int point = 8; point < 16 * 8; ++point)
    {
        const auto phase = static_cast<double> (point - 8) / (15 * 8);
        const auto movement = .5 - .5 * std::cos (phase * 4 * std::numbers::pi);
        const auto other = .5 + .4 * std::sin (phase * 2 * std::numbers::pi);
        const auto rhythm = profile == "rhythm" || profile == "combined";
        const auto texture = profile == "texture" || profile == "combined";
        const std::array axes {
            rhythm ? static_cast<float> (.1 + .8 * movement) : 0.f,
            rhythm ? static_cast<float> (.35 + .5 * other) : .5f,
            texture ? static_cast<float> (.7 * movement) : 0.f,
            texture ? static_cast<float> (.2 + .6 * other) : .5f
        };
        gestures.push_back ({ static_cast<int> (std::llround (point * framesPerBar / 8)), axes, {}, -1 });
    }
    return gestures;
}

void writeAudio (const Audio& audio, int rate, const juce::File& path, double gain = 1.0)
{
    REQUIRE_FALSE (path.exists());
    livecut::CapturedPhrase phrase;
    phrase.sampleRate = rate;
    phrase.channels = audio;
    for (auto& channel : phrase.channels)
        for (auto& value : channel)
            value = static_cast<float> (value * gain);
    REQUIRE (livecut::CaptureService::writeWav (phrase, path).wasOk());
}
} // namespace

// Opt in with LIVECUT_PERFORMANCE_RENDER_DIR=/absolute/new-directory and the
// explicit tag [.performance-render]. Never counted as a human listening pass.
TEST_CASE ("Render reproducible synthetic Performance audition cohort", "[.performance-render]")
{
    const auto* requested = std::getenv ("LIVECUT_PERFORMANCE_RENDER_DIR");
    REQUIRE (requested != nullptr);
    const juce::File directory { juce::String (requested) };
    REQUIRE (directory.isAbsolutePath (directory.getFullPathName()));
    REQUIRE_FALSE (directory.exists());
    REQUIRE (directory.createDirectory().wasOk());
    const auto rawDirectory = directory.getChildFile ("raw");
    const auto listeningDirectory = directory.getChildFile ("listening");
    REQUIRE (rawDirectory.createDirectory().wasOk());
    REQUIRE (listeningDirectory.createDirectory().wasOk());
    constexpr int rate = 48000, bars = 16;
    constexpr double bpm = 174.0;
    const auto framesPerBar = rate * 240.0 / bpm;
    const auto activeStart = static_cast<std::size_t> (std::ceil (framesPerBar));
    const auto source = demonstrationDrums (rate, bpm, bars);
    const std::array<juce::String, 6> names { "dry", "neutral", "rhythm", "texture", "combined", "actions" };
    std::array<Audio, names.size()> renders;
    std::array<AudioLevels, names.size()> levels;
    std::array<std::vector<Gesture>, names.size()> scripts;
    constexpr std::array blocks { 137, 512 };
    renders[0] = source;
    for (std::size_t profile = 0; profile < names.size(); ++profile)
    {
        if (profile > 0)
        {
            LivecutAudioProcessor processor;
            preparePerformance (processor, rate, 512);
            scripts[profile] = demonstrationGestures (names[profile], framesPerBar);
            renders[profile] = renderPerformance (processor, source, rate, blocks, scripts[profile], false, 0, bpm);
            CHECK (processor.getDisplayState().performanceState.load() == PerformanceState::playing);
        }
        levels[profile] = audioLevels (renders[profile], activeStart);
        REQUIRE (levels[profile].rms > 0);
        writeAudio (renders[profile], rate, rawDirectory.getChildFile (names[profile] + ".wav"));
    }
    auto targetRms = levels[0].rms;
    for (const auto& level : levels)
        targetRms = std::min (targetRms, level.rms);
    auto matchedPeak = 0.0;
    for (const auto& level : levels)
        matchedPeak = std::max (matchedPeak, level.peak * targetRms / level.rms);
    if (matchedPeak > .95)
        targetRms *= .95 / matchedPeak;

    auto metadata = object();
    auto& document = *metadata.getDynamicObject();
    document.setProperty ("scope", "Original synthetic percussion only; objective render evidence; human listening is pending.");
    document.setProperty ("source", "Procedural two-bar syncopated drum pattern generated by demonstrationDrums; no sampled recording.");
    document.setProperty ("sampleRate", rate);
    document.setProperty ("bpm", bpm);
    document.setProperty ("bars", bars);
    document.setProperty ("frames", static_cast<int> (source[0].size()));
    document.setProperty ("seed", 1);
    document.setProperty ("processingMode", "Perform, zero added latency, Spectral disabled");
    document.setProperty ("hostPartitions", "alternating 137/512, split at exact gesture sample timestamps");
    document.setProperty ("levelMatch", "One constant stereo gain per complete file; equal RMS measured from the first completed source bar to end; common peak headroom if needed; no limiter.");
    document.setProperty ("levelMatchStartFrame", static_cast<int> (activeStart));
    document.setProperty ("listeningTargetRms", targetRms);
    document.setProperty ("musicalQualityVerdict", "PENDING HUMAN AUDITION");
    juce::Array<juce::var> profiles;
    for (std::size_t profile = 0; profile < names.size(); ++profile)
    {
        const auto gain = targetRms / levels[profile].rms;
        writeAudio (renders[profile], rate, listeningDirectory.getChildFile (names[profile] + ".wav"), gain);
        auto record = object();
        auto& detail = *record.getDynamicObject();
        detail.setProperty ("profile", names[profile]);
        detail.setProperty ("rawWav", "raw/" + names[profile] + ".wav");
        detail.setProperty ("listeningWav", "listening/" + names[profile] + ".wav");
        detail.setProperty ("rawActiveRms", levels[profile].rms);
        detail.setProperty ("rawActivePeak", levels[profile].peak);
        detail.setProperty ("rawActiveDc", levels[profile].dc);
        detail.setProperty ("rawActiveDerivativePeakDiagnosticOnly", levels[profile].derivativePeak);
        detail.setProperty ("listeningGain", gain);
        detail.setProperty ("listeningGainDb", 20 * std::log10 (gain));
        detail.setProperty ("listeningActiveRms", levels[profile].rms * gain);
        detail.setProperty ("listeningActivePeak", levels[profile].peak * gain);
        juce::Array<juce::var> eventRecords;
        for (const auto& gesture : scripts[profile])
        {
            auto event = object();
            auto& item = *event.getDynamicObject();
            item.setProperty ("sampleFrame", gesture.frame);
            item.setProperty ("seconds", static_cast<double> (gesture.frame) / rate);
            item.setProperty ("barOneBased", 1 + gesture.frame / framesPerBar);
            if (gesture.axes)
            {
                for (std::size_t axis = 0; axis < axisIds.size(); ++axis)
                    item.setProperty (axisIds[axis], (*gesture.axes)[axis]);
            }
            if (gesture.action)
                item.setProperty ("action", actionName (*gesture.action));
            if (gesture.fill >= 0)
                item.setProperty ("performanceFill", gesture.fill != 0);
            eventRecords.add (event);
        }
        detail.setProperty ("sampleTimestampedGestures", juce::var (eventRecords));
        profiles.add (record);
    }
    document.setProperty ("profiles", juce::var (profiles));
    REQUIRE (directory.getChildFile ("manifest.json").replaceWithText (juce::JSON::toString (metadata, false, 17)));
    REQUIRE (directory.getChildFile ("README.md").replaceWithText (
        "# Synthetic Performance audition\n\n"
        "These are original procedural drums at 174 BPM, 48 kHz stereo, for sixteen bars. No artist recording or licensed break was used.\n\n"
        "Start with listening/dry.wav, neutral.wav, rhythm.wav, texture.wav, combined.wav, and actions.wav. Neutral repeats the captured first bar with both pads at defaults. Each file uses one recorded constant gain to match RMS after the first bar; raw/ preserves the processor output. The first bar monitors and captures the input before replay begins.\n\n"
        "Rhythm moves variation/density. Texture moves grain/character. Combined moves both pads. Actions keeps targets at bar 3, holds Fill at bar 5, mutates at bar 7, changes both pads at bar 9, returns at bar 11, requests a new full source bar at bar 13, and mutates with Fill at bar 15. manifest.json records exact sample timestamps and gains.\n\n"
        "Listen for pulse and phrase ending, purposeful variation, attack weight, texture body without clicks or brittle artifacts, stable mono, predictable gestures, and useful Fill/Return/Keep/Mutate recovery. Record actual listener observations separately.\n\n"
        "Objective finite/nonzero output and reproducibility do not establish musical quality. Human listening verdict: PENDING.\n"));
    SUCCEED (directory.getFullPathName().toStdString());
}

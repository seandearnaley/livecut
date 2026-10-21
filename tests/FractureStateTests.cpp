// SPDX-License-Identifier: GPL-3.0-or-later

#include "PluginProcessor.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <array>

namespace
{
constexpr std::array fractureIds {
    "fractureProcedure",
    "fractureActivity",
    "fractureGrainSizeMs",
    "fractureGrainDensity",
    "fractureGrainPositionSpreadMs",
    "fractureGrainPitchSpreadCents",
    "fractureGrainPanSpread",
    "fractureTapeStartSpeed",
    "fractureTapeEndSpeed",
    "fractureTapeCurve",
    "fractureTapeCycle",
    "fractureTapeAlternate",
    "fractureFrequencyEnabled",
    "fractureFrequencyMode",
    "fractureFrequencyChance",
    "fractureFrequencyHz",
    "fractureFrequencyEnvHz",
    "fractureFrequencyRepeatHz",
    "fractureFrequencyMix"
};
}

TEST_CASE ("Pre-Fracture state clears every adjusted Fracture control and retains legacy settings", "[processor][state][fracture]")
{
    LivecutAudioProcessor processor;
    auto& parameters = processor.getParameterState();
    auto* seed = parameters.getParameter ("seed");
    REQUIRE (seed != nullptr);
    seed->setValueNotifyingHost (seed->convertTo0to1 (13.0f));
    juce::MemoryBlock saved;
    processor.getStateInformation (saved);
    auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), static_cast<int> (saved.getSize()));
    REQUIRE (xml != nullptr);
    xml->setAttribute ("schemaVersion", 6);

    for (const auto* id : fractureIds)
    {
        INFO (id);
        auto* parameter = parameters.getParameter (id);
        REQUIRE (parameter != nullptr);
        auto* child = xml->getChildByAttribute ("id", id);
        REQUIRE (child != nullptr);
        xml->removeChildElement (child, true);
        parameter->setValueNotifyingHost (parameter->getDefaultValue() < 0.5f ? 1.0f : 0.0f);
        REQUIRE (parameter->getValue() != parameter->getDefaultValue());
    }

    juce::AudioProcessor::copyXmlToBinary (*xml, saved);
    processor.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
    for (const auto* id : fractureIds)
    {
        INFO (id);
        const auto* parameter = parameters.getParameter (id);
        CHECK (parameter->getValue() == Catch::Approx (parameter->getDefaultValue()).margin (1.0e-6f));
    }
    CHECK (parameters.getRawParameterValue ("seed")->load() == 13.0f);
}

TEST_CASE ("Fracture settings survive state recall into a fresh processor", "[processor][state][fracture]")
{
    LivecutAudioProcessor original;
    for (std::size_t i = 0; i < fractureIds.size(); ++i)
    {
        auto* parameter = original.getParameterState().getParameter (fractureIds[i]);
        REQUIRE (parameter != nullptr);
        const auto requested = i % 2 == 0 ? 0.8f : 0.2f;
        const auto legal = parameter->getNormalisableRange().snapToLegalValue (parameter->convertFrom0to1 (requested));
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (legal));
    }
    juce::MemoryBlock saved;
    original.getStateInformation (saved);
    LivecutAudioProcessor restored;
    restored.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
    for (const auto* id : fractureIds)
    {
        INFO (id);
        const auto* expected = original.getParameterState().getParameter (id);
        const auto* actual = restored.getParameterState().getParameter (id);
        REQUIRE (actual != nullptr);
        CHECK (actual->getValue() == Catch::Approx (expected->getValue()).margin (1.0e-6f));
    }
}

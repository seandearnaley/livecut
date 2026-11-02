// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "PluginProcessor.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

inline void useClassicSurface (LivecutAudioProcessor& processor)
{
    processor.getParameterState ().getParameter (livecut::ids::surface)->setValueNotifyingHost (1.0f);
}

// Where the hidden [.screenshot] cases write their PNGs. Without the variable
// they skip, so a tag filter that reaches them leaves nothing in the repo root.
inline juce::File screenshotDirectory()
{
    const auto* path = std::getenv ("LIVECUT_SCREENSHOT_DIR");
    if (path == nullptr || *path == '\0')
        SKIP ("Set LIVECUT_SCREENSHOT_DIR to write editor screenshots");

    const auto directory = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (path));
    REQUIRE (directory.createDirectory().wasOk());
    return directory;
}

inline void saveScreenshot (juce::Component& editor, const juce::File& directory, const char* name)
{
    const auto image = editor.createComponentSnapshot (editor.getLocalBounds(), true, 2.0f);
    juce::FileOutputStream stream (directory.getChildFile (name));
    REQUIRE (stream.openedOk());
    stream.setPosition (0);
    stream.truncate();
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
}

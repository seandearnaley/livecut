// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/PhraseRuler.h"

#include "engine/Cutter.h"
#include "ui/LivecutLookAndFeel.h"

#include <bit>
#include <cmath>
#include <cstdint>

namespace livecut::ui
{

PhraseRuler::Snapshot PhraseRuler::Snapshot::of (const DisplayState& state)
{
    Snapshot snapshot;
    snapshot.status = state.status.load (std::memory_order_relaxed);
    snapshot.phraseBars = state.phraseBars.load (std::memory_order_relaxed);
    snapshot.unitsPerBar = state.unitsPerBar.load (std::memory_order_relaxed);
    snapshot.blockStart = state.blockStart.load (std::memory_order_relaxed);
    snapshot.blockEnd = state.blockEnd.load (std::memory_order_relaxed);
    snapshot.position = state.position.load (std::memory_order_relaxed);
    return snapshot;
}

bool PhraseRuler::Snapshot::sameAs (const Snapshot& other) const
{
    const auto bits = [] (double value) { return std::bit_cast<std::uint64_t> (value); };

    return status == other.status && phraseBars == other.phraseBars && unitsPerBar == other.unitsPerBar
        && bits (blockStart) == bits (other.blockStart) && bits (blockEnd) == bits (other.blockEnd)
        && bits (position) == bits (other.position);
}

void PhraseRuler::show (const Snapshot& snapshot)
{
    if (snapshot.sameAs (current))
        return;

    current = snapshot;
    repaint();
}

juce::String PhraseRuler::describe (const Snapshot& snapshot)
{
    using Status = DisplayState::Status;

    switch (snapshot.status)
    {
        case Status::waitingForHost:
            return "Press play in your host to start cutting";
        case Status::barTooLong:
            return "Bars longer than " + juce::String (engine::Cutter::maxBarSeconds, 0)
                 + " seconds pass through uncut. Raise the tempo to cut them.";
        case Status::planOverflow: return "Cut plan queue exhausted; restart transport";
        case Status::sourceUnavailable: return "Source audio unavailable; restart transport";
        case Status::filling: return "Studio filling its 500 ms audio buffer";
        case Status::preparationError: return "Cannot prepare audio: use a sample rate from 8 to 384 kHz and prepare again";
        case Status::streamError: return "Audio stream error: prepare the plug-in again";
        case Status::startingSoon:
            return "Starting on the next unit";
        case Status::cutting:
        {
            const auto bar = juce::jlimit (1, juce::jmax (1, snapshot.phraseBars),
                                           static_cast<int> (std::floor (snapshot.position / snapshot.unitsPerBar)) + 1);
            return "Bar " + juce::String (bar) + " of " + juce::String (snapshot.phraseBars);
        }
    }

    return {};
}

void PhraseRuler::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (palette::lcd);
    g.fillRoundedRectangle (bounds, 6.0f);

    auto inner = bounds.reduced (16.0f, 12.0f);
    auto textLine = inner.removeFromTop (20.0f);
    const auto cutting = current.status == DisplayState::Status::cutting;

    g.setFont (lcdFont (15.0f));
    g.setColour (palette::lcdOn);
    g.drawText (describe (current), textLine, juce::Justification::centredLeft);

    if (cutting)
    {
        g.setColour (palette::lcdDim.brighter (0.6f));
        g.drawText (juce::String (current.unitsPerBar) + " units per bar", textLine, juce::Justification::centredRight);
    }

    inner.removeFromTop (10.0f);
    const auto ruler = inner;
    g.setColour (palette::lcdDim);
    g.fillRect (ruler.withTop (ruler.getBottom() - 1.0f));

    if (! cutting || current.phraseBars < 1 || current.unitsPerBar < 1)
        return;

    const auto totalUnits = static_cast<double> (current.phraseBars * current.unitsPerBar);
    const auto xFor = [&] (double units)
    {
        return ruler.getX() + static_cast<float> (units / totalUnits) * ruler.getWidth();
    };

    // The block being cut, lit behind the grid.
    g.setColour (palette::lcdOn.withAlpha (0.28f));
    g.fillRect (juce::Rectangle<float>::leftTopRightBottom (xFor (current.blockStart), ruler.getY(),
                                                            xFor (current.blockEnd), ruler.getBottom()));

    for (int unit = 0; unit <= current.phraseBars * current.unitsPerBar; ++unit)
    {
        const auto isBarLine = unit % current.unitsPerBar == 0;
        const auto x = xFor (unit);
        g.setColour (isBarLine ? palette::lcdOn.withAlpha (0.7f) : palette::lcdDim);
        g.fillRect (juce::Rectangle<float> (x - 0.5f, isBarLine ? ruler.getY() : ruler.getBottom() - 7.0f,
                                            1.0f, isBarLine ? ruler.getHeight() : 7.0f));
    }

    g.setColour (palette::lcdOn);
    g.fillRect (juce::Rectangle<float> (xFor (current.position) - 1.0f, ruler.getY(), 2.0f, ruler.getHeight()));
}

} // namespace livecut::ui

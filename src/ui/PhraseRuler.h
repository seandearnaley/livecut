// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "DisplayState.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace livecut::ui
{

// The LCD across the top of the editor: the current phrase's bars and units,
// the block being cut and the playhead, with a line of status text above.
class PhraseRuler final : public juce::Component
{
public:
    struct Snapshot
    {
        DisplayState::Status status = DisplayState::Status::waitingForHost;
        int phraseBars = 0;
        int unitsPerBar = 8;
        double blockStart = 0.0;
        double blockEnd = 0.0;
        double position = 0.0;

        static Snapshot of (const DisplayState& state);

        // Bitwise: the display only needs repainting when something changed at all.
        bool sameAs (const Snapshot& other) const;
    };

    void show (const Snapshot& snapshot);

    // What the status line says for a snapshot.
    static juce::String describe (const Snapshot& snapshot);
    juce::String statusText() const { return describe (current); }

    void paint (juce::Graphics& g) override;

private:
    Snapshot current;
};

} // namespace livecut::ui

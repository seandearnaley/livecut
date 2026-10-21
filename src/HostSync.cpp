// SPDX-License-Identifier: GPL-3.0-or-later

#include "HostSync.h"

#include <cmath>

namespace livecut
{

std::optional<HostSync> hostSync (const juce::AudioPlayHead::PositionInfo& info)
{
    const auto ppq = info.getPpqPosition();
    const auto bpm = info.getBpm();
    const auto meter = info.getTimeSignature();

    if (! info.getIsPlaying() || ! ppq || ! bpm || ! meter)
        return std::nullopt;

    // A tempo or time signature that gives a bar no length can't be followed either.
    if (! std::isfinite (*ppq) || ! std::isfinite (*bpm) || ! (*bpm > 0.0) || meter->numerator <= 0 || meter->denominator <= 0)
        return std::nullopt;

    HostSync sync;
    sync.timing.bpm = *bpm;
    sync.timing.beatsPerBar = 4.0 * meter->numerator / meter->denominator;

    // Hosts that don't report where the bar started are taken to have kept the
    // same meter from the start of the song, which is what JUCE documents.
    const auto barStart = info.getPpqPositionOfLastBarStart()
                              .orFallback (std::floor (*ppq / sync.timing.beatsPerBar) * sync.timing.beatsPerBar);
    sync.position.beatsIntoBar = *ppq - barStart;
    sync.position.bar = info.getBarCount()
                            .orFallback (static_cast<std::int64_t> (std::llround (barStart / sync.timing.beatsPerBar)));
    return sync;
}

} // namespace livecut

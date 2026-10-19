// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "engine/Cutter.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <optional>

namespace livecut
{

struct HostSync
{
    engine::Timing timing;
    engine::Position position;
};

// The cutter's view of the host transport, or nothing when the host is stopped
// or doesn't report the tempo, time signature and position needed to follow it.
std::optional<HostSync> hostSync (const juce::AudioPlayHead::PositionInfo& info);

} // namespace livecut

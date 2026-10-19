// SPDX-License-Identifier: GPL-3.0-or-later

#include "HostSync.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using livecut::hostSync;
using Catch::Matchers::WithinAbs;
using PositionInfo = juce::AudioPlayHead::PositionInfo;

namespace
{
PositionInfo playing (double ppq, double bpm = 120.0, int numerator = 4, int denominator = 4)
{
    PositionInfo info;
    info.setIsPlaying (true);
    info.setPpqPosition (ppq);
    info.setBpm (bpm);
    info.setTimeSignature (juce::AudioPlayHead::TimeSignature { numerator, denominator });
    return info;
}
} // namespace

TEST_CASE ("There is nothing to follow while the host is stopped", "[hostsync]")
{
    auto info = playing (0.0);
    info.setIsPlaying (false);

    CHECK_FALSE (hostSync (info).has_value());
}

TEST_CASE ("There is nothing to follow without tempo, position or time signature", "[hostsync]")
{
    auto noTempo = playing (0.0);
    noTempo.setBpm ({});
    auto noPosition = playing (0.0);
    noPosition.setPpqPosition ({});
    auto noMeter = playing (0.0);
    noMeter.setTimeSignature ({});

    CHECK_FALSE (hostSync (noTempo).has_value());
    CHECK_FALSE (hostSync (noPosition).has_value());
    CHECK_FALSE (hostSync (noMeter).has_value());
}

// Found in review: a numerator of zero gave bars no length at all.
TEST_CASE ("There is nothing to follow with a meaningless time signature", "[hostsync]")
{
    CHECK_FALSE (hostSync (playing (0.0, 120.0, 0, 4)).has_value());
    CHECK_FALSE (hostSync (playing (0.0, 120.0, 4, 0)).has_value());
    CHECK_FALSE (hostSync (playing (0.0, 0.0, 4, 4)).has_value());
}

TEST_CASE ("Bars and beats come from the host's bar start and count", "[hostsync]")
{
    auto info = playing (13.5, 128.0);
    info.setPpqPositionOfLastBarStart (12.0);
    info.setBarCount (3);

    const auto sync = hostSync (info);

    REQUIRE (sync.has_value());
    CHECK (sync->position.bar == 3);
    CHECK_THAT (sync->position.beatsIntoBar, WithinAbs (1.5, 1.0e-12));
    CHECK_THAT (sync->timing.bpm, WithinAbs (128.0, 1.0e-12));
    CHECK_THAT (sync->timing.beatsPerBar, WithinAbs (4.0, 1.0e-12));
}

TEST_CASE ("The time signature sets the quarter notes in a bar", "[hostsync]")
{
    CHECK_THAT (hostSync (playing (0.0, 120.0, 3, 4))->timing.beatsPerBar, WithinAbs (3.0, 1.0e-12));
    CHECK_THAT (hostSync (playing (0.0, 120.0, 6, 8))->timing.beatsPerBar, WithinAbs (3.0, 1.0e-12));
    CHECK_THAT (hostSync (playing (0.0, 120.0, 7, 8))->timing.beatsPerBar, WithinAbs (3.5, 1.0e-12));
}

TEST_CASE ("Without a bar start, bars are counted from the start of the song", "[hostsync]")
{
    const auto sync = hostSync (playing (13.5));

    REQUIRE (sync.has_value());
    CHECK (sync->position.bar == 3);
    CHECK_THAT (sync->position.beatsIntoBar, WithinAbs (1.5, 1.0e-12));
}

TEST_CASE ("Without a bar count, the bar is found from the bar start", "[hostsync]")
{
    auto info = playing (25.0, 120.0, 3, 4);
    info.setPpqPositionOfLastBarStart (24.0);

    const auto sync = hostSync (info);

    REQUIRE (sync.has_value());
    CHECK (sync->position.bar == 8);
    CHECK_THAT (sync->position.beatsIntoBar, WithinAbs (1.0, 1.0e-12));
}

// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "engine/CutProcedure.h"

#include <vector>

namespace livecut::engine
{

// Straight hits and triplet bounces, a ratchet roll that climbs to close each phrase, and the odd halftime bar.
class Bounce final : public CutProcedure
{
public:
    void chooseBlock (Random&, const CutParameters&, const BlockContext&, std::vector<Cut>&) override;
};

// The pad's Rush: the track as it is, and a roll into every bar line that runs
// even, speeds up or slows down. One-bar phrases, and no random draws.
class Rush final : public CutProcedure
{
public:
    int choosePhraseBars (Random&, const CutParameters&) override { return 1; }
    void chooseBlock (Random&, const CutParameters&, const BlockContext&, std::vector<Cut>&) override;
};

// Nick Collins' BBCutProc11: odd-length cuts, repeated, with a stutter roll near the end of each phrase.
class CutProc11 final : public CutProcedure
{
public:
    void chooseBlock (Random&, const CutParameters&, const BlockContext&, std::vector<Cut>&) override;
};

// Nick Collins' WarpCutProc1: straight slices or rolls that are even, accelerating or slowing.
class WarpCut final : public CutProcedure
{
public:
    void chooseBlock (Random&, const CutParameters&, const BlockContext&, std::vector<Cut>&) override;
};

// Nick Collins' SQPusher1: quaver and semiquaver cuts, with a transcribed
// Squarepusher fill on the last bar of every phrase.
class SQPusher final : public CutProcedure
{
public:
    // A fill is a list of blocks; each block lists its cut lengths in beats.
    using Fill = std::vector<std::vector<double>>;

    static const std::vector<Fill>& fills();

    void reset() override;
    int choosePhraseBars (Random&, const CutParameters&) override;
    void chooseBlock (Random&, const CutParameters&, const BlockContext&, std::vector<Cut>&) override;

private:
    int fill = -1; // index of the fill being played, or -1 outside a fill
    std::size_t fillBlock = 0;
};

} // namespace livecut::engine

// SPDX-License-Identifier: GPL-3.0-or-later

#include "EngineTestHelpers.h"
#include "engine/CutProcedures.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cstdint>
#include <vector>

using namespace livecut::engine;
using namespace livecut::engine::testing;
using Catch::Matchers::WithinAbs;

namespace
{
// 1/64 of a beat is the finest even division the pad's grids reach:
// 32 units per bar at 8 cuts per unit.
bool evenDivisions (const std::vector<Cut>& cuts, int unitsPerBar, double beatsPerBar)
{
    for (const auto& cut : cuts)
    {
        const auto beats = cut.units * beatsPerBar / unitsPerBar;

        if (! isWhole (beats * 64.0))
            return false;
    }

    return true;
}

struct Meter
{
    int unitsPerBar;
    double beatsPerBar;
};

// One phrase as the engine cut it before Straight existed, at commit 722d960:
// 16 units per bar in 4/4, phrases of 2 to 4 bars.
struct Golden
{
    std::uint32_t seed;
    std::vector<std::size_t> blockSizes;
    std::vector<Cut> cuts;
};

const std::vector<Golden> cutProc11Golden {
    // seed 1: 3 bars, 5 blocks
    { 1, { 1, 2, 1, 1, 14 },
    {
        { 9.0, 1.0, 0.0f, 1.0f, 0.0f },
        { 7.0, 1.0, 0.0f, 1.0f, 0.0f },
        { 7.0, 1.0, 0.0f, 1.0f, 0.0f },
        { 9.0, 1.0, 0.0f, 1.0f, 0.0f },
        { 9.0, 1.0, 0.0f, 1.0f, 0.0f },
        { 0.5, 1.0, -0.0412930138f, 1.0f, 0.0f },
        { 0.5, 1.0, -0.0258995388f, 1.0f, 0.0f },
        { 0.5, 1.0, -0.0105060637f, 1.0f, 0.0f },
        { 0.5, 1.0, 0.00488741091f, 1.0f, 0.0f },
        { 0.5, 1.0, 0.0202808846f, 1.0f, 0.0f },
        { 0.5, 1.0, 0.0356743596f, 1.0f, 0.0f },
        { 0.5, 1.0, 0.0510678329f, 1.0f, 0.0f },
        { 0.5, 1.0, 0.0664613098f, 1.0f, 0.0f },
        { 0.5, 1.0, 0.081854783f, 1.0f, 0.0f },
        { 0.5, 1.0, 0.0972482562f, 1.0f, 0.0f },
        { 0.5, 1.0, 0.112641729f, 1.0f, 0.0f },
        { 0.5, 1.0, 0.128035203f, 1.0f, 0.0f },
        { 0.5, 1.0, 0.143428683f, 1.0f, 0.0f },
        { 0.5, 1.0, 0.158822164f, 1.0f, 0.0f },
    } },
    // seed 2: 2 bars, 3 blocks
    { 2, { 2, 1, 13 },
    {
        { 5.0, 1.0, 0.0f, 1.0f, 0.0f },
        { 5.0, 1.0, 0.0f, 1.0f, 0.0f },
        { 9.0, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.0678660721f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.0732786283f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.078691192f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.0841037482f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.0895163044f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.0949288681f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.100341424f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.105753981f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.111166537f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.1165791f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.121991657f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.127404213f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.132816777f, 1.0f, 0.0f },
    } },
    // seed 3: 3 bars, 7 blocks
    { 3, { 2, 1, 2, 2, 1, 1, 30 },
    {
        { 7.0, 1.0, 0.0f, 1.0f, 0.0f },
        { 7.0, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.0, 1.0, 0.0f, 1.0f, 0.0f },
        { 3.0, 1.0, 0.0f, 1.0f, 0.0f },
        { 3.0, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.0, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.0, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.0, 1.0, 0.0f, 1.0f, 0.0f },
        { 9.0, 1.0, 0.0f, 1.0f, 0.0f },
        { 0.5, 1.0, 0.0596576221f, 1.0f, 0.0f },
        { 0.5, 1.0, 0.0538767278f, 1.0f, 0.0f },
        { 0.5, 1.0, 0.0480958335f, 1.0f, 0.0f },
        { 0.5, 1.0, 0.0423149392f, 1.0f, 0.0f },
        { 0.5, 1.0, 0.0365340449f, 1.0f, 0.0f },
        { 0.5, 1.0, 0.0307531524f, 1.0f, 0.0f },
        { 0.5, 1.0, 0.0249722581f, 1.0f, 0.0f },
        { 0.5, 1.0, 0.0191913638f, 1.0f, 0.0f },
        { 0.5, 1.0, 0.0134104704f, 1.0f, 0.0f },
        { 0.5, 1.0, 0.00762957614f, 1.0f, 0.0f },
        { 0.5, 1.0, 0.00184868206f, 1.0f, 0.0f },
        { 0.5, 1.0, -0.00393221201f, 1.0f, 0.0f },
        { 0.5, 1.0, -0.00971310586f, 1.0f, 0.0f },
        { 0.5, 1.0, -0.0154940002f, 1.0f, 0.0f },
        { 0.5, 1.0, -0.0212748945f, 1.0f, 0.0f },
        { 0.5, 1.0, -0.0270557869f, 1.0f, 0.0f },
        { 0.5, 1.0, -0.0328366831f, 1.0f, 0.0f },
        { 0.5, 1.0, -0.0386175737f, 1.0f, 0.0f },
        { 0.5, 1.0, -0.044398468f, 1.0f, 0.0f },
        { 0.5, 1.0, -0.0501793623f, 1.0f, 0.0f },
        { 0.5, 1.0, -0.0559602566f, 1.0f, 0.0f },
        { 0.5, 1.0, -0.0617411509f, 1.0f, 0.0f },
        { 0.5, 1.0, -0.0675220415f, 1.0f, 0.0f },
        { 0.5, 1.0, -0.0733029395f, 1.0f, 0.0f },
        { 0.5, 1.0, -0.0790838301f, 1.0f, 0.0f },
        { 0.5, 1.0, -0.0848647282f, 1.0f, 0.0f },
        { 0.5, 1.0, -0.0906456187f, 1.0f, 0.0f },
        { 0.5, 1.0, -0.0964265168f, 1.0f, 0.0f },
        { 0.5, 1.0, -0.102207407f, 1.0f, 0.0f },
        { 0.5, 1.0, -0.107988305f, 1.0f, 0.0f },
    } },
};

const std::vector<Golden> bounceGolden {
    // seed 1: 3 bars, 11 blocks
    { 1, { 2, 2, 3, 3, 2, 2, 2, 2, 3, 2, 12 },
    {
        { 2.6666666666666665, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 2.6666666666666665, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 2.6666666666666665, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 2.6666666666666665, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 2.6666666666666665, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 2.6666666666666665, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 2.6666666666666665, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0072610192f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0192022398f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0311434604f, 1.0f, 0.0f },
        { 0.66666666666666663, 1.0, 0.043084681f, 1.0f, 400.0f },
        { 0.66666666666666663, 1.0, 0.0550259016f, 1.0f, 400.0f },
        { 0.66666666666666663, 1.0, 0.0669671223f, 1.0f, 400.0f },
        { 0.33333333333333331, 1.0, 0.0789083391f, 1.0f, 700.0f },
        { 0.33333333333333331, 1.0, 0.0908495635f, 1.0f, 700.0f },
        { 0.33333333333333331, 1.0, 0.10279078f, 1.0f, 700.0f },
        { 0.33333333333333331, 1.0, 0.114731997f, 1.0f, 700.0f },
        { 0.33333333333333331, 1.0, 0.126673222f, 1.0f, 700.0f },
        { 0.33333333333333331, 1.0, 0.138614446f, 1.0f, 700.0f },
    } },
    // seed 2: 2 bars, 7 blocks
    { 2, { 2, 3, 3, 3, 2, 2, 12 },
    {
        { 2.6666666666666665, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 2.6666666666666665, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 2.6666666666666665, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.114134058f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0965728238f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0790115893f, 1.0f, 0.0f },
        { 0.66666666666666663, 1.0, 0.0614503548f, 1.0f, 400.0f },
        { 0.66666666666666663, 1.0, 0.0438891165f, 1.0f, 400.0f },
        { 0.66666666666666663, 1.0, 0.026327882f, 1.0f, 400.0f },
        { 0.33333333333333331, 1.0, 0.00876664557f, 1.0f, 700.0f },
        { 0.33333333333333331, 1.0, -0.0087945899f, 1.0f, 700.0f },
        { 0.33333333333333331, 1.0, -0.0263558254f, 1.0f, 700.0f },
        { 0.33333333333333331, 1.0, -0.0439170599f, 1.0f, 700.0f },
        { 0.33333333333333331, 1.0, -0.0614782982f, 1.0f, 700.0f },
        { 0.33333333333333331, 1.0, -0.0790395364f, 1.0f, 700.0f },
    } },
    // seed 3: 3 bars, 8 blocks
    { 3, { 1, 3, 2, 3, 2, 3, 2, 12 },
    {
        { 16.0, 1.0, 0.0f, 1.0f, -1200.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 2.6666666666666665, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 2.6666666666666665, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 2.6666666666666665, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, 0.0f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, -0.190407246f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, -0.165679097f, 1.0f, 0.0f },
        { 1.3333333333333333, 1.0, -0.140950948f, 1.0f, 0.0f },
        { 0.66666666666666663, 1.0, -0.116222799f, 1.0f, 400.0f },
        { 0.66666666666666663, 1.0, -0.0914946496f, 1.0f, 400.0f },
        { 0.66666666666666663, 1.0, -0.0667665005f, 1.0f, 400.0f },
        { 0.33333333333333331, 1.0, -0.0420383476f, 1.0f, 700.0f },
        { 0.33333333333333331, 1.0, -0.0173101984f, 1.0f, 700.0f },
        { 0.33333333333333331, 1.0, 0.00741795124f, 1.0f, 700.0f },
        { 0.33333333333333331, 1.0, 0.0321461f, 1.0f, 700.0f },
        { 0.33333333333333331, 1.0, 0.0568742529f, 1.0f, 700.0f },
        { 0.33333333333333331, 1.0, 0.081602402f, 1.0f, 700.0f },
    } },
};

const std::vector<Golden> sqPusherGolden {
    // seed 1: 3 bars, 13 blocks
    { 1, { 2, 1, 1, 2, 2, 4, 1, 2, 2, 4, 4, 3, 4 },
    {
        { 2.0, 1.0, 0.173022941f, 1.0f, 0.0f },
        { 2.0, 1.0, -0.0790669769f, 1.0f, 0.0f },
        { 2.0, 1.0, -0.0413677096f, 1.0f, 0.0f },
        { 2.0, 1.0, -0.0412930138f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.0746905953f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.1182191f, 1.0f, 0.0f },
        { 1.0, 1.0, 0.0137655633f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.0330780782f, 1.0f, 0.0f },
        { 1.0, 1.0, 0.175651118f, 1.0f, 0.0f },
        { 1.0, 1.0, 0.120297827f, 1.0f, 0.0f },
        { 1.0, 1.0, 0.121103004f, 1.0f, 0.0f },
        { 1.0, 1.0, 0.0769290477f, 1.0f, 0.0f },
        { 2.0, 1.0, 0.131658763f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.132067829f, 1.0f, 0.0f },
        { 1.0, 1.0, 0.0682112202f, 1.0f, 0.0f },
        { 2.0, 1.0, 0.18315582f, 1.0f, 0.0f },
        { 2.0, 1.0, -0.120979644f, 1.0f, 0.0f },
        { 1.0, 1.0, 0.0746003687f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.034984462f, 1.0f, 0.0f },
        { 1.0, 1.0, 0.100057729f, 1.0f, 0.0f },
        { 1.0, 1.0, 0.0642542914f, 1.0f, 0.0f },
        { 1.0, 1.0, 0.115711734f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.170654327f, 1.0f, 0.0f },
        { 1.0, 1.0, 0.163438201f, 1.0f, 0.0f },
        { 1.0, 1.0, 0.161348075f, 1.0f, 0.0f },
        { 6.0, 1.0, 0.0099197533f, 1.0f, 0.0f },
        { 2.0, 1.0, 0.0715342164f, 1.0f, 0.0f },
        { 4.0, 1.0, 0.164179355f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.00337073603f, 1.0f, 0.0f },
        { 1.0, 1.0, 0.0263648126f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.141308576f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.0956084058f, 1.0f, 0.0f },
    } },
    // seed 2: 2 bars, 8 blocks
    { 2, { 2, 2, 4, 2, 2, 1, 3, 1 },
    {
        { 2.0, 1.0, 0.172616348f, 1.0f, 0.0f },
        { 2.0, 1.0, -0.025871044f, 1.0f, 0.0f },
        { 2.0, 1.0, -0.138229325f, 1.0f, 0.0f },
        { 2.0, 1.0, 0.0477083884f, 1.0f, 0.0f },
        { 1.0, 1.0, 0.0530950949f, 1.0f, 0.0f },
        { 1.0, 1.0, 0.0116568366f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.000575532962f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.126224056f, 1.0f, 0.0f },
        { 1.0, 1.0, 0.0184829272f, 1.0f, 0.0f },
        { 1.0, 1.0, 0.138624594f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.0811926574f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.0287510678f, 1.0f, 0.0f },
        { 2.0, 1.0, -0.149136007f, 1.0f, 0.0f },
        { 4.0, 1.0, 0.0263060518f, 1.0f, 0.0f },
        { 4.0, 1.0, -0.157221735f, 1.0f, 0.0f },
        { 4.0, 1.0, 0.0309232287f, 1.0f, 0.0f },
        { 2.0, 1.0, -0.0128850061f, 1.0f, 0.0f },
    } },
    // seed 3: 3 bars, 17 blocks
    { 3, { 1, 1, 2, 2, 2, 2, 2, 1, 1, 2, 4, 4, 1, 2, 2, 1, 2 },
    {
        { 2.0, 1.0, 0.135979623f, 1.0f, 0.0f },
        { 2.0, 1.0, 0.157178789f, 1.0f, 0.0f },
        { 2.0, 1.0, -0.183747709f, 1.0f, 0.0f },
        { 2.0, 1.0, -0.179413125f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.141851947f, 1.0f, 0.0f },
        { 1.0, 1.0, 0.0596576221f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.00294563151f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.190407246f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.0390557423f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.0865899697f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.0900319591f, 1.0f, 0.0f },
        { 1.0, 1.0, 0.0178596061f, 1.0f, 0.0f },
        { 2.0, 1.0, -0.0969658867f, 1.0f, 0.0f },
        { 2.0, 1.0, 0.174553469f, 1.0f, 0.0f },
        { 2.0, 1.0, -0.0698775277f, 1.0f, 0.0f },
        { 2.0, 1.0, 0.138300344f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.0345125273f, 1.0f, 0.0f },
        { 1.0, 1.0, 0.0231363066f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.190752968f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.0373920351f, 1.0f, 0.0f },
        { 1.0, 1.0, 0.0438494645f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.0169254411f, 1.0f, 0.0f },
        { 1.0, 1.0, 0.0650745109f, 1.0f, 0.0f },
        { 1.0, 1.0, -0.086604856f, 1.0f, 0.0f },
        { 2.2857142857142856, 1.0, -0.0181511678f, 1.0f, 0.0f },
        { 2.2857142857142856, 1.0, 0.104231395f, 1.0f, 0.0f },
        { 2.2857142857142856, 1.0, 0.00561402179f, 1.0f, 0.0f },
        { 2.2857142857142856, 1.0, 0.0770535022f, 1.0f, 0.0f },
        { 2.2857142857142856, 1.0, -0.0551295131f, 1.0f, 0.0f },
        { 2.2857142857142856, 1.0, 0.00643784786f, 1.0f, 0.0f },
        { 1.1428571428571428, 1.0, 0.0764371678f, 1.0f, 0.0f },
        { 1.1428571428571428, 1.0, -0.0734861866f, 1.0f, 0.0f },
    } },
};

// Bit-exact: a zero-width match fails on any difference at all.
void requireGolden (CutProcedure& procedure, const CutParameters& params, const Golden& golden)
{
    CAPTURE (golden.seed);
    Random random (golden.seed);
    const auto blocks = playPhrase (procedure, random, params, 16);
    REQUIRE (blocks.size() == golden.blockSizes.size());
    std::size_t next = 0;

    for (std::size_t i = 0; i < blocks.size(); ++i)
    {
        CAPTURE (i);
        REQUIRE (blocks[i].cuts.size() == golden.blockSizes[i]);

        for (std::size_t j = 0; j < blocks[i].cuts.size(); ++j, ++next)
        {
            CAPTURE (j);
            REQUIRE (next < golden.cuts.size());
            const auto& cut = blocks[i].cuts[j];
            const auto& expected = golden.cuts[next];
            REQUIRE_THAT (cut.units, WithinAbs (expected.units, 0.0));
            REQUIRE_THAT (cut.gate, WithinAbs (expected.gate, 0.0));
            REQUIRE_THAT (cut.pan, WithinAbs (expected.pan, 0.0));
            REQUIRE_THAT (cut.amp, WithinAbs (expected.amp, 0.0));
            REQUIRE_THAT (cut.cents, WithinAbs (expected.cents, 0.0));
        }
    }

    REQUIRE (next == golden.cuts.size());
}
} // namespace

TEST_CASE ("Straight Cut rolls only in even divisions", "[straight]")
{
    const auto unitsPerBar = GENERATE (8, 16, 32);
    CutProc11 procedure;
    CutParameters params;
    params.straight = true;
    params.stutterChance = 1.0;
    params.stutterArea = 1.0;

    for (std::uint32_t seed = 1; seed <= 200; ++seed)
    {
        CAPTURE (unitsPerBar, seed);
        Random random (seed);
        const auto blocks = playPhrase (procedure, random, params, unitsPerBar);

        for (const auto& block : blocks)
            REQUIRE (evenDivisions (block.cuts, unitsPerBar, 4.0));
    }
}

TEST_CASE ("Straight Bounce bounces and ratchets in even divisions", "[straight]")
{
    const auto meter = GENERATE (values<Meter> ({ { 16, 4.0 }, { 12, 3.0 } }));
    Bounce procedure;
    CutParameters params;
    params.straight = true;
    params.bounceChance = 1.0;
    params.ratchetChance = 1.0;

    for (std::uint32_t seed = 1; seed <= 200; ++seed)
    {
        CAPTURE (meter.unitsPerBar, meter.beatsPerBar, seed);
        Random random (seed);
        const auto blocks = playPhrase (procedure, random, params, meter.unitsPerBar, meter.beatsPerBar);

        for (const auto& block : blocks)
            REQUIRE (evenDivisions (block.cuts, meter.unitsPerBar, meter.beatsPerBar));
    }
}

TEST_CASE ("A straight ratchet climbs in three steps over eight cuts", "[straight]")
{
    Bounce procedure;
    CutParameters params;
    params.straight = true;
    params.minPhraseBars = params.maxPhraseBars = 1;
    params.bounceChance = 0.0;
    params.halftimeChance = 0.0;
    params.ratchetChance = 1.0;
    params.climb = 12;
    params.fillDutyCycle = 0.37;
    constexpr int unitsPerBar = 16;
    constexpr double zone = 8.0; // two beats
    constexpr std::array expectedUnits { 2.0, 2.0, 1.0, 1.0, 0.5, 0.5, 0.5, 0.5 };
    constexpr std::array expectedCents { 0.0, 0.0, 600.0, 600.0, 1200.0, 1200.0, 1200.0, 1200.0 };

    for (std::uint32_t seed = 1; seed <= 50; ++seed)
    {
        CAPTURE (seed);
        Random random (seed);
        const auto blocks = playPhrase (procedure, random, params, unitsPerBar);
        const auto& last = blocks.back();

        REQUIRE_THAT (last.startUnits, WithinAbs (unitsPerBar - zone, tolerance));
        REQUIRE (last.cuts.size() == expectedUnits.size());
        CHECK_THAT (totalUnits (last.cuts), WithinAbs (zone, tolerance));

        for (std::size_t i = 0; i < last.cuts.size(); ++i)
        {
            CAPTURE (i);
            CHECK_THAT (last.cuts[i].units, WithinAbs (expectedUnits[i], tolerance));
            CHECK_THAT (last.cuts[i].cents, WithinAbs (expectedCents[i], 1.0e-6));
            CHECK_THAT (last.cuts[i].gate, WithinAbs (params.fillDutyCycle, tolerance));
        }
    }
}

TEST_CASE ("Straight Glitch plays only even fills", "[straight]")
{
    const auto unitsPerBar = GENERATE (16, 32);
    SQPusher procedure;
    CutParameters params;
    params.straight = true;
    params.activity = 0.95;

    for (std::uint32_t seed = 1; seed <= 300; ++seed)
    {
        CAPTURE (unitsPerBar, seed);
        Random random (seed);
        const auto blocks = playPhrase (procedure, random, params, unitsPerBar);

        for (const auto& block : blocks)
            REQUIRE (evenDivisions (block.cuts, unitsPerBar, 4.0));
    }
}

TEST_CASE ("Straight off reproduces the pre-Straight cut lists", "[straight]")
{
    CutParameters params;
    params.straight = false;
    params.minPhraseBars = 2;

    SECTION ("CutProc11")
    {
        params.stutterChance = 1.0;
        params.stutterArea = 1.0;

        for (const auto& golden : cutProc11Golden)
        {
            CutProc11 procedure;
            requireGolden (procedure, params, golden);
        }
    }

    SECTION ("Bounce")
    {
        params.bounceChance = 1.0;
        params.ratchetChance = 1.0;

        for (const auto& golden : bounceGolden)
        {
            Bounce procedure;
            requireGolden (procedure, params, golden);
        }
    }

    SECTION ("SQPusher")
    {
        params.activity = 0.95;

        for (const auto& golden : sqPusherGolden)
        {
            SQPusher procedure;
            requireGolden (procedure, params, golden);
        }
    }
}

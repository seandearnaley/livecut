// SPDX-License-Identifier: GPL-3.0-or-later

#include "PadSurface.h"

namespace livecut
{
Settings padSettings (const Settings& user, const engine::PadMapping& mapping)
{
    Settings settings{};
    settings.procedure = mapping.procedure;
    settings.unitsPerBar = mapping.unitsPerBar;
    settings.seed = mapping.seed;
    settings.cut = mapping.cut;
    settings.crusher = mapping.crusher;
    settings.wet = user.wet;
    settings.outputTrimDb = user.outputTrimDb;
    settings.requestedMode = user.requestedMode;
    settings.requestedSpectral = user.requestedSpectral;
    settings.spectralFreeze = user.spectralFreeze;
    settings.spectralSmearMs = user.spectralSmearMs;
    settings.spectralMix = user.spectralMix;
    settings.surface = user.surface;
    settings.padProgram = user.padProgram;
    settings.padX = user.padX;
    settings.padY = user.padY;
    settings.padTouch = user.padTouch;
    settings.padHold = user.padHold;
    settings.padQuantize = user.padQuantize;
    return settings;
}
} // namespace livecut

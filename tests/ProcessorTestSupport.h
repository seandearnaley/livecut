// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "PluginProcessor.h"

inline void useClassicSurface (LivecutAudioProcessor& processor)
{
    processor.getParameterState ().getParameter (livecut::ids::surface)->setValueNotifyingHost (1.0f);
}

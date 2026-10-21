// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Livecut, Copyright 2003-2004 Remy Muller.

#include "engine/LivePlayer.h"
#include "engine/TransformSpan.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <stdexcept>

namespace livecut::engine
{

float fadeEnvelope (int t, int fade, int length)
{
    const auto time = static_cast<double> (t);
    const auto fadeLength = static_cast<double> (fade);
    const auto attack = 1.0 - std::exp (-5.0 * time / fadeLength);
    const auto release = 1.0 - std::exp (5.0 * (time - length) / fadeLength);
    return static_cast<float> (attack * release);
}

int playableSamples (int length, double rate, int recorded, int capacity)
{
    const auto readable = [&] (int t)
    {
        const auto index = static_cast<std::int64_t> (static_cast<double> (t) * rate);
        const auto available = std::min<std::int64_t> (capacity, std::int64_t { recorded } + t + 1);
        return index + 1 < available;
    };

    // The read head moves at `rate` while the recording advances one sample at
    // a time into a finite buffer, so once a sample is unreadable every later
    // one is too: bisect for the first unreadable sample.
    auto low = 0;
    auto high = length;

    while (low < high)
    {
        const auto middle = low + (high - low) / 2;

        if (readable (middle))
            low = middle + 1;
        else
            high = middle;
    }

    return low;
}

void LivePlayer::prepare (int newCapacity, std::size_t retainedFrames)
{
    capacity = newCapacity;
    // A block may repeat its first eight seconds for the entire eight-bar
    // phrase. Every supported bar <=8s, so 8*sourceExtent plus two interpolation
    // guards retains its origin until its last output frame, even with swing.
    if (! history.prepare (retainedFrames != 0 ? retainedFrames : 8 * static_cast<std::size_t> (capacity) + 2))
        throw std::runtime_error ("Cannot prepare source history");
    sourceError = false;
    transform.reset();
    playing.clear();
    playing.reserve (maxCutsPerBlock);
    current = 0;
    position = 0;
}

void LivePlayer::setFadeSamples (int samples)
{
    fade = std::max (samples, 1);
}

int LivePlayer::playableFor (const PlayingCut& cut, int soundingLength) const
{
    // A live cut reading slower than the recording never catches up with it.
    if (cut.pitched && cut.start == 0 && cut.rate < 1.0)
        return soundingLength;

    return cut.pitched ? playableSamples (soundingLength, cut.rate, std::min (cut.start, capacity), capacity)
                       : soundingLength;
}

void LivePlayer::startBlock (std::span<const Cut> cuts, double newSamplesPerUnit)
{
    startBlock (cuts, newSamplesPerUnit, sourceForNextFrame());
}

void LivePlayer::startBlock (std::span<const Cut> cuts, double newSamplesPerUnit, SourceRegion newSource)
{
    source = newSource;
    transform.reset();
    assert (cuts.size() <= maxCutsPerBlock);
    playing.clear();
    current = 0;
    position = 0;
    samplesPerUnit = newSamplesPerUnit;

    auto units = 0.0;
    auto start = 0;

    for (std::size_t i = 0; i < cuts.size(); ++i)
    {
        const auto& cut = cuts[i];
        units += cut.units;
        const auto end = static_cast<int> (std::lround (units * samplesPerUnit));
        const auto size = end - start;

        if (size > 0)
        {
            assert (size <= capacity);
            PlayingCut playingCut;
            playingCut.start = start;
            playingCut.size = size;
            playingCut.units = cut.units;
            playingCut.logicalStart = units - cut.units;
            playingCut.gate = cut.gate;
            playingCut.pitched = std::abs (cut.cents) > 1.0e-3f && (i > 0 || cut.cents < 0.0f);
            playingCut.rate = playingCut.pitched ? std::pow (2.0, cut.cents / 1200.0) : 1.0;
            playingCut.playable = playableFor (playingCut, std::min (static_cast<int> (std::lround (size * cut.gate)), size));
            playingCut.soundingFrames = std::min (static_cast<int> (std::lround (size * cut.gate)), size);

            // Livecut's pan rotates the stereo pair: the input on the far side
            // folds into the near channel as the cut moves away from centre.
            const auto angle = static_cast<double> (cut.pan) * std::numbers::pi / 2.0;
            const auto amp = static_cast<double> (cut.amp);
            playingCut.leftToLeft = static_cast<float> (amp * (cut.pan < 0.0f ? 1.0 : std::cos (angle)));
            playingCut.leftToRight = static_cast<float> (amp * (cut.pan < 0.0f ? 0.0 : std::sin (angle)));
            playingCut.rightToLeft = static_cast<float> (amp * (cut.pan > 0.0f ? 0.0 : -std::sin (angle)));
            playingCut.rightToRight = static_cast<float> (amp * (cut.pan > 0.0f ? 1.0 : std::cos (angle)));

            playing.push_back (playingCut);
        }

        start = end;
    }
}

void LivePlayer::setSamplesPerUnit (double newSamplesPerUnit)
{
    const auto unchanged = std::abs (newSamplesPerUnit - samplesPerUnit) <= 1.0e-9 * samplesPerUnit;

    if (unchanged || current >= playing.size())
    {
        samplesPerUnit = newSamplesPerUnit;
        return;
    }

    // Lay out the rest of the block again in units from the current sample,
    // rounding cumulatively so the cuts still add up to the block.
    auto& now = playing[current];
    const auto elapsedUnits = position / samplesPerUnit;
    const auto samplesFromCutStart = [&] (double unitsFromNow)
    {
        return position + static_cast<int> (std::lround (std::max (unitsFromNow, 0.0) * newSamplesPerUnit));
    };

    const auto soundingUnitsLeft = now.gate * now.units - elapsedUnits;

    if (soundingUnitsLeft > 0.0)
        now.playable = playableFor (now, samplesFromCutStart (soundingUnitsLeft));
    now.soundingFrames = samplesFromCutStart (soundingUnitsLeft);

    auto unitsFromNow = now.units - elapsedUnits;
    auto end = samplesFromCutStart (unitsFromNow);
    now.size = end;

    for (auto i = current + 1; i < playing.size(); ++i)
    {
        auto& cut = playing[i];
        unitsFromNow += cut.units;
        const auto nextEnd = samplesFromCutStart (unitsFromNow);
        cut.start = now.start + end;
        cut.size = nextEnd - end;
        cut.playable = playableFor (cut, std::min (static_cast<int> (std::lround (cut.size * cut.gate)), cut.size));
        cut.soundingFrames = std::min (static_cast<int> (std::lround (cut.size * cut.gate)), cut.size);
        assert (cut.size <= capacity);
        end = nextEnd;
    }

    samplesPerUnit = newSamplesPerUnit;
    skipFinishedCuts();
}

void LivePlayer::skipFinishedCuts()
{
    // A faster tempo can shrink what's left of a cut, or a whole cut, to nothing.
    while (current < playing.size() && position >= playing[current].size)
    {
        ++current;
        position = 0;
        transform.reset();
    }
}

SourceRegion LivePlayer::sourceAtFrame (std::int64_t frame) const
{
    return { history.retainedRegion().epoch, frame, capacity };
}

SourceRegion LivePlayer::sourceForNextFrame() const
{
    const auto retained = history.retainedRegion();
    return sourceAtFrame (retained.firstFrame + retained.frameCount);
}

void LivePlayer::resetRendering()
{
    transform.reset();
    playing.clear();
    current = 0;
    position = 0;
    sourceError = false;
}

void LivePlayer::resetSource()
{
    sourceError = ! history.reset();
}

bool LivePlayer::ingestFrame (float left, float right)
{
    if (! history.append ({ left, right }))
        sourceError = true;
    return ! sourceError;
}

std::optional<StereoFrame> LivePlayer::readSourceFrame (double relativeFrame) const
{
    if (! std::isfinite (relativeFrame) || relativeFrame < 0.0 || relativeFrame >= static_cast<double> (source.frameCount))
        return std::nullopt;
    const auto index = static_cast<std::int64_t> (relativeFrame);
    // Validate the double fraction before AudioHistory converts it to float
    // for classic interpolation; a value below one can round to float 1.0.
    return history.readFractional (source, index, relativeFrame - static_cast<double> (index));
}

void LivePlayer::process (float& left, float& right)
{
    ingestFrame (left, right);
    renderFrame (left, right);
}

void LivePlayer::advanceToUnits (double logicalUnits)
{
    while (current < playing.size() && logicalUnits >= playing[current].logicalStart + playing[current].units)
    {
        ++current;
        position = 0;
        transform.reset();
    }
}

int LivePlayer::currentSoundingFrames() const
{
    return current < playing.size() ? playing[current].soundingFrames : 0;
}

void LivePlayer::markCurrentTransformDecided()
{
    if (current < playing.size() && position == 0)
        playing[current].transformDecided = true;
}

bool LivePlayer::setCurrentTransform (SourceRegion completeFixedSpan, bool reverse)
{
    if (current >= playing.size() || position != 0 || playing[current].transformDecided
        || sourceError || completeFixedSpan.frameCount > capacity || ! sourceAvailable (completeFixedSpan))
        return false;
    auto& cut = playing[current];
    const auto required = requiredSourceFrames (cut.soundingFrames, cut.rate, capacity);
    if (! required || completeFixedSpan.frameCount < *required)
        return false;
    transform = Transform { completeFixedSpan, cut.rate, cut.soundingFrames, reverse };
    cut.transformDecided = true;
    return true;
}

bool LivePlayer::sourceAvailable (SourceRegion completeFixedSpan) const
{
    return history.isAvailable (completeFixedSpan);
}

SourceRegion LivePlayer::retainedSource() const
{
    return history.retainedRegion();
}

void LivePlayer::retimeMusicalCut (int samplesToEnd, int samplesToGate, std::int64_t renderedExclusiveEnd)
{
    if (current >= playing.size())
        return;
    auto& cut = playing[current];
    cut.size = position + std::max (samplesToEnd, 1);
    const auto sounding = position + std::max (samplesToGate, 0);
    const auto progress = renderedExclusiveEnd - source.firstFrame;
    const auto recordedAtStart = static_cast<int> (std::clamp<std::int64_t> (progress - position - 1, 0, capacity));
    cut.playable = cut.pitched ? (cut.start == 0 && cut.rate < 1.0 ? sounding : playableSamples (sounding, cut.rate, recordedAtStart, capacity))
                               : std::min (sounding, capacity);
    cut.soundingFrames = sounding;
}

void LivePlayer::renderFrame (float& left, float& right, bool advanceBySamples)
{
    if (current >= playing.size())
    {
        left = 0.0f;
        right = 0.0f;
        return;
    }

    const auto& cut = playing[current];
    auto outLeft = 0.0f;
    auto outRight = 0.0f;
    const auto sounding = transform ? std::min ({ cut.soundingFrames, cut.size, transform->outputFrames }) : cut.playable;

    if (position < sounding)
    {
        float inLeft;
        float inRight;

        std::optional<StereoFrame> frame;
        if (transform)
        {
            if (const auto coordinate = readCoordinate (position, transform->rate, transform->source.frameCount, transform->reverse);
                coordinate && sourceAvailable (transform->source))
                frame = history.readFractional (transform->source, coordinate->index, coordinate->fraction);
        }
        else
        {
            const auto read = cut.pitched ? static_cast<double> (position) * cut.rate : static_cast<double> (position);
            frame = readSourceFrame (read);
        }
        if (! frame)
        {
            sourceError = true;
            left = right = 0.0f;
            return;
        }
        inLeft = frame->left;
        inRight = frame->right;

        const auto envelope = fadeEnvelope (position, fade, sounding);
        outLeft = envelope * (cut.leftToLeft * inLeft + cut.rightToLeft * inRight);
        outRight = envelope * (cut.leftToRight * inLeft + cut.rightToRight * inRight);
    }

    left = outLeft;
    right = outRight;
    ++position;
    if (advanceBySamples)
        skipFinishedCuts();
}

} // namespace livecut::engine

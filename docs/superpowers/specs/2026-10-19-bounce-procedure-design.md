# Bounce: a fourth cut procedure

Status: approved to build by the maintainer on 2026-10-19, for Livecut 0.2.0.
Tracking issue: #9.

## Intent

Livecut's three procedures are portraits of breakbeat cutting around 2000:
CutProc11 is the classic jungle edit, WarpCut the Warp Records IDM roll, and
SQPusher the Squarepusher drill'n'bass fill. Every one of them cuts on a
straight grid, and their pitch moves are smooth sweeps or random scatter.

Bounce is the same idea for the 2020s: the way jersey club, drill, UK bass and
the jungle revival treat a break. Three things define that sound and none of
the existing procedures can make them:

1. **Triplets against a straight break**, the bounce of jersey club kicks and
   drill hi-hat rolls.
2. **Rolls that speed up in steps and climb in semitones**, rather than
   WarpCut's smooth geometric curve and continuous pitch sweep.
3. **Halftime drops**: a bar that suddenly plays at half speed.

Bounce should feel like a producer re-editing the incoming break with those
moves, while the break stays recognisable most of the time.

## Goals

- A fourth procedure, **Bounce**, selectable beside the other three.
- Four new controls, shown only while Bounce is selected: **Bounce**,
  **Ratchet**, **Climb** and **Halftime**.
- It sounds right out of the box: with default settings, a straight drum
  loop clearly bounces, ratchets at phrase ends and occasionally drops to
  halftime.
- Ships as Livecut 0.2.0.

## Non-goals

- No hit (onset) detection: blocks start on beats, wherever the hits are.
- No look-ahead or added latency.
- No time-stretching: a halftime bar drops an octave, like tape at half speed.
- No swing or humanise controls.
- No changes to how CutProc11, WarpCut or SQPusher sound.

## Anti-goals

- Bounce must not turn the break into mush: straight "groove" blocks are the
  most common move at default settings.
- No allocation, locking or added latency on the audio thread.
- No change that breaks existing saved sets: no parameter ID is renamed or
  removed, and existing parameters keep their ranges and defaults.

## Constraints

- The engine stays JUCE-free C++20 (`src/engine`), real-time safe and
  deterministic: the same seed and host position give the same cuts on every
  platform, using the engine's own `Random`.
- The `CutProcedure` interface: each call to `chooseBlock` returns one block, a
  list of `Cut`s. Every cut in a block replays the block's recording from its
  start; only the first cut plays the input live. A block is never zero units
  long, never runs past the phrase, and holds at most `maxCutsPerBlock` cuts.
- A block never lasts longer than a bar (the recording buffer holds one bar of
  at most 8 seconds).
- Test-first (red, then green) for every behaviour below.
- CI must stay green, including pluginval at strictness 10 and `auval`.

## Musical model

Bounce works in **beats** (quarter notes), not in the subdivision's units:
one beat is `unitsPerBar / beatsPerBar` units, which may be fractional (for
example 1.5 units with 6 units per bar in 4/4). Cut lengths are fractional
units, as SQPusher's already are.

Positions come from `BlockContext`: `unitsDone` counts units from the bar line
the phrase started on, so the position in the bar is `unitsDone` modulo
`unitsPerBar`. A phrase can start part way through a bar after the cutter
resets; Bounce must cope with blocks that start off the beat.

Rules for every block:

- **Blocks never cross a bar line**, so a decision can be made at every bar.
- **Blocks never cross the start of the ratchet zone** (below), so a phrase
  always reaches it on a block boundary.
- If a block starts off the beat, it is a groove block that runs to the next
  beat (or the next boundary above, whichever comes first).
- In meters whose bar is shorter than one beat (such as 1/8), every block,
  including a ratchet, is clipped to the bar line. (Builder's call.)

### The four moves

Each block is chosen in this order:

1. **Ratchet** (end of phrase). The **ratchet zone** is the last two beats of
   the phrase, or the last beat if the phrase's last bar is shorter than two
   beats. When a block would start exactly at the zone's start, Bounce rolls
   the Ratchet chance once. On success the whole zone is one ratchet block. On
   failure the zone is played with the ordinary moves below (groove or bounce,
   never halftime).

   A ratchet block has three steps that last 1/2, 1/4 and 1/4 of the zone. The
   cuts in each step are `r`, `2r` and `4r` per beat, where `r` is 3 for a
   two-beat zone and 6 for a one-beat zone. So a two-beat zone is 3 cuts of a
   third of a beat, then 3 cuts of a sixth, then 6 cuts of a twelfth: 12 cuts,
   eighth-note triplets accelerating to 32nd-note triplets.

   Pitch climbs in whole semitones, one level per step: step `k` of 3 is
   `round(climb * k / 2)` semitones (0, half the climb, the full climb). The
   first cut is live and stays at its original pitch, which step 0 already is.
   Gate is **Fill duty**. Pan and amplitude sweep from one random setting to
   another across the roll, as `sweep` does for the existing rolls.

2. **Halftime**. At the start of a bar that is not the phrase's last bar,
   Bounce rolls the Halftime chance. On success the bar is one block of one
   cut, a whole bar long, pitched down 1200 cents: it plays the first half of
   the bar's audio at half speed. Gate is **Duty**; amplitude is drawn from the
   Amp range, pan is centred. A one-bar phrase never has a halftime bar.

   This is the one move that needs the engine change below.

3. **Bounce** (one beat). If a whole beat is left before the next bar line,
   ratchet zone or phrase end, Bounce rolls the Bounce chance; otherwise the
   block is a groove block to that boundary. On success the beat is a triplet
   block, equally likely to be:
   - **even**: three cuts of a third of a beat (the start of the beat three
     times, as triplets), or
   - **shuffle**: two cuts, two thirds then one third of a beat (long-short).

   Gate is **Duty**. Each cut's amplitude is drawn from the Amp range; pan is
   centred and pitch is unchanged.

4. **Groove** (otherwise). One cut, one or two beats long with equal chance,
   clipped to the bar line, the ratchet zone and the end of the phrase. Gate
   is **Duty**, amplitude from the Amp range, pan centred, pitch unchanged.

At default settings, 60% of the on-beat choices outside halftime bars and
ratchets are groove blocks, so the break stays recognisable.

The shared **Pitch** range is not used by Bounce; its pitch moves are Climb and
halftime. The shared **Subdivision** only changes the ruler's tick marks, since
Bounce thinks in beats.

## Engine change: a live first cut may be pitched down

Today `LivePlayer` ignores the first cut's pitch (`pitched = i > 0 && ...`),
because the first cut plays the input as it arrives and could read audio that
hasn't been recorded yet. That is only true for pitching up. A cut pitched
down reads more slowly than the recording grows, so it never catches up.

Change: the first cut honours its pitch when it is **below** the original
(cents < 0); a first cut pitched up still plays at the original pitch, as now.

Watch out: `playableSamples` treats a sample as readable only if the
interpolation's next sample (`index + 1`) has already been recorded. For a
first cut nothing is recorded at `t = 0`, so today's function returns 0
playable samples and the bisection's assumption (once unreadable, always
unreadable) fails for this case. The fix must make a pitched-down first cut
sound for its whole gated length, without reading unrecorded samples, and the
tests must pin that down (including that the first cut pitched up is still
ignored, and that later cuts behave exactly as before).

This change must not change how the other procedures sound. SQPusher calls
`scatter`, which gives every cut a random pitch, including the first; today
the player throws the first cut's pitch away. So `scatter` must leave the
first cut at its original pitch. It still draws that cut's random pitch and
then sets it back to zero, so the random sequence, and with it every
existing seed's pattern, stays the same. A test pins SQPusher's cuts for a
fixed seed and Pitch range below zero to what they are today. (`sweep`
already gives the first cut its original pitch.)

Update the comment in `LivePlayer.h` that says the first cut ignores its
pitch, and `scatter`'s comment in `CutShaping.h`.

## Parameters

New parameters, appended after the existing ones. Their IDs never change once
released.

| ID | Name | Range | Default | Display |
|---|---|---|---|---|
| `bounceChance` | Bounce | 0–100% | 40% | percent |
| `ratchetChance` | Ratchet | 0–100% | 70% | percent |
| `climb` | Climb | 0 to +12 semitones, whole steps | +7 | `+7 st` |
| `halftimeChance` | Halftime | 0–100% | 15% | percent |

- Use JUCE's `ParameterID` version hint **2** for the four new parameters (the
  existing ones use 1), as JUCE recommends for parameters added in a later
  release, so AU hosts keep parameter order stable.
- The **Cut Procedure** choice gains a fourth entry, **Bounce**, appended
  after SQPusher. Saved sets store the choice's index, so they reload
  correctly. Host automation of Cut Procedure is stored as a 0–1 value, which
  now maps across four choices instead of three, so an automated Cut
  Procedure lane from 0.1.0 would change meaning. The release workflow
  generates notes from PR titles, so this warning goes in the
  `bounce/plugin` PR description and is pasted into the 0.2.0 draft release
  when it is tagged, before it is published. (Builder's call; no changelog
  file is introduced.)
- `SettingsReader` reads the new parameters with the same validation as the
  others (non-finite reads as the default, everything else is clamped), and
  `applySettings` passes them to the cutter through `CutParameters`.

## Editor

- The procedure switch gets a fourth segment, **Bounce**; widen it so every
  label fits at the default size.
- The procedure section shows the four Bounce controls when Bounce is active,
  laid out like the other procedures' controls, with the description:
  "Straight hits and triplet bounces, a ratchet roll that climbs to close each
  phrase, and the odd halftime bar".
- The footer currently credits all procedures to BBCut. Change it to credit
  CutProc11, WarpCut and SQPusher to Nick Collins' BBCut, since Bounce is new.

## Delivery

A two-layer stack branched from `main` once the whole 0.1.0 stack (PRs #1 to
#8) has landed there, never from a branch of that stack:

1. **`bounce/engine`**: the pitched-down first cut in `LivePlayer`, the
   `Bounce` procedure, `Procedure::bounce` and the cutter wiring, the new
   fields in `CutParameters`, and engine tests. This design document is its
   first commit.
2. **`bounce/plugin`**: the parameters, settings, editor, README (describe
   Bounce beside the other procedures) and the version bump to 0.2.0 in
   `CMakeLists.txt`.

Each layer is independently green.

## Testing

Engine tests, in the style of `tests/engine/WarpCutTests.cpp`, across several
subdivisions (6, 8, 16, 32 units per bar), meters (4/4, 3/4, 7/8) and seeds:

- Blocks never cross a bar line or the ratchet zone's start, and a phrase
  always adds up exactly.
- With Ratchet at 100%, every phrase ends in a ratchet block with the step
  structure and semitone levels above; at 0%, none does.
- With Halftime at 100%, every bar except the last starts a one-bar,
  -1200 cent block; at 0%, none does; one-bar phrases never have one.
- With Bounce at 100% (and the others at 0%), on-beat blocks are even or
  shuffle triplets of exactly one beat; at 0%, all non-ratchet blocks are
  groove blocks.
- Determinism: the same seed gives the same blocks.
- A phrase that starts off the beat recovers onto the beat.
- No allocation while processing a phrase with the cutter (use the existing
  allocation counter tests' approach).
- `LivePlayer`: the pitched-down first cut test cases described above.

Plugin tests: the existing "a control for every parameter" test covers the new
controls; extend "only the active procedure's controls are shown" to Bounce,
and add a parameters test for the new defaults and the fourth procedure.

Hands-on: load the 0.2.0 build in Ableton Live with a straight drum loop and
hear bounces, phrase-end ratchets climbing in steps, and halftime bars.

## Left to the builder

- Names of internal helpers and the exact layout of the new controls.
- How `playableSamples` is changed for the pitched-down first cut, as long as
  the behaviour above holds and is tested.
- The README wording.

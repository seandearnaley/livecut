# Livecut 0.6–0.8 implementation roadmap

This sequence adds three focused musical releases to 0.5.1 Recut. Each release
must be independently reviewable and validated before the next release lands.
BandCut, MIDI features, pitch-preserving time-stretching and preset banks are
outside this sequence.

## 0.6 Impact

Slice Filter, Drive/Wavefold and Transient Shaper share cut onset context and
retain separate enable, probability and mix controls. Filter/drive share a
cut-triggered envelope and have
bounded progression across repeats. The new rack follows Crusher/Comb and feeds
wet Audio Hold; final-output Capture remains post-mix.

`ImpactEffects` is a bounded plain-C++ unit. `Cutter::BlockPlan` snapshots its
parameters. A separate Impact onset event executes before the cut's first output
sample without changing the old Crusher/Comb call order. Probability uses
independent tagged hashes rather than draws from the legacy pattern RNG.

New effects default off. Existing parameter/state identities, old ranges and
defaults remain intact. New controls use version hint/schema 6. See the
[0.6 behavior contract](releases/0.6.0.md), including ADAA's half-sample phase and
high-frequency response limits despite no additional buffered latency.

Acceptance: RED/GREEN component and integration regressions; neutral legacy
audio, unchanged patterns/RNG, physical onsets, queued parameter snapshots,
exact modulation schedules across host buffers and exact audio for the same
original render contexts, tempo/seek handling, no processing allocation,
state migration, rendered UI, Hold/Capture, exact mode delay and deferred-host
restart convergence. Review the immutable patch independently before release.

## 0.7 Fracture

GrainCut and TapeCut need explicit new playback modes, with the classic reader
left unchanged. The procedures choose musical blocks; the new bounded readers
implement the actual grains and continuous tape trajectories. Shorter classic
repeats alone do not satisfy these features.

GrainCut uses fixed voice storage, tapered windows, explicit overlap normalization
and deterministic source/pitch variation. Each grain freezes its complete source
span and lifetime. Sources belong to the current transport region and must be
available before activation. Fresh Live playback may explicitly await grain
history; it must not silently substitute another procedure. The outer musical
cuts trigger FX; internal grain launches do not.

TapeCut integrates a continuous start/stop speed curve into its read position.
The initial design limits speed to at most the original speed so Live playback
does not overtake incoming audio. Tempo edits preserve read-position continuity.
Attack Align and Reverse retain complete-source eligibility and output-budget
rules for the full curved trajectory.

Frequency Shifter and Ring Mod share a coherent oscillator but use distinct
paths: both sidebands for Ring Mod, approximate single-sideband translation for
the shifter using a quadrature network. Measure sideband rejection and the usable
frequency range across supported sample rates. Its allpass phase response must
be documented; no extra fixed buffering delay is assumed. Primary references:
[Wardle](https://www.dafx.de/paper-archive/1998/WAR19.PS.pdf) and
[Csound's Hilbert documentation](https://csound.com/docs/manual/hilbert.html).

Preserve the original four-choice `procedure` parameter exactly. Appending choices
would change its normalized host-automation mapping even if raw saved indexes
survived. Append a version-7 `fractureProcedure` selector with Classic (default),
GrainCut and TapeCut. Classic uses the original selector; the other choices are
explicit procedure-family overrides. Structural changes retain bar boundaries.

Acceptance adds bounded voice/source proofs, startup and transport-region tests,
deterministic grain schedules, overlap levels, tape trajectory/tempo tests,
measured spectral sidebands, original automation mapping and CPU/memory evidence.
The inherited swung gate-rounding sensitivity documented for 0.6 needs a
deliberate compatibility/versioning decision before any timing correction.

## 0.8 Resonance

A small tuned resonator bank exposes root/intervals, excitation, decay and mix.
Use stable pole radii below one and explicit gain normalization. Verify tuning,
decay, retuning behavior and summed output levels rather than masking excessive
gain with an undisclosed limiter. [Smith's resonator treatment](https://www.dsprelated.com/freebooks/filters/Resonator_Bandwidth_Terms_Pole.html)
provides the pole/radius basis.

Spectral Freeze/Smear needs a real STFT analysis/resynthesis stage. Begin with a
fixed FFT/hop configuration, verified weighted overlap-add normalization, frozen
magnitudes with continuous phase evolution and controlled smoothing. The
[WOLA reconstruction condition](https://www.dsprelated.com/freebooks/SASP/Choice_WOLA_Window.html)
must be verified for the exact windows and scheduler used.

Prototype the post-render stage before integration. Measure its exact impulse
delay; do not infer a latency formula from FFT size alone. Delay dry/bypass and
musical events by that same amount. Hold and Capture must receive the events
alongside the corresponding audio. Publish total configured latency through the
existing stopped-Apply lifecycle and retain the declaration across release and
reprepare. Spectral off defaults to the original latency contract.

Use three explicit order choices rather than arbitrary routing. The default
retains the historical order, and order changes use a defined musical boundary
and short transition. Source epochs, FFT windows crossing seeks, initial fill,
tail/reset behavior and exclusive-end phrase events are architectural acceptance
gates, not follow-up polish. Do not silently replace unavailable spectral audio.

Acceptance adds unity reconstruction, measured latency and dry/bypass alignment,
freeze phase continuity, smear response, spectral transport/event ordering,
effect-order transitions, lifecycle restart convergence and measured CPU/memory.

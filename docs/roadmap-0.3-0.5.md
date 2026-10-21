# Livecut 0.3–0.5 release plan

Requested 2026-10-21. Baseline: v0.2.0, commit `cb32fbe`.

The goal is three substantive, independently validated releases. Each ends in
a GitHub **draft** containing the signed/notarised universal macOS installer,
the Windows x64 plug-ins archive, and release notes. Publication remains with
the user. Windows signing is not part of the existing release pipeline.

## 0.3.0 — Capture

Keep the last completed phrase exactly as it left the plug-in, then save it as
audio. Add the mixing controls needed to use Livecut as a subtle effect as well
as a full replacement.

### Deliverables

- Bounded retrospective recording of complete, naturally completed phrases.
- Capture and save to stereo 32-bit float WAV at the processing sample rate.
- Visible readiness, capture, capacity and file-writing states.
- Smoothed linear dry/wet and output trim; defaults preserve the current sound.
- Stable parameter IDs/order and explicit state/algorithm version properties.

### Contracts

- Capture records final output, after wet/dry and trim.
- A partial phrase caused by starting mid-phrase, seeking, stopping or unsupported
  timing cannot masquerade as a completed phrase.
- Phrase events have sample offsets. A host buffer can contain several events.
- The audio callback neither allocates nor waits for UI/file work. Capture data
  handed to an exporter remains immutable until ownership returns.
- Bounds and busy states are visible. No silent truncated export or substitution.
- Saving audio is explicit. Captured audio is not silently embedded in DAW state.

### Release gate

Failing tests precede implementation. Verify phrase-boundary accuracy across
buffer sizes, discontinuities and supported rates; immutable capture ownership
under export; exported WAV samples/rate/channel count; dry/wet and gain behavior;
old-state recall; UI controls. Run normal CI and plugin validators. Exercise the
actual editor and export/import workflow in an available DAW, recording exactly
what was and was not verified.

Hold, native file drag-out, time-stretching and look-ahead are outside this version.

## 0.4.0 — Perform

Turn captured audio into a playable loop and make groove adjustable. Establish
source addressing once, so later features do not each invent a new player.

### Deliverables

- Audio Hold using a complete captured phrase, with explicit quantised entry,
  release, seam handling and unavailable-capture behavior.
- Swing and bounded accent/humanise controls whose neutral values preserve
  current timing and random sequences.
- Source-region playback: source start/extent/direction separate from output
  duration, with explicit history availability and memory ownership.
- Editor state that distinguishes live input, a pending command and held audio.

### Contracts

- A source region refers to stable audio, never an ambiguous wrapped ring index.
- Hold is audio looping, not a promise that freezing a seed freezes the sound.
- Timing transforms preserve block/phrase endpoints and legal positive durations.
- Added randomness is isolated from legacy random draws; disabled new controls
  leave old sessions unchanged.
- Tempo changes, host stops and seeks have defined Hold behavior. Any initial
  limitations must be documented and surfaced, rather than implicitly resampled
  or silently released.
- The source model is small and specific to known operations; no general graph
  framework or unrelated engine rewrite.

### Release gate

Prove legacy output equivalence in neutral settings; source bounds and lifetime;
Hold entry/release, loop seams and transport transitions; swing totals and
deterministic accents; state/automation compatibility. Run full CI/validators and
hands-on Hold/groove checks. Land 0.3 before starting the release branch from the
new upstream default branch.

Pattern-lock/mutation banks, MIDI routing changes and native drag-out are optional
future projects, not substitutes for the deliverables above.

## 0.5.0 — Recut

Add an explicit buffered Studio mode for operations that need complete source
audio, while retaining the immediate Live mode. Let analysis improve where cuts
read, rather than claiming perfect drum classification.

### Deliverables

- Studio look-ahead with a bounded prepared buffer and correct host latency.
- Reverse playback of complete available source regions.
- Onset markers and attack-aligned slice selection with documented sensitivity
  and timing behavior; no claim of kick/snare classification.
- Visible mode, latency, readiness and unsupported-operation state.
- Aligned dry path and defined bypass/transport behavior.

### Contracts

- Look-ahead cannot eliminate monitoring latency. The UI reports actual added
  delay; existing sessions default to Live mode.
- Prefer a stable sample latency during playback. Tempo-dependent musical source
  spans must fit the prepared budget, with explicit handling when they do not.
- Audio, transport and control changes share a timestamp model. Delayed playback
  uses the corresponding musical position and control events.
- Reverse requires a complete source region. Detection time includes analysis
  delay; zero-latency mode cannot repair a first attack retrospectively.
- Detection identifies candidate attacks, not instrument identity. Detection
  confidence/availability must not silently select an unrelated source.
- No time-stretch library, ML model or stem-separation dependency in this release.

### Release gate

Test actual impulse delay against reported latency; alignment of wet/dry/bypass;
reverse source bounds; onset timing with synthetic and suitable drum fixtures;
sample-rate/block-size changes; stops, seeks, loops and tempo transitions;
delayed automation; old-state default behavior and callback allocation bounds.
CI/validators must pass. Test host playback/monitoring and offline rendering in
available hosts; distinguish automated evidence from listening judgments.

## Execution and acceptance

- Commander coordinates GPT-6.1 Sol workers at high or extra-high reasoning.
- Use separate managed worktrees and `codex/` feature branches. Each independent
  unit starts from the latest upstream default; each version lands before its
  dependent successor. Never change shared `main` directly or force-push it.
- Use RED/GREEN tests for new behavior. Compile the targeted C++ tests and release
  binaries necessary for this explicitly requested implementation/release work.
  Do not run development servers or unrelated build commands.
- Review immutable commits, fix material findings, and reassess the architecture
  when repeated findings expose the same flawed assumption.
- Push branches, open/attach PRs, monitor checks and reviews, then merge the
  verified head through GitHub without bypassing protections.
- Update CMake's embedded version for every release. A `v0.x.0` tag must resolve to
  the reviewed version commit. Do not replace an existing release tag.
- Trigger the existing tag-driven Release workflow only after normal CI passes.
  Verify macOS signing/notarisation, both uploaded assets, release notes and
  `draft: true`. A notes-only draft is not a completed packaged release.
- Preserve each earlier version's draft while later versions are implemented.
- Manual computer use follows the user's GPT-5.6 Sol preference. Never label an
  automated processor test or successful startup as a listening test.

## Status

- [x] Goal created; current release pipeline inspected.
- [x] Baseline v0.2.0 and existing macOS/Windows release assets verified.
- [ ] 0.3.0 implementation, review, validation and packaged draft.
- [ ] 0.4.0 implementation, review, validation and packaged draft.
- [ ] 0.5.0 implementation, review, validation and packaged draft.

Detailed test evidence and known limitations belong in each version's release
notes and validation record. Feature scope may be refined to uphold these
contracts; material removals must be called out rather than silently omitted.

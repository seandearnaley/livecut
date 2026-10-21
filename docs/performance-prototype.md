# Livecut Perform prototype

## Musical brief

Build one playable path for expressive break performance, with precision and audio fidelity as explicit acceptance gates. The user's references are Aphex Twin, Squarepusher and Noisia: exploratory rhythmic relationships, articulate break phrasing and sculpted physical impact. These are listening directions, not artist-imitation presets or a claim that an algorithm has taste.

The 0.6–0.8 processors remain available. This prototype adds a separate Perform instrument rather than mapping an XY widget onto legacy block-frozen FX settings. A captured source, rhythmic event plan and continuous texture controls have distinct ownership.

## First complete workflow

1. Select Perform and prepare while the host is stopped. The existing Live/Studio parameter keeps its old values; Perform has a separate prepared switch.
2. Start playback. Input is explicitly monitored while the engine waits for and captures one complete host bar. The bar must fit within eight seconds.
3. Play Rhythm XY: Variation chooses related source rearrangements; Density changes detail around protected quarter-bar anchors. Rhythm changes enter at the next 1/8-bar boundary on a 32-step grid. These are metrical anchors, not automatic kick/snare labels.
4. Play Texture XY: Grain controls the granular contribution; Tail controls its character. Stereo-linked texture preserves a defined attack interval and smooths continuous targets.
5. Hold Fill for a temporary rhythmic excursion. Keep stores the current four pad targets and seed. Mutate requests a new seed on the next bar. Return restores the kept public targets; texture glides, rhythm changes at its step boundary and seed changes at the next bar. Transport is not rewound.
6. Grab bar arms a fresh complete-bar capture. The previous source stays in use until the new source is complete. Output Capture/Save WAV remains separate and records what was heard.

The four pad axes, Fill and seed support host automation. Independent axis sliders provide precise text/keyboard/accessibility control. Direct plug-in MIDI learn, four scene slots, automatic drum-class classification and independent pitch/time stretching are outside this first prototype.

The processor reads host parameter targets at callback boundaries. Rhythm then commits on its musical boundary; texture uses a 3 ms exponential time constant, rather than reaching its target completely in 3 ms. Offline gesture tests split callbacks at the specified sample timestamps. They do not establish sample-offset automation delivery by every host/wrapper.

## Precision and state contracts

- Perform has zero declared added input latency. One-bar capture readiness is explicit, not hidden latency compensation.
- The source is immutable until a complete recapture is committed. Seek, stop/restart, state restore and preparation invalidate the old source explicitly.
- Timing and source coordinates are separate. Pattern randomness is deterministic from the public seed and musical context.
- At a unity source rate and integer source coordinates, neutral playback uses direct source samples. Fractional coordinates and tempo changes use filtered varispeed; tempo changes also change pitch. Independent time stretching is not claimed. A discontinuity present in the original neutral loop remains part of that recording; the engine does not alter its attacks with a corrective DC ramp.
- Grain reads reuse prepared bandlimited interpolation. Attack protection, boundary transitions, stereo linkage and bounded voice work receive dedicated tests.
- Three source banks keep an outgoing read head immutable during a source-change crossfade. Source PCM uses about 8.8 MiB at 48 kHz or 70.3 MiB at 384 kHz, plus interpolation and stream resources. All are allocated during preparation.
- No allocation, file I/O or waiting occurs in the audio callback. Source memory is prepared ahead of processing. Phrase completion is dispatched after its final output sample, so Capture does not require a sample from the following bar.
- Perform and the prepared Spectral engine are incompatible in this prototype. The convenience Prepare action explicitly disables Spectral; an incompatible restored configuration reports an error.
- Host bypass selects unity raw audio while the Perform timeline continues. It does not discard the captured source.
- Keep stores control targets, not captured PCM. Its home snapshot is serialized. A fresh instance must acquire fresh source audio.
- Existing parameter IDs, ordering and normalized Live/Studio choices remain stable. Seven new parameters append with version hint9; state schema9 and legacy cutting algorithm2.

## Validation and listening

Use RED/GREEN tests for complete capture, source identity, neutral PCM, sample-timed control streams, buffer partition invariance, protected attacks, boundary continuity, stereo relationships, bounded allocation-free processing, host mode lifecycle, saved state and UI gesture pairing.

Render a repeatable audition set from a documented synthetic percussion source: dry, rhythm, texture, combined XY movement and performance actions. Retain raw Float32 renders plus separately labelled listening copies using one constant gain per file. Report measured level differences and exact render identities. Synthetic correctness does not establish musical quality on recorded breaks.

The musical gate requires listening on suitable material: a recognizable pulse, useful surprises, intact hit weight, attractive tails, predictable pad regions and a convincing return. Listener observations and decisions must be recorded. No test count, finite-output check or waveform image substitutes for that judgment.

## Delivery

Use the dedicated `LIVECUT_PERFORMANCE_PROTOTYPE` build option for a distinct product/bundle/plugin identity: Livecut Perform Prototype. Keep the installed original Livecut and all existing draft releases unchanged. Deliver the prototype, source branch, plan, verification record and audition material. This work does not authorize publication of a new release.

For local reproduction, configure with `-DLIVECUT_PERFORMANCE_PROTOTYPE=ON -DLIVECUT_COPY_PLUGIN_AFTER_BUILD=OFF`, build the normal CMake targets and run CTest. The host-only engine tests accept `[performance]`; the processor runner accepts `[performance],[editor]`. The optional audition render is `LIVECUT_PERFORMANCE_RENDER_DIR=/absolute/new-directory ./LivecutTests '[.performance-render]'`. It creates raw and constant-gain listening WAVs with a sample-timestamped manifest. The directory must not already exist.

The standalone application is useful for inspecting the editor, but Perform requires tempo, bar position and running transport from a DAW. Use the VST3 or AU for musical playback. Locally ad-hoc signed prototype bundles are not Developer ID signed or notarized release installers.

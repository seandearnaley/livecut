# Immutable neutral reference

These four files preserve `src/engine/Cutter.{h,cpp}` and
`src/engine/LivePlayer.{h,cpp}` from Livecut 0.3.0 commit
`f054b36e232ecd20f1986d5856f42bdcf96bc9bf`.

Only the namespace, include paths and provenance comments differ. They compile
into the engine tests as `livecut::engine::legacy`, so old/new output comparisons
use the same compiler/libm on each CI platform. The cut procedures, RNG, phrase
seed, cut helpers and effect primitives are shared and must remain unchanged for
this oracle to retain its stated scope. If those change, preserve their baseline
implementations too before claiming 0.3 equivalence.

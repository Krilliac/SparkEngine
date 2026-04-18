# Neural training substrate — extended in-house system (2026-04-18)

**Type:** Observation + Pattern

## Summary

Added a shared CPU neural-network training substrate to `Graphics/Neural/`
and wired it into the two existing online-training consumers. Previously,
training was duplicated (NTC had vanilla SGD + backprop inline; NRC used a
stochastic finite-difference approximation at ~O(params × samples) cost).
Now there is one real backprop implementation with Adam + SGD optimisers
and MSE/L1/Huber losses.

## Files

- `SparkEngine/Source/Graphics/Neural/CpuNeuralTraining.{h,cpp}` — new.
  Provides `Trainer`, `ComputeLoss`, `LossType`, `SGDConfig`, `AdamConfig`,
  `Trainer::GradientCheck` (numerical vs analytical validation).
- `SparkEngine/Source/Graphics/Neural/NeuralFunctionApproximator.{h,cpp}` — new.
  Generic facade for subsystems that want a learned CPU function: `Configure` /
  `TrainOffline` / `TrainIncremental` / `Query` / `Save` / `Load`. Reuses the
  same AlignedWeightLayout inference path so forward passes stay SIMD-fast.
- `SparkEngine/Source/Graphics/Neural/NeuralWeights.{h,cpp}` — .nnw bumped to v2
  with optional optimizer-state trailer (Adam m/v, SGD momentum, step counter).
  v1 files still load with `optimizer.kind == None`.
- `SparkEngine/Source/Graphics/Neural/NeuralTextureCompressor.cpp` — `TrainBlock`
  refactored from ~130 lines of inline SGD+backprop to ~40 lines using `Trainer`
  + Adam. Same iteration budgets; Adam converges faster per-iter than the old
  vanilla SGD loop.
- `SparkEngine/Source/Graphics/Neural/NeuralRadianceCache.{h,cpp}` — `Update`
  replaced the finite-difference MLP weight search + ad-hoc hash-grid update
  with real analytical backprop. Adam state persists across frames via a
  `std::unique_ptr<Trainer>` member. Hash-grid features get their exact
  analytical gradient via the new `outInputGradient` parameter of
  `Trainer::AccumulateGradient`. Re-upload reduced from once-per-weight to
  once-per-batch. Also replaced the bespoke Xavier init with
  `Trainer::InitializeWeightsXavier` (removed `<random>` dep).
- `Tests/TestCpuNeuralTraining.cpp` — new, 13 tests:
  - Loss + gradient correctness for MSE / L1 / Huber
  - Numerical gradient check for MSE and Huber end-to-end (relErr < 1e-2)
  - Adam + SGD+momentum convergence on XOR
  - NFA sine-fit convergence
  - NFA incremental training reduces loss + learns x·y
  - NFA save/load round-trip identical predictions
  - .nnw v2 optimizer-state round-trip
  - .nnw v1 backwards-compat (hand-written legacy file loads cleanly)
  - NRC loss decreases under real-backprop path

## Design decisions

1. **Flat weight layout reused as-is.** The trainer operates on the same
   layer-major weight buffer `NetworkDesc` uses for inference. Gradients are a
   parallel buffer of identical shape. This keeps the serialisation story
   simple and avoids introducing a competing format.

2. **Thread-local scratch for steady-state allocation-free training.**
   `Trainer::AccumulateGradient` uses `thread_local std::vector`s for per-layer
   outputs, input-snapshot pointers, and delta. First call allocates; steady
   state is zero-alloc. Suitable for per-frame NRC updates.

3. **`outInputGradient` on AccumulateGradient** is the key hook that lets NRC
   get real hash-grid gradients. Previously NRC used a hand-waved
   `grad = mean(error) * 0.1` — now it receives the actual ∂loss/∂input
   decomposed per feature. The first `kHashGridLevels * kFeaturesPerEntry`
   entries correspond to hash features; the trailing 3 are direction (ignored).

4. **Adam state persists across frames in NRC** via a `std::unique_ptr<Trainer>`
   member so the first-/second-moment buffers accumulate a smooth signal. The
   previous code had no momentum-like state at all.

5. **NeuralFunctionApproximator is the "use this from anywhere" primitive.**
   Any subsystem wanting a learned CPU approximation should use NFA rather
   than hand-rolling training. Hidden layers ReLU, output default linear;
   `SetOutputActivation(Sigmoid)` for bounded [0,1] targets. Lazy layout
   rebuild on weight change keeps `Query()` fast.

6. **v2 .nnw optimizer trailer is optional** (flag bit). Ship weights without
   it for smaller files; carry it for resumeable training workflows.
   Backwards-compat preserved — older v1 files load with `kind == None`.

## Non-goals / what was deliberately skipped

- **No GPU backward pass.** The existing GPU compute shaders are inference-only.
  A GPU backprop would require significant shader work; out of scope. Training
  runs on CPU (SIMD forward pass, scalar backward).
- **No DynamicQualityScaler or BlendSpace wiring.** Both were considered and
  rejected: BlendSpace's barycentric search (~60 flops for typical small
  spaces) is already cheaper than a 2→16→16→N MLP (~450 flops), so a learned
  version would regress perf. DQS has no natural supervision signal — wiring
  NFA against the existing heuristic would be circular. NFA is available for
  future use cases when a real training signal materialises.
- **Optimizer-state save in NFA::Save(..., true)** is a no-op pending a
  `Trainer` accessor for internal moment buffers; left as TODO since no
  caller needs it today. The format supports it when needed.

## Test results

`cmake --preset linux-gcc-release && cmake --build build/linux-gcc-release
--target SparkTests` → 5648/5648 pass including all 13 new training tests.
Slowest new test: `NFA_LearnsSineFunction` at 13.5s (800 epochs × 128
samples × a [1→32→32→1] MLP — acceptable for a convergence test).

## When to add a neural function to a system

Use the checklist:
1. Is the current function **expensive**? (iterative solver, many transcendentals,
   O(N) search per query). If no, skip — a small MLP is ~500 flops minimum.
2. Is a **smooth approximation** acceptable (not bit-exact)?
3. Can you generate **training data**? (offline reference or gameplay trace)
4. Is **determinism across platforms** NOT required? (NFs use floating-point
   accumulation order — save games / networking / physics replay are OFF-limits.)

If all four are yes, `NeuralFunctionApproximator` is the path. Otherwise, stick
with the hand-written version.

## Follow-ups (optional)

- **Expose Adam moments from Trainer** so NFA can actually persist optimizer
  state on `Save(..., true)`. Needs a read accessor + setter.
- **Mini-batch Adam helper** — right now NTC accumulates full-batch gradients
  and calls StepAdam(1/N); a `TrainMiniBatch` helper would tidy this pattern.
- **GPU backward pass (compute shader).** Only worth it if a heavy online
  workload emerges (e.g. a per-frame neural physics LOD with 10K+ samples).
  For now, CPU training + GPU inference is the right balance.

# SparkEngine Animation — Gap Analysis

> **Scope**: `SparkEngine/Source/Engine/Animation/` (AnimationSystem.h/cpp)
> **Date**: 2026-03-09
> **Methodology**: Static source-code audit of `AnimationSystem.h` and `AnimationSystem.cpp`.
> Each gap is assigned a severity: **Critical**, **Major**, **Moderate**, **Minor**.

---

## Context

The Animation subsystem has an ambitious and well-designed architecture declared in `AnimationSystem.h`:
- **Skeleton/Bone hierarchy** with offset matrices and bind poses
- **AnimationClip** with per-bone keyframe tracks (position, rotation, scale)
- **AnimationInstance** per-entity runtime data
- **AnimationStateMachine** with states, transitions, and condition-based blending
- **AnimationLayer** for layered blending (full body, upper body, override, additive)
- **AnimationEvaluator** with clip sampling, transform blending, skinning matrix computation
- **IK solvers** (two-bone IK, CCD IK declared)
- **AnimationManager** singleton for asset caching

However, only 2 source files exist (`.h` and `.cpp`), and the `.cpp` has only 1 stub pattern match — suggesting the header is heavily documented but the implementation may be thin.

---

## Critical Gaps

### GAP-AN01 — No Animation Asset Loading (FBX/GLTF Import)

**Files**: `Engine/Animation/AnimationSystem.h` (AnimationManager)

**Impact**: `AnimationManager::LoadSkeleton()` and `AnimationManager::LoadAnimations()` are declared but there is no FBX or GLTF parser implementation. Without this, skeletons and animation clips cannot be loaded from artist-created files.

**Evidence**: The header documents loading from FBX/GLTF but the engine has no dependency on Assimp, OpenFBX, cgltf, or any mesh/animation import library in the CMakeLists.

**What is needed**: Integrate Assimp (or a lightweight alternative like cgltf for GLTF) to extract:
- Bone hierarchy (names, parent indices, offset matrices)
- Animation clips (per-bone keyframe tracks)
- Bind pose transforms

---

### GAP-AN02 — No GPU Skinning Shader or Bone Buffer Upload

**Files**: `Engine/Animation/AnimationSystem.h` (AnimationEvaluator)

**Impact**: `AnimationEvaluator::ComputeSkinningMatrices()` is declared to produce final bone matrices, but there is no corresponding:
- Per-entity bone matrix constant buffer (GPU resource)
- Vertex shader that reads bone matrices and applies skinning
- Integration with the `RenderSystem` to bind bone data before drawing

Without GPU skinning, animated meshes cannot deform on screen.

**What is needed**:
- Create a structured buffer or constant buffer holding bone matrices (max 256 bones)
- Write a skinned mesh vertex shader that reads bone weights/indices and applies the transform
- Modify `RenderSystem` to detect `AnimationController` and bind bone data before the draw call

---

## Major Gaps

### GAP-AN03 — AnimationStateMachine Transition Blending May Be Incomplete

**Files**: `Engine/Animation/AnimationSystem.h`

**Impact**: The state machine declares `AnimationTransition` with crossfade duration and condition callbacks, but the actual blending implementation between outgoing and incoming states during transitions needs verification. If transitions snap rather than blend, character animation will be jerky.

**What is needed**: Verify and implement smooth crossfade blending during state transitions using the declared `transitionDuration` parameter. The evaluator should sample both the outgoing and incoming clips and lerp bone transforms.

---

### GAP-AN04 — IK Solvers Declared But Integration Unclear

**Files**: `Engine/Animation/AnimationSystem.h`

**Impact**: `AnimationEvaluator::SolveTwoBoneIK()` is declared for common IK use cases (foot placement, hand targeting, aim adjustment). However, integration with the animation pipeline — specifically when IK is applied relative to the blended result and how IK targets are set — is unclear.

**What is needed**:
- Implement foot IK for ground conformance (essential for FPS on uneven terrain)
- Implement aim IK for look-at/weapon targeting
- Apply IK as a post-process after animation evaluation but before GPU upload

---

### GAP-AN05 — No Root Motion Support

**Files**: `Engine/Animation/AnimationSystem.h`

**Impact**: No mechanism exists to extract root bone translation from animation clips and apply it as entity movement. Without root motion, character locomotion is driven entirely by code velocity, which can cause foot sliding.

**What is needed**: Extract per-frame root bone delta (position + rotation), zero out the root bone in the animation, and return the delta to the movement system. Add a `rootMotionEnabled` flag to `AnimationController`.

---

### GAP-AN06 — No Additive Animation Implementation

**Files**: `Engine/Animation/AnimationSystem.h`

**Impact**: `AnimationLayer` declares `BlendMode::Additive` but the actual additive blending math (computing the difference from a reference pose and adding it to the base) may not be implemented. Additive animations are essential for layered effects (breathing on top of locomotion, hit reactions, lean).

**What is needed**: Implement additive blending: `result = base + (additive - referencePose)`. Store reference pose per additive clip.

---

## Moderate Gaps

### GAP-AN07 — No Animation Events / Notifies

**Files**: `Engine/Animation/AnimationSystem.h`

**Impact**: No system for firing callbacks at specific keyframe times (e.g., "footstep sound at frame 12", "spawn projectile at frame 24", "enable hitbox at frame 8"). Gameplay code cannot synchronize with animation playback.

**What is needed**: Add an `AnimationEvent` struct with (time, eventName, parameters). Store events per clip. Fire callbacks when playback crosses event timestamps.

---

### GAP-AN08 — No Animation Compression

**Files**: `Engine/Animation/AnimationSystem.h`

**Impact**: Keyframe data is stored as raw `XMFLOAT3` (position, scale) and `XMFLOAT4` (rotation quaternion) per bone per keyframe. For a character with 80 bones and 30 FPS clips, this is ~3.8KB per frame. A 10-second clip = ~1.14MB per clip with no compression.

**What is needed**: Implement keyframe reduction (remove redundant keys), quaternion compression (smallest-3 encoding), and uniform/variable quantization.

---

### GAP-AN09 — No Blend Tree / Blend Space

**Files**: `Engine/Animation/AnimationSystem.h`

**Impact**: While `AnimationLayer` supports blending between clips, there is no 1D/2D blend space (e.g., blend between walk/run based on speed, or blend between strafe directions based on movement angle). This is essential for smooth FPS locomotion.

**What is needed**: Implement a `BlendSpace1D` (parameter → clip weights) and `BlendSpace2D` (2 parameters → triangulated clip weights) that can be used as state machine state sources.

---

### GAP-AN10 — AnimationManager Is a Singleton

**Files**: `Engine/Animation/AnimationSystem.h`

**Impact**: `AnimationManager::GetInstance()` uses the singleton pattern rather than the `EngineContext` service locator, inconsistent with the project's architecture.

**What is needed**: Register with `EngineContext` for testability and consistency.

---

## Minor Gaps

### GAP-AN11 — No Animation LOD

**Impact**: All entities evaluate full animation regardless of distance from camera. Distant characters could use reduced bone sets or lower tick rates.

---

### GAP-AN12 — No Montage / Animation Composition System

**Impact**: No system for chaining animation sequences (e.g., "melee combo: swing1 → swing2 → swing3") with branch points and interrupts.

---

### GAP-AN13 — No Morph Target / Blend Shape Support

**Impact**: Only skeletal animation is supported. Facial animation via morph targets is not available.

---

## Summary Table

| ID | Severity | Gap | Impact |
|---|---|---|---|
| GAP-AN01 | Critical | No FBX/GLTF loading | Cannot load animation assets |
| GAP-AN02 | Critical | No GPU skinning pipeline | Animated meshes can't render |
| GAP-AN03 | Major | State machine transition blending | Jerky transitions |
| GAP-AN04 | Major | IK solver integration unclear | No foot/aim IK |
| GAP-AN05 | Major | No root motion | Foot sliding |
| GAP-AN06 | Major | No additive animation | No layered effects |
| GAP-AN07 | Moderate | No animation events | No gameplay sync |
| GAP-AN08 | Moderate | No animation compression | High memory usage |
| GAP-AN09 | Moderate | No blend spaces | Limited locomotion |
| GAP-AN10 | Moderate | Singleton pattern | Architecture inconsistency |
| GAP-AN11 | Minor | No animation LOD | Performance at scale |
| GAP-AN12 | Minor | No montage system | No combo sequences |
| GAP-AN13 | Minor | No morph targets | No facial animation |

---

## Recommended Priority Order

1. **GAP-AN01** — FBX/GLTF loading (unblocks all animation work)
2. **GAP-AN02** — GPU skinning pipeline (unblocks visual results)
3. **GAP-AN05** — Root motion (FPS locomotion quality)
4. **GAP-AN03** — Transition blending verification
5. **GAP-AN04** — Foot IK and aim IK
6. **GAP-AN07** — Animation events (gameplay integration)
7. **GAP-AN09** — Blend spaces (locomotion quality)
8. Everything else

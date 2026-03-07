# Animation

SparkEngine provides a full skeletal animation system with bone hierarchies, state machines, multi-layer blending, inverse kinematics, and root motion support.

**Source:** `SparkEngine/Source/Engine/Animation/AnimationSystem.h`

## Overview

The animation system supports:
- Skeletal animation with bone hierarchies
- Keyframe animation clips
- Animation state machines with cross-fading
- Multi-layer blending (override, additive, layered with per-bone masks)
- Inverse kinematics (two-bone, look-at, FABRIK)
- Root motion extraction
- FBX and glTF import via Assimp

## Skeletal Animation

### Bone Hierarchies

Skeletons are imported from FBX/glTF files via the asset pipeline. Each bone has:
- Name
- Parent index (-1 for root)
- Bind pose transform (local and inverse bind matrix)

### Animation Clips

Clips contain keyframe data for bones:
- Position keyframes (translation over time)
- Rotation keyframes (quaternion over time)
- Scale keyframes (scaling over time)

Keyframes are interpolated (lerp for position/scale, slerp for rotation).

## State Machines

Animation state machines manage transitions between animation clips with configurable cross-fading:

```
  ┌───────┐   walk trigger   ┌───────┐
  │ Idle  │ ──────────────── │ Walk  │
  │       │ ◄─────────────── │       │
  └───────┘   idle trigger   └───────┘
      │                          │
      │ jump trigger             │ run trigger
      ▼                          ▼
  ┌───────┐                  ┌───────┐
  │ Jump  │                  │  Run  │
  └───────┘                  └───────┘
```

### Transitions

- **Cross-fade duration** — Time to blend between states (seconds)
- **Transition conditions** — Trigger names, parameter thresholds
- **Exit time** — Optional: wait for current clip to finish before transitioning

## Multi-Layer Blending

Multiple animation layers can be combined:

| Mode | Description |
|------|-------------|
| **Override** | Layer completely replaces the base pose for affected bones |
| **Additive** | Layer is added on top of the base pose |
| **Layered** | Per-bone weight masks for selective blending |

Use per-bone masks to, for example, play an upper-body attack animation while the lower body continues a walk cycle.

## Inverse Kinematics

Three IK solvers are available:

### Two-Bone IK

For limbs (arms, legs):
```
Shoulder ─── Elbow ─── Hand → Target
```
Solves for natural joint angles to reach a target position.

### Look-At IK

Rotates a bone (typically the head) to face a target:
```
Head bone → Look at target position
```

### FABRIK (Forward And Backward Reaching IK)

Iterative multi-joint solver for chains of any length:
```
Root ─── Joint1 ─── Joint2 ─── ... ─── End Effector → Target
```
Supports joint constraints and converges quickly (typically 5-10 iterations).

## Root Motion

Root motion extraction moves the character based on animation data rather than gameplay code:
- Translation from the root bone drives character movement
- Rotation from the root bone drives character facing
- Useful for realistic walk/run cycles, combat animations

## ECS Integration

Use `AnimationController` on entities (see [Entity Component System](Entity-Component-System)):

```cpp
auto& anim = world.AddComponent<AnimationController>(entity);
anim.currentAnimation = "Idle";
anim.playbackSpeed    = 1.0f;
anim.isPlaying        = true;
anim.loop             = true;
anim.blendFactor      = 1.0f;
```

The `AnimationUpdateSystem` evaluates state machines and blends poses each frame.

## Asset Import

Models with animations are imported via Assimp (see [Asset Pipeline](Asset-Pipeline)):
- **FBX** — Industry standard, supports skeletons and animations
- **glTF** — Modern format with PBR materials and animations

---

## See Also

- [Entity Component System](Entity-Component-System) — AnimationController component
- [Asset Pipeline](Asset-Pipeline) — Importing animated models
- [SparkEditor](SparkEditor) — Animation timeline editor
- [Rendering and Graphics](Rendering-and-Graphics) — Skinned mesh rendering
- [AI and Navigation](AI-and-Navigation) — NPC animation integration
- [Physics](Physics) — Root motion and physics interaction
- [Gameplay Systems](Gameplay-Systems) — Player and weapon animations
- [Cinematic Sequencer](Cinematic-Sequencer) — Animation in cinematics

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

```cpp
// Load a skeleton from an asset file
auto& animMgr = AnimationManager::GetInstance();
animMgr.LoadSkeleton("soldier", "Assets/Models/Soldier.fbx");

// Query bone information
const Skeleton* skel = animMgr.GetSkeleton("soldier");
int boneCount = skel->GetBoneCount();
int headIdx   = skel->FindBone("Head");  // -1 if not found
```

### Animation Clips

Clips contain keyframe data for bones:
- Position keyframes (translation over time)
- Rotation keyframes (quaternion over time)
- Scale keyframes (scaling over time)

Keyframes are interpolated (lerp for position/scale, slerp for rotation).

```cpp
// Load animation clips for a skeleton
animMgr.LoadAnimations("soldier", "Assets/Animations/Soldier_Anims.fbx");

// Retrieve a clip by name
const AnimationClip* walkClip = animMgr.GetClip("Walk");
float duration = walkClip->duration;
bool loops     = walkClip->loop;

// Register a custom clip
AnimationClip customClip;
customClip.name = "CustomIdle";
customClip.duration = 2.0f;
customClip.loop = true;
animMgr.RegisterClip("CustomIdle", customClip);
```

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

### Building a State Machine

```cpp
AnimationStateMachine sm;

// Add states (each maps to an animation clip)
sm.AddState("Idle", "Idle");
sm.AddState("Walk", "Walk");
sm.AddState("Run",  "Run");
sm.AddState("Jump", "Jump");
sm.SetDefaultState("Idle");

// Add transitions with cross-fade durations
sm.AddTransition("Idle", "Walk", "walk",  0.2f);  // trigger, blend time
sm.AddTransition("Walk", "Idle", "idle",  0.2f);
sm.AddTransition("Walk", "Run",  "run",   0.15f);
sm.AddTransition("Idle", "Jump", "jump",  0.1f);

// Drive the state machine each frame
sm.Update(deltaTime);

// Query current state
std::string current = sm.GetCurrentStateName();
bool blending = sm.IsTransitioning();
float blend   = sm.GetBlendFactor();

// Force an immediate state change (no transition)
sm.ForceState("Idle");
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

```cpp
// Set up a two-layer animation instance
AnimationInstance instance;
instance.skeleton = animMgr.GetSkeleton("soldier");

// Base layer: full-body walk cycle
AnimationLayer baseLayer;
baseLayer.clipName  = "Walk";
baseLayer.weight    = 1.0f;
baseLayer.blendMode = BlendMode::Override;
baseLayer.playing   = true;
baseLayer.loop      = true;
baseLayer.speed     = 1.0f;
instance.layers.push_back(baseLayer);

// Upper-body layer: reload animation (only affects spine and arms)
AnimationLayer upperLayer;
upperLayer.clipName  = "Reload";
upperLayer.weight    = 1.0f;
upperLayer.blendMode = BlendMode::Override;
upperLayer.playing   = true;
upperLayer.loop      = false;
upperLayer.boneMask  = {"Spine", "Spine1", "Spine2",
                        "LeftArm", "LeftForeArm", "LeftHand",
                        "RightArm", "RightForeArm", "RightHand"};
instance.layers.push_back(upperLayer);

// Update each frame
instance.Update(deltaTime);
instance.UpdateLayers(deltaTime);
```

## Inverse Kinematics

Three IK solvers are available:

### Two-Bone IK

For limbs (arms, legs):
```
Shoulder ─── Elbow ─── Hand → Target
```
Solves for natural joint angles to reach a target position.

```cpp
// Attach a two-bone IK chain for the right arm
IKChain rightArm;
rightArm.name       = "RightArmIK";
rightArm.type       = IKType::TwoBone;
rightArm.boneIndices = {skel->FindBone("RightArm"),
                        skel->FindBone("RightForeArm"),
                        skel->FindBone("RightHand")};
rightArm.targetPosition = weaponGripPos;
rightArm.poleVector     = elbowHintPos;
rightArm.weight         = 1.0f;
rightArm.enabled        = true;
instance.ikChains.push_back(rightArm);
```

### Look-At IK

Rotates a bone (typically the head) to face a target:
```
Head bone → Look at target position
```

```cpp
// Make the character look at a target
IKChain lookAt;
lookAt.name       = "HeadLookAt";
lookAt.type       = IKType::LookAt;
lookAt.boneIndices = {skel->FindBone("Head")};
lookAt.targetPosition = enemyPosition;
lookAt.weight         = 0.8f;  // partial blend with base animation
lookAt.enabled        = true;
instance.ikChains.push_back(lookAt);
```

### FABRIK (Forward And Backward Reaching IK)

Iterative multi-joint solver for chains of any length:
```
Root ─── Joint1 ─── Joint2 ─── ... ─── End Effector → Target
```
Supports joint constraints and converges quickly (typically 5-10 iterations).

```cpp
// Tentacle or tail chain using FABRIK
IKChain tail;
tail.name           = "TailIK";
tail.type           = IKType::FABRIK;
tail.boneIndices    = {skel->FindBone("Tail1"), skel->FindBone("Tail2"),
                       skel->FindBone("Tail3"), skel->FindBone("Tail4")};
tail.targetPosition = swayTarget;
tail.maxIterations  = 10;
tail.tolerance      = 0.01f;
tail.weight         = 1.0f;
tail.enabled        = true;
instance.ikChains.push_back(tail);
```

## Root Motion

Root motion extraction moves the character based on animation data rather than gameplay code:
- Translation from the root bone drives character movement
- Rotation from the root bone drives character facing
- Useful for realistic walk/run cycles, combat animations

```cpp
// After updating the animation instance, apply root motion
instance.Update(deltaTime);

// Root motion delta is computed from the root bone's movement this frame
XMFLOAT3 moveDelta = instance.rootMotionDelta;
transform.position.x += moveDelta.x;
transform.position.y += moveDelta.y;
transform.position.z += moveDelta.z;
```

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

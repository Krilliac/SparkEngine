# Animation

SparkEngine provides a full skeletal animation system with bone hierarchies, state machines, multi-layer blending, inverse kinematics, and root motion support.

**Source:** `SparkEngine/Source/Engine/Animation/AnimationSystem.h`

## Overview

```
AnimationManager (singleton asset cache)
    ├── Skeleton       (bone hierarchy, shared across instances)
    └── AnimationClip  (keyframe data, loaded from FBX/GLTF)

Per-entity runtime data:
    AnimationInstance
        ├── AnimationStateMachine  (state transitions, crossfade)
        ├── AnimationLayer[]       (blend layer stack)
        ├── IKChain[]             (IK post-processing)
        └── BlendResult           (final bone matrices → GPU)

Processing pipeline (per frame):
    AnimationEvaluator::SampleClip()              → local bone transforms
    AnimationEvaluator::BlendTransforms()         → blended local transforms
    AnimationEvaluator::ComputeSkinningMatrices() → GPU skinning matrices
    AnimationEvaluator::Solve*IK()                → IK corrections
```

The animation system supports:
- Skeletal animation with bone hierarchies
- Keyframe animation clips with position, rotation, and scale tracks
- Animation state machines with cross-fading transitions
- Multi-layer blending (override, additive, layered with per-bone masks)
- Inverse kinematics (two-bone, look-at, FABRIK)
- Root motion extraction for locomotion
- FBX and glTF import via Assimp

## Skeletal Animation

### Bone Structure

Each bone in a skeleton is represented by the `Bone` struct:

```cpp
struct Bone
{
    std::string name;           // Matches export name from 3D tool (e.g. "Bip01_R_Hand")
    int32_t parentIndex = -1;   // Parent index (-1 for root bone)
    XMFLOAT4X4 offsetMatrix;   // Inverse bind pose (mesh-to-bone space)
    XMFLOAT4X4 localBindPose;  // Rest pose relative to parent
};
```

| Field | Description |
|-------|-------------|
| `name` | Human-readable name matching the authoring tool export |
| `parentIndex` | Index of parent bone (-1 for root); forms a tree hierarchy |
| `offsetMatrix` | Inverse bind pose matrix; transforms vertices from model space to bone space |
| `localBindPose` | Bone's transform relative to parent in the rest/bind pose |

### Skeleton Structure

```cpp
struct Skeleton
{
    std::string name;
    std::vector<Bone> bones;                              // Ordered parent-before-child
    std::unordered_map<std::string, int32_t> boneNameToIndex;  // O(1) lookup

    int32_t FindBone(const std::string& boneName) const;  // Returns -1 if not found
    size_t GetBoneCount() const;                           // Number of bones
};
```

The `bones` array is ordered such that every bone appears AFTER its parent. This guarantees that when computing transforms in index order, a bone's parent world transform is always available.

### Loading Skeletons

```cpp
auto& animMgr = Spark::Animation::AnimationManager::GetInstance();

// Load a skeleton (cached by filepath)
auto skeleton = animMgr.LoadSkeleton("Assets/Models/Soldier.fbx");

// Query bone information
int boneCount = skeleton->GetBoneCount();          // e.g. 65
int headIdx   = skeleton->FindBone("Head");        // Returns index or -1
int spineIdx  = skeleton->FindBone("Bip01_Spine"); // For bone masks
```

## Animation Clips

### Keyframe Types

```cpp
struct VectorKey
{
    float time;        // Time in seconds (sorted ascending)
    XMFLOAT3 value;   // 3D vector (position or scale)
};

struct QuatKey
{
    float time;        // Time in seconds (sorted ascending)
    XMFLOAT4 value;   // Quaternion rotation (x, y, z, w), normalized
};
```

| Keyframe Type | Interpolation | Use |
|---------------|---------------|-----|
| `VectorKey` | Linear (LERP) | Position and scale tracks |
| `QuatKey` | Spherical linear (SLERP) | Rotation tracks (avoids gimbal lock) |

### BoneAnimation (Per-Bone Channel)

```cpp
struct BoneAnimation
{
    std::string boneName;                    // Must match a bone in the target Skeleton
    int32_t boneIndex = -1;                  // Pre-resolved index (cached for performance)
    std::vector<VectorKey> positionKeys;     // Position track (may be empty)
    std::vector<QuatKey> rotationKeys;       // Rotation track (may be empty)
    std::vector<VectorKey> scaleKeys;        // Scale track (may be empty)

    XMFLOAT3 InterpolatePosition(float time) const;  // Binary search + LERP
    XMFLOAT4 InterpolateRotation(float time) const;   // Binary search + SLERP
    XMFLOAT3 InterpolateScale(float time) const;      // Binary search + LERP
};
```

Empty tracks default to the bone's bind pose value, allowing partial animation clips that only animate a subset of channels.

### AnimationClip

```cpp
struct AnimationClip
{
    std::string name;                       // Registration key (e.g. "Run", "Idle")
    float duration = 0.0f;                  // Total duration in seconds
    float ticksPerSecond = 24.0f;           // Source asset playback rate (24, 30, or 60)
    std::vector<BoneAnimation> channels;    // Per-bone animation data
    bool loop = true;                       // Auto-loop at end (false for one-shot)

    const BoneAnimation* FindChannel(const std::string& boneName) const;
};
```

### Loading and Registering Clips

```cpp
auto& animMgr = Spark::Animation::AnimationManager::GetInstance();

// Load all clips from an animation file
auto clips = animMgr.LoadAnimations("Assets/Animations/Soldier_Anims.fbx");

// Register each clip by name for later lookup
for (auto& clip : clips)
    animMgr.RegisterClip(clip->name, clip);

// Retrieve a clip by name
auto walkClip = animMgr.GetClip("Walk");
float duration = walkClip->duration;    // e.g. 1.2 seconds
bool loops     = walkClip->loop;        // true

// Register a custom clip
auto customClip = std::make_shared<Spark::Animation::AnimationClip>();
customClip->name     = "CustomIdle";
customClip->duration = 2.0f;
customClip->loop     = true;
animMgr.RegisterClip("CustomIdle", customClip);
```

## AnimationManager (Singleton Cache)

The `AnimationManager` ensures each unique asset is loaded once. All runtime instances share clips and skeletons via `shared_ptr`.

| Method | Description |
|--------|-------------|
| `GetInstance()` | Access the global singleton |
| `LoadSkeleton(filepath)` | Load/cache skeleton from FBX/GLTF |
| `LoadAnimations(filepath)` | Load all clips from a file (does NOT auto-register) |
| `RegisterClip(name, clip)` | Register a clip by name for lookup |
| `GetClip(name)` | Retrieve a cached clip (nullptr if not found) |
| `GetSkeleton(name)` | Retrieve a cached skeleton |
| `Clear()` | Release all cached assets (shared_ptr holders retain data) |
| `Console_ListAnimations()` | List all registered clip names |
| `Console_ListSkeletons()` | List all loaded skeleton names |

## State Machines

Animation state machines manage transitions between clips with configurable cross-fading.

```
  ┌───────┐   speed > 0.1   ┌───────┐
  │ Idle  │ ──────────────── │ Walk  │
  │       │ ◄─────────────── │       │
  └───────┘   speed < 0.05   └───────┘
      │                          │
      │ jump trigger             │ speed > 5.0
      ▼                          ▼
  ┌───────┐                  ┌───────┐
  │ Jump  │                  │  Run  │
  └───────┘                  └───────┘
```

### AnimationState

```cpp
struct AnimationState
{
    std::string name;       // Unique name (e.g. "Idle", "Run", "Shoot")
    std::string clipName;   // AnimationClip to play (must be registered)
    float speed = 1.0f;     // Playback speed multiplier (1.5 for sprint)
    bool loop = true;       // Loop the clip while in this state
};
```

### AnimationTransition

```cpp
struct AnimationTransition
{
    std::string fromState;                  // Source state ("*" matches any)
    std::string toState;                    // Destination state
    float duration = 0.2f;                  // Crossfade blend duration (seconds)
    std::function<bool()> condition;        // Predicate (first satisfied wins)
    bool hasExitTime = false;               // Wait for clip to reach exitTime
    float exitTime = 1.0f;                  // Normalized exit time [0, 1]
};
```

| Field | Description |
|-------|-------------|
| `fromState` | Source state name; `"*"` matches any state |
| `toState` | Destination state name |
| `duration` | Crossfade blend time (0.1-0.2s for action, 0.3-0.5s for locomotion) |
| `condition` | Lambda returning true to trigger the transition |
| `hasExitTime` | If true, wait until the clip reaches `exitTime` before transitioning |
| `exitTime` | Normalized time threshold [0, 1]; 1.0 = must reach end of clip |

### Building a State Machine

```cpp
Spark::Animation::AnimationStateMachine sm;

// Add states (each maps to an animation clip)
sm.AddState({"Idle", "idle_anim", 1.0f, true});
sm.AddState({"Walk", "walk_anim", 1.0f, true});
sm.AddState({"Run",  "run_anim",  1.0f, true});
sm.AddState({"Jump", "jump_anim", 1.0f, false});  // one-shot
sm.SetDefaultState("Idle");

// Add condition-triggered transitions
sm.AddTransition({"Idle", "Walk", 0.2f, [&]{ return speed > 0.1f; }});
sm.AddTransition({"Walk", "Idle", 0.2f, [&]{ return speed < 0.05f; }});
sm.AddTransition({"Walk", "Run",  0.15f, [&]{ return speed > 5.0f; }});
sm.AddTransition({"Run",  "Walk", 0.15f, [&]{ return speed <= 5.0f; }});

// Exit-time transition: wait for jump to finish before returning to Idle
sm.AddTransition({"Jump", "Idle", 0.1f, nullptr, true, 0.9f});

// Drive the state machine each frame
sm.Update(deltaTime);

// Query current state
std::string current = sm.GetCurrentStateName();   // "Idle"
bool blending       = sm.IsTransitioning();        // false
float blend         = sm.GetBlendFactor();         // 0.0
```

### AnimationStateMachine API

| Method | Description |
|--------|-------------|
| `AddState(state)` | Register a state |
| `AddTransition(transition)` | Register a transition |
| `SetDefaultState(name)` | Set the initial/entry state |
| `Update(deltaTime)` | Advance the machine one frame |
| `GetCurrentStateName()` | Name of the active state |
| `GetCurrentTime()` | Playback time in the current clip |
| `GetBlendFactor()` | Crossfade progress [0, 1] |
| `IsTransitioning()` | Whether a crossfade is active |
| `GetTargetStateName()` | Target state during crossfade |
| `GetTargetTime()` | Playback time in the target clip |
| `GetCurrentClipName()` | Clip name for the current state |
| `GetTargetClipName()` | Clip name for the target state |
| `ForceState(name)` | Immediately switch state (no crossfade) |
| `Console_GetStateInfo()` | Debug info string |

## Multi-Layer Blending

Multiple animation layers can be combined. Layers are processed bottom-to-top (index 0 = base).

### Blend Modes

```cpp
enum class BlendMode
{
    Override,   // Fully replace lower layers for affected bones
    Additive,   // Add delta from bind pose on top of lower layers
    Layered     // Linearly blend with lower layers using weight
};
```

| Mode | Description | Use Case |
|------|-------------|----------|
| **Override** | Completely replaces lower layers | Upper body shooting over lower body walk |
| **Additive** | Adds animation delta on top of base | Breathing cycle, hit reactions |
| **Layered** | Weight-based blend with lower layers | Smooth transitions, partial overrides |

### AnimationLayer

```cpp
struct AnimationLayer
{
    std::string clipName;                  // Clip to play
    float weight = 1.0f;                   // Blend weight [0, 1] (Layered mode)
    float currentTime = 0.0f;              // Playback position (seconds)
    float speed = 1.0f;                    // Speed multiplier (negative = reverse)
    BlendMode blendMode = BlendMode::Override;
    bool playing = true;                   // Whether playback is advancing
    bool loop = true;                      // Loop at end of clip
    std::vector<int32_t> boneMask;         // Affected bone indices (empty = all)
};
```

### Layered Blending Example

```cpp
Spark::Animation::AnimationInstance instance;
instance.skeleton = animMgr.GetSkeleton("soldier").get();

// Base layer: full-body walk cycle
Spark::Animation::AnimationLayer baseLayer;
baseLayer.clipName  = "Walk";
baseLayer.weight    = 1.0f;
baseLayer.blendMode = Spark::Animation::BlendMode::Override;
baseLayer.playing   = true;
baseLayer.loop      = true;
instance.layers.push_back(baseLayer);

// Upper-body layer: reload animation (only affects spine and arms)
Spark::Animation::AnimationLayer upperLayer;
upperLayer.clipName  = "Reload";
upperLayer.weight    = 1.0f;
upperLayer.blendMode = Spark::Animation::BlendMode::Override;
upperLayer.playing   = true;
upperLayer.loop      = false;
upperLayer.boneMask  = {
    skeleton->FindBone("Spine"), skeleton->FindBone("Spine1"),
    skeleton->FindBone("Spine2"),
    skeleton->FindBone("LeftArm"), skeleton->FindBone("LeftForeArm"),
    skeleton->FindBone("LeftHand"),
    skeleton->FindBone("RightArm"), skeleton->FindBone("RightForeArm"),
    skeleton->FindBone("RightHand")
};
instance.layers.push_back(upperLayer);

// Additive breathing layer
Spark::Animation::AnimationLayer breathLayer;
breathLayer.clipName  = "Breathing";
breathLayer.weight    = 0.3f;
breathLayer.blendMode = Spark::Animation::BlendMode::Additive;
breathLayer.playing   = true;
breathLayer.loop      = true;
instance.layers.push_back(breathLayer);

// Update each frame
instance.Update(deltaTime);
```

## Inverse Kinematics

Three IK solvers are available, applied as a post-processing pass after animation blending.

### IK Types

```cpp
enum class IKType
{
    TwoBone,   // Analytical 2-joint solver (arms, legs) — fast and exact
    LookAt,    // Single-bone rotation to face target (head tracking, turrets)
    FABRIK     // Iterative multi-joint solver for arbitrary chain length
};
```

### IKChain Structure

```cpp
struct IKChain
{
    std::string name;                       // Debug name (e.g. "RightArmIK")
    IKType type = IKType::TwoBone;          // Solver algorithm
    std::vector<int32_t> boneIndices;       // Ordered root-to-end-effector
    XMFLOAT3 targetPosition{0, 0, 0};      // World-space target
    XMFLOAT3 poleVector{0, 1, 0};          // Joint bend hint (TwoBone only)
    float weight = 1.0f;                    // IK influence [0, 1]
    bool enabled = true;                    // Skip when false
    int maxIterations = 10;                 // FABRIK iterations per frame
    float tolerance = 0.01f;               // FABRIK convergence threshold (metres)
};
```

### Two-Bone IK

For limbs (arms, legs). Requires exactly 3 bone indices: [root, middle, end-effector].

```
Shoulder ─── Elbow ─── Hand → Target
```

```cpp
Spark::Animation::IKChain rightArm;
rightArm.name         = "RightArmIK";
rightArm.type         = Spark::Animation::IKType::TwoBone;
rightArm.boneIndices  = {skeleton->FindBone("RightArm"),
                         skeleton->FindBone("RightForeArm"),
                         skeleton->FindBone("RightHand")};
rightArm.targetPosition = weaponGripPos;
rightArm.poleVector     = elbowHintPos;  // Determines elbow bend direction
rightArm.weight         = 1.0f;
rightArm.enabled        = true;
instance.ikChains.push_back(rightArm);
```

### Look-At IK

Rotates a single bone to face a target. Requires exactly 1 bone index.

```
Head bone → Look at target position
```

```cpp
Spark::Animation::IKChain lookAt;
lookAt.name         = "HeadLookAt";
lookAt.type         = Spark::Animation::IKType::LookAt;
lookAt.boneIndices  = {skeleton->FindBone("Head")};
lookAt.targetPosition = enemyPosition;
lookAt.weight         = 0.8f;  // Partial blend with base animation
lookAt.enabled        = true;
instance.ikChains.push_back(lookAt);
```

### FABRIK (Forward And Backward Reaching IK)

Iterative multi-joint solver for chains of any length (2+ bones). Converges quickly (typically 5-10 iterations).

```
Root ─── Joint1 ─── Joint2 ─── ... ─── End Effector → Target
```

```cpp
Spark::Animation::IKChain tail;
tail.name           = "TailIK";
tail.type           = Spark::Animation::IKType::FABRIK;
tail.boneIndices    = {skeleton->FindBone("Tail1"), skeleton->FindBone("Tail2"),
                       skeleton->FindBone("Tail3"), skeleton->FindBone("Tail4")};
tail.targetPosition = swayTarget;
tail.maxIterations  = 10;       // More iterations = more accurate
tail.tolerance      = 0.01f;    // Stop early if within 1cm
tail.weight         = 1.0f;
tail.enabled        = true;
instance.ikChains.push_back(tail);
```

## AnimationEvaluator (Core Processing)

The `AnimationEvaluator` is a static utility class with pure functions for per-frame animation computation. The `AnimationUpdateSystem` calls these methods in order:

| Method | Input | Output | Description |
|--------|-------|--------|-------------|
| `SampleClip()` | Clip + Skeleton + time | `localTransforms[]` | Interpolate keyframes for all bones |
| `BlendTransforms()` | Two pose arrays + factor | Blended `localTransforms[]` | Component-wise LERP for crossfade |
| `ComputeSkinningMatrices()` | Skeleton + local transforms | `finalTransforms[]` | Walk hierarchy, multiply offset matrices |
| `SolveTwoBoneIK()` | Local transforms + IKChain | Modified transforms | Analytical 2-joint solution |
| `SolveLookAtIK()` | Local transforms + IKChain | Modified transforms | Single-bone rotation toward target |
| `SolveFABRIK()` | Local transforms + IKChain | Modified transforms | Iterative multi-joint solution |

## AnimationInstance (Per-Entity Runtime)

Each animated entity has one `AnimationInstance`:

```cpp
struct AnimationInstance
{
    const Skeleton* skeleton = nullptr;           // Shared skeleton reference
    AnimationStateMachine stateMachine;            // Clip selection controller
    std::vector<AnimationLayer> layers;            // Blend layer stack
    std::vector<IKChain> ikChains;                 // IK post-processing chains
    BlendResult blendResult;                       // Output bone matrices

    XMFLOAT3 rootMotionDelta{0, 0, 0};            // Root bone translation delta
    XMFLOAT4 rootMotionRotationDelta{0, 0, 0, 1}; // Root bone rotation delta (quat)
    bool enableRootMotion = false;                 // Extract root bone motion

    void Update(float deltaTime);                  // Full per-entity pipeline
    void UpdateLayers(float deltaTime);            // Layer playback and blending
};
```

### BlendResult

```cpp
struct BlendResult
{
    std::vector<XMFLOAT4X4> localTransforms;  // Per-bone local transforms (intermediate)
    std::vector<XMFLOAT4X4> finalTransforms;  // GPU-ready skinning matrices
};
```

`finalTransforms` is uploaded to the vertex shader's bone constant buffer each frame.

## Root Motion

Root motion extraction moves the character based on animation data rather than gameplay code:

```cpp
instance.enableRootMotion = true;
instance.Update(deltaTime);

// Apply root motion delta to the character's transform
XMFLOAT3 moveDelta = instance.rootMotionDelta;
transform.position.x += moveDelta.x;
transform.position.y += moveDelta.y;
transform.position.z += moveDelta.z;

// Apply rotation delta for turn-in-place
XMFLOAT4 rotDelta = instance.rootMotionRotationDelta;
// ... apply quaternion multiplication to transform.rotation
```

Benefits:
- Translation from the root bone drives character movement (prevents foot sliding)
- Rotation from the root bone drives character facing
- Useful for realistic walk/run cycles, combat animations, and dance moves

## ECS Integration

Use `AnimationController` on entities (see [Entity Component System](Entity-Component-System.md)):

```cpp
auto& anim = world.AddComponent<AnimationController>(entity);
anim.currentAnimation = "Idle";
anim.playbackSpeed    = 1.0f;
anim.isPlaying        = true;
anim.loop             = true;
anim.blendFactor      = 1.0f;
```

The `AnimationUpdateSystem` evaluates state machines and blends poses each frame. It:
1. Locates or creates an `AnimationInstance` for each entity with `AnimationController`
2. Advances the state machine and blend layer playback times
3. Samples clips and blends transforms
4. Computes GPU-ready skinning matrices
5. Solves enabled IK chains
6. Uploads bone matrices to the per-entity GPU constant buffer

## Asset Import

Models with animations are imported via Assimp (see [Asset Pipeline](../gameplay-tools/Asset-Pipeline.md)):

| Format | Description |
|--------|-------------|
| **FBX** | Industry standard; full skeleton and animation support |
| **glTF** | Modern format with PBR materials and animations |
| **DAE (Collada)** | XML-based interchange format |

## Thread Safety

The animation system is **main thread only**. All state machine updates, blending, and IK solving must occur on the main update thread. The `AnimationManager` singleton is not thread-safe for concurrent reads and writes.

---

## See Also

- [Entity Component System](Entity-Component-System.md) -- AnimationController component
- [Asset Pipeline](../gameplay-tools/Asset-Pipeline.md) -- Importing animated models
- [SparkEditor](../gameplay-tools/SparkEditor.md) -- Animation timeline editor
- [Rendering and Graphics](Rendering-and-Graphics.md) -- Skinned mesh rendering
- [AI and Navigation](AI-and-Navigation.md) -- NPC animation integration
- [Physics](Physics.md) -- Root motion and physics interaction
- [Gameplay Systems](../gameplay-tools/Gameplay-Systems.md) -- Player and weapon animations
- [Cinematic Sequencer](../gameplay-tools/Cinematic-Sequencer.md) -- Animation in cinematics

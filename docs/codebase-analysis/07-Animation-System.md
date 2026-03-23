# 07 — Animation System

**Location:** `SparkEngine/Source/Engine/Animation/`

Complete skeletal animation pipeline with bone hierarchies, clip blending, state machines, Inverse Kinematics (IK), root motion extraction, 2D blend spaces, and animation compression. Uses **Assimp** for FBX/GLTF loading.

---

## Core Types

### Skeleton & Bones

```cpp
struct Bone {
    std::string name;
    int32_t parentIndex = -1;     // -1 = root bone
    XMFLOAT4X4 offsetMatrix;      // Inverse bind pose (mesh-to-bone space)
    XMFLOAT4X4 localBindPose;     // Local rest pose transform
};

struct Skeleton {
    std::string name;
    std::vector<Bone> bones;
    std::unordered_map<std::string, int32_t> boneNameToIndex;

    int32_t FindBone(const std::string& boneName) const;
    size_t GetBoneCount() const;
};
```

### Keyframes

```cpp
struct VectorKey {
    float time;
    XMFLOAT3 value;     // Position or scale
};

struct QuatKey {
    float time;
    XMFLOAT4 value;     // Quaternion (X, Y, Z, W)
};

struct BoneAnimation {
    std::string boneName;
    int32_t boneIndex = -1;
    std::vector<VectorKey> positionKeys;
    std::vector<QuatKey> rotationKeys;
    std::vector<VectorKey> scaleKeys;

    XMFLOAT3 InterpolatePosition(float time) const;  // LERP
    XMFLOAT4 InterpolateRotation(float time) const;  // SLERP
    XMFLOAT3 InterpolateScale(float time) const;     // LERP
};
```

### Animation Clips

```cpp
struct AnimationClip {
    std::string name;
    float duration = 0.0f;
    float ticksPerSecond = 24.0f;
    std::vector<BoneAnimation> channels;
    bool loop = true;

    const BoneAnimation* FindChannel(const std::string& boneName) const;
};
```

---

## State Machine

Manages animation state transitions with crossfading:

```cpp
struct AnimationState {
    std::string name;
    std::string clipName;
    float speed = 1.0f;
    bool loop = true;
};

struct AnimationTransition {
    std::string fromState, toState;
    float duration = 0.2f;                  // Crossfade time
    std::function<bool()> condition;         // Predicate
    bool hasExitTime = false;
    float exitTime = 1.0f;                  // Normalized [0, 1]
};
```

### Usage

```cpp
AnimationStateMachine sm;

sm.AddState({"Idle", "idle_anim", 1.0f, true});
sm.AddState({"Run", "run_anim", 1.0f, true});
sm.AddState({"Attack", "attack_anim", 1.2f, false});

sm.AddTransition({"Idle", "Run", 0.2f, [&]() { return speed > 0.1f; }});
sm.AddTransition({"Run", "Idle", 0.2f, [&]() { return speed < 0.1f; }});
sm.AddTransition({"*", "Attack", 0.1f, [&]() { return attackTriggered; }});

sm.SetDefaultState("Idle");
sm.Update(deltaTime);

std::string current = sm.GetCurrentStateName();
bool transitioning = sm.IsTransitioning();
float blend = sm.GetBlendFactor();
```

---

## Animation Layers & Blending

```cpp
enum class BlendMode {
    Override,   // Fully replace lower layers
    Additive,   // Add delta from bind pose
    Layered     // Linear blend with lower layers (weight-based)
};

struct AnimationLayer {
    std::string clipName;
    float weight = 1.0f;
    float currentTime = 0.0f;
    float speed = 1.0f;
    BlendMode blendMode = BlendMode::Override;
    bool playing = true;
    bool loop = true;
    std::vector<int32_t> boneMask;  // Empty = all bones
};
```

Example: Upper-body shooting while lower-body runs:

```cpp
// Layer 0: Full-body run (Override)
instance.layers[0] = {"run_anim", 1.0f, 0.0f, 1.0f, BlendMode::Override};

// Layer 1: Upper-body shoot (Layered, spine + arms only)
instance.layers[1] = {"shoot_anim", 0.8f, 0.0f, 1.0f, BlendMode::Layered};
instance.layers[1].boneMask = {spineIndex, leftArmIndex, rightArmIndex, headIndex};
```

---

## Inverse Kinematics (IK)

### IK Types

```cpp
enum class IKType {
    TwoBone,   // Analytical 2-joint (arm, leg)
    LookAt,    // Single-bone rotation to target
    FABRIK     // Forward-Backward Reaching (arbitrary chain length)
};

struct IKChain {
    std::string name;
    IKType type = IKType::TwoBone;
    std::vector<int32_t> boneIndices;  // Root to end-effector
    XMFLOAT3 targetPosition{0, 0, 0};
    XMFLOAT3 poleVector{0, 1, 0};     // Bend hint (TwoBone)
    float weight = 1.0f;
    bool enabled = true;
    int maxIterations = 10;            // FABRIK
    float tolerance = 0.01f;           // Convergence threshold
};
```

### Usage Examples

```cpp
// Foot IK: plant feet on terrain
IKChain leftFoot;
leftFoot.name = "LeftFootIK";
leftFoot.type = IKType::TwoBone;
leftFoot.boneIndices = {leftHipIndex, leftKneeIndex, leftFootIndex};
leftFoot.targetPosition = terrainHitPoint;
leftFoot.poleVector = {0, 0, 1};  // Knee bends forward

// Head look-at
IKChain headLookAt;
headLookAt.name = "HeadLookAt";
headLookAt.type = IKType::LookAt;
headLookAt.boneIndices = {headIndex};
headLookAt.targetPosition = playerPosition;
headLookAt.weight = 0.7f;  // Partial blend
```

---

## 2D Blend Space

Parametric animation blending (e.g., speed + direction → locomotion):

```cpp
BlendSpace2D& locomotion = BlendSpaceManager::GetInstance()
    .CreateBlendSpace("Locomotion");

// Place animation samples in 2D parameter space
locomotion.AddSample({0.0f, 0.0f}, "idle");           // Center: standing
locomotion.AddSample({0.0f, 1.0f}, "walk_forward");   // +Y: forward
locomotion.AddSample({0.0f, -1.0f}, "walk_backward"); // -Y: backward
locomotion.AddSample({-1.0f, 0.0f}, "strafe_left");   // -X: left
locomotion.AddSample({1.0f, 0.0f}, "strafe_right");   // +X: right
locomotion.AddSample({0.0f, 2.0f}, "run_forward");    // Far +Y: running

// Evaluate at runtime (barycentric interpolation)
BlendSpaceResult result = locomotion.Evaluate(moveX, moveY);
// result.animations: [{name="walk_forward", weight=0.7}, {name="strafe_right", weight=0.3}]
```

---

## Root Motion

Extract movement from the root bone and apply to character controller instead of skeleton:

```cpp
AnimationInstance instance;
instance.enableRootMotion = true;

instance.Update(deltaTime);

XMFLOAT3 moveDelta = instance.rootMotionDelta;           // Translation this frame
XMFLOAT4 rotDelta  = instance.rootMotionRotationDelta;   // Rotation this frame

characterController.Move(moveDelta);
characterController.Rotate(rotDelta);
```

---

## AnimationManager — Asset Cache

```cpp
auto& mgr = AnimationManager::GetInstance();

// Loading
auto skeleton = mgr.LoadSkeleton("Data/Models/character.fbx");
auto clips = mgr.LoadAnimations("Data/Animations/character_anims.fbx");

// Registration
mgr.RegisterClip("idle", clips[0]);
mgr.RegisterClip("run", clips[1]);

// Lookup
auto clip = mgr.GetClip("idle");

// Console
std::string list = mgr.Console_ListAnimations();
```

---

## AnimationEvaluator — Core Processing

Static utility class for the pipeline steps:

```cpp
// 1. Sample clip at time → per-bone local transforms
AnimationEvaluator::SampleClip(clip, skeleton, time, localTransforms);

// 2. Blend two pose sets
AnimationEvaluator::BlendTransforms(poseA, poseB, blendFactor, result);

// 3. Compute GPU-ready skinning matrices
AnimationEvaluator::ComputeSkinningMatrices(skeleton, localTransforms, finalTransforms);

// 4. Apply IK post-process
AnimationEvaluator::SolveTwoBoneIK(localTransforms, skeleton, footIKChain);
AnimationEvaluator::SolveLookAtIK(localTransforms, skeleton, headLookAtChain);
AnimationEvaluator::SolveFABRIK(localTransforms, skeleton, tentacleChain);
```

---

## Animation Compression

**File:** `SparkEngine/Source/Engine/Animation/AnimationCompression.h`

Reduces memory for animation clips:

- **Quantization**: Reduce floating-point precision per component
- **Keyframe reduction**: Remove redundant keys within tolerance
- **Curve fitting**: Replace keyframes with parametric curves
- **Per-bone LOD**: Different compression levels per bone importance

---

## Processing Pipeline (Per Frame)

```
AnimationInstance::Update(deltaTime)
│
├── 1. StateMachine.Update()        — Evaluate transitions, advance clips
├── 2. Layer evaluation             — For each layer: sample clip, blend with result
├── 3. ComputeSkinningMatrices()    — Multiply bone chains with offset matrices
├── 4. IK post-process              — Apply IK chains (foot placement, aim)
└── 5. Root motion extraction       — Extract root bone delta → rootMotionDelta
```

Output: `blendResult.finalTransforms` → uploaded to GPU as skinning matrix constant buffer.

---

## Integration with ECS

- **Component**: `AnimationController` (clip name, speed, loop, handle)
- **System**: `AnimationUpdateSystem` creates/updates `AnimationInstance` per entity
- **Output**: Final transforms uploaded to GPU per entity
- **Input**: IK targets driven by gameplay (raycasts, aim vectors)

/**
 * @file AnimationSystem.h
 * @brief Skeletal animation system with bone hierarchies, skinning, blending, and state machines
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a complete skeletal animation pipeline:
 * - Bone/skeleton data structures
 * - Animation clips with keyframe interpolation
 * - Animation blending (crossfade, layered, additive)
 * - Animation state machine with transitions
 * - IK (Inverse Kinematics) for foot placement and aim
 */

#pragma once
#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <cstdint>
#include <algorithm>

using namespace DirectX;

namespace Spark::Animation {

// ============================================================================
// Bone & Skeleton
// ============================================================================

struct Bone {
    std::string name;
    int32_t parentIndex = -1;
    XMFLOAT4X4 offsetMatrix;       ///< Inverse bind pose matrix
    XMFLOAT4X4 localBindPose;      ///< Local bind pose transform
};

struct Skeleton {
    std::string name;
    std::vector<Bone> bones;
    std::unordered_map<std::string, int32_t> boneNameToIndex;

    int32_t FindBone(const std::string& boneName) const {
        auto it = boneNameToIndex.find(boneName);
        return (it != boneNameToIndex.end()) ? it->second : -1;
    }

    size_t GetBoneCount() const { return bones.size(); }
};

// ============================================================================
// Keyframes & Animation Clips
// ============================================================================

struct VectorKey {
    float time;
    XMFLOAT3 value;
};

struct QuatKey {
    float time;
    XMFLOAT4 value;  ///< Quaternion rotation
};

struct BoneAnimation {
    std::string boneName;
    int32_t boneIndex = -1;
    std::vector<VectorKey> positionKeys;
    std::vector<QuatKey> rotationKeys;
    std::vector<VectorKey> scaleKeys;

    XMFLOAT3 InterpolatePosition(float time) const;
    XMFLOAT4 InterpolateRotation(float time) const;
    XMFLOAT3 InterpolateScale(float time) const;
};

struct AnimationClip {
    std::string name;
    float duration = 0.0f;
    float ticksPerSecond = 24.0f;
    std::vector<BoneAnimation> channels;
    bool loop = true;

    const BoneAnimation* FindChannel(const std::string& boneName) const {
        for (const auto& ch : channels)
            if (ch.boneName == boneName) return &ch;
        return nullptr;
    }
};

// ============================================================================
// Animation Blending
// ============================================================================

enum class BlendMode {
    Override,      ///< Fully replace lower layers
    Additive,      ///< Add on top of lower layers
    Layered        ///< Blend with weight
};

struct AnimationLayer {
    std::string clipName;
    float weight = 1.0f;
    float currentTime = 0.0f;
    float speed = 1.0f;
    BlendMode blendMode = BlendMode::Override;
    bool playing = true;
    bool loop = true;

    /// Optional bone mask — if non-empty, only these bones are affected
    std::vector<int32_t> boneMask;
};

struct BlendResult {
    std::vector<XMFLOAT4X4> localTransforms;   ///< Per-bone local transforms
    std::vector<XMFLOAT4X4> finalTransforms;    ///< Per-bone final (skinning) matrices
};

// ============================================================================
// Animation State Machine
// ============================================================================

struct AnimationTransition {
    std::string fromState;
    std::string toState;
    float duration = 0.2f;              ///< Crossfade duration in seconds
    std::function<bool()> condition;    ///< Condition function (returns true to trigger)
    bool hasExitTime = false;           ///< Wait for animation to finish before transitioning
    float exitTime = 1.0f;             ///< Normalized exit time (0-1)
};

struct AnimationState {
    std::string name;
    std::string clipName;
    float speed = 1.0f;
    bool loop = true;
};

class AnimationStateMachine {
public:
    void AddState(const AnimationState& state);
    void AddTransition(const AnimationTransition& transition);
    void SetDefaultState(const std::string& stateName);

    void Update(float deltaTime);

    const std::string& GetCurrentStateName() const { return m_currentState; }
    float GetCurrentTime() const { return m_currentTime; }
    float GetBlendFactor() const { return m_blendFactor; }
    bool IsTransitioning() const { return m_isTransitioning; }
    const std::string& GetTargetStateName() const { return m_targetState; }

    /// Force immediate state change (no blending)
    void ForceState(const std::string& stateName);

    /// Console integration
    std::string Console_GetStateInfo() const;

private:
    std::unordered_map<std::string, AnimationState> m_states;
    std::vector<AnimationTransition> m_transitions;
    std::string m_currentState;
    std::string m_targetState;
    std::string m_defaultState;

    float m_currentTime = 0.0f;
    float m_blendFactor = 0.0f;
    float m_transitionDuration = 0.0f;
    float m_transitionElapsed = 0.0f;
    bool m_isTransitioning = false;
};

// ============================================================================
// IK (Inverse Kinematics)
// ============================================================================

enum class IKType {
    TwoBone,      ///< Simple two-bone IK (legs, arms)
    LookAt,       ///< Rotate bone to look at target
    FABRIK         ///< Forward and Backward Reaching IK
};

struct IKChain {
    std::string name;
    IKType type = IKType::TwoBone;
    std::vector<int32_t> boneIndices;
    XMFLOAT3 targetPosition{0, 0, 0};
    XMFLOAT3 poleVector{0, 1, 0};     ///< Hint direction for elbow/knee
    float weight = 1.0f;
    bool enabled = true;
    int maxIterations = 10;            ///< For FABRIK solver
    float tolerance = 0.01f;           ///< Distance threshold
};

// ============================================================================
// Animation Instance (per-entity runtime data)
// ============================================================================

struct AnimationInstance {
    const Skeleton* skeleton = nullptr;
    AnimationStateMachine stateMachine;
    std::vector<AnimationLayer> layers;
    std::vector<IKChain> ikChains;
    BlendResult blendResult;

    /// Root motion delta for this frame
    XMFLOAT3 rootMotionDelta{0, 0, 0};
    XMFLOAT4 rootMotionRotationDelta{0, 0, 0, 1};
    bool enableRootMotion = false;
};

// ============================================================================
// AnimationManager — loads and caches animation assets
// ============================================================================

class AnimationManager {
public:
    static AnimationManager& GetInstance();

    /// Load a skeleton from file (FBX/glTF via Assimp)
    std::shared_ptr<Skeleton> LoadSkeleton(const std::string& filepath);

    /// Load animation clips from file
    std::vector<std::shared_ptr<AnimationClip>> LoadAnimations(const std::string& filepath);

    /// Register pre-built clip
    void RegisterClip(const std::string& name, std::shared_ptr<AnimationClip> clip);

    /// Get a loaded clip by name
    std::shared_ptr<AnimationClip> GetClip(const std::string& name) const;

    /// Get a loaded skeleton by name
    std::shared_ptr<Skeleton> GetSkeleton(const std::string& name) const;

    /// Unload all cached data
    void Clear();

    /// Console integration
    std::string Console_ListAnimations() const;
    std::string Console_ListSkeletons() const;

private:
    AnimationManager() = default;
    std::unordered_map<std::string, std::shared_ptr<AnimationClip>> m_clips;
    std::unordered_map<std::string, std::shared_ptr<Skeleton>> m_skeletons;
};

// ============================================================================
// AnimationEvaluator — core animation processing
// ============================================================================

class AnimationEvaluator {
public:
    /// Sample a single clip at a given time, producing local bone transforms
    static void SampleClip(const AnimationClip& clip, const Skeleton& skeleton,
                           float time, std::vector<XMFLOAT4X4>& outLocalTransforms);

    /// Blend two sets of local transforms
    static void BlendTransforms(const std::vector<XMFLOAT4X4>& a,
                                const std::vector<XMFLOAT4X4>& b,
                                float blendFactor,
                                std::vector<XMFLOAT4X4>& outResult);

    /// Compute final skinning matrices from local transforms
    static void ComputeSkinningMatrices(const Skeleton& skeleton,
                                        const std::vector<XMFLOAT4X4>& localTransforms,
                                        std::vector<XMFLOAT4X4>& outFinalTransforms);

    /// Apply two-bone IK
    static void SolveTwoBoneIK(std::vector<XMFLOAT4X4>& localTransforms,
                                const Skeleton& skeleton,
                                const IKChain& chain);

    /// Apply look-at IK
    static void SolveLookAtIK(std::vector<XMFLOAT4X4>& localTransforms,
                               const Skeleton& skeleton,
                               const IKChain& chain);

    /// Apply FABRIK (Forward and Backward Reaching IK)
    static void SolveFABRIK(std::vector<XMFLOAT4X4>& localTransforms,
                             const Skeleton& skeleton,
                             const IKChain& chain);
};

} // namespace Spark::Animation

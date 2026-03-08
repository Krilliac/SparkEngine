/**
 * @file PerceptionSystem.h
 * @brief AI perception system for sight, hearing, and memory
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides sensory utilities and an ECS-compatible perception component:
 * - Vision cone check (field-of-view + range test)
 * - Hearing radius check (point-in-sphere)
 * - Perception memory system with confidence decay
 * - PerceptionComponent struct for integration with the ECS
 *
 * All functions are header-only inline implementations.
 */

#pragma once
#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS

#include <vector>
#include <unordered_map>
#include <cmath>
#include <cstdint>


namespace Spark::AI
{

    // ============================================================================
    // Perception Memory
    // ============================================================================

    /// Records what an AI agent remembers about a perceived stimulus.
    ///
    /// Each time the agent detects a target (by sight or sound), a
    /// PerceptionMemory entry is created or updated. Over time the confidence
    /// decays, representing the agent gradually losing track of the stimulus.
    struct PerceptionMemory
    {
        XMFLOAT3 lastSeenPosition{0.0f, 0.0f, 0.0f}; ///< Last known position of the stimulus
        float lastSeenTime = 0.0f;                   ///< World time when the stimulus was last detected
        float confidence = 0.0f;                     ///< 0..1 confidence that the memory is still valid

        /// Update this memory with a fresh observation
        /// @param position      Observed position of the stimulus
        /// @param currentTime   Current world time
        inline void Update(const XMFLOAT3& position, float currentTime)
        {
            lastSeenPosition = position;
            lastSeenTime = currentTime;
            confidence = 1.0f;
        }

        /// Decay confidence over time. Call once per frame with the current time.
        /// @param currentTime  Current world time
        /// @param decayRate    Confidence units lost per second (e.g. 0.1 = full decay in 10s)
        inline void Decay(float currentTime, float decayRate)
        {
            float elapsed = currentTime - lastSeenTime;
            confidence = 1.0f - (elapsed * decayRate);
            if (confidence < 0.0f)
                confidence = 0.0f;
        }

        /// Whether this memory is still considered valid
        /// @param minConfidence  Minimum confidence threshold
        /// @return True if confidence is at or above the threshold
        inline bool IsValid(float minConfidence = 0.05f) const { return confidence >= minConfidence; }
    };

    // ============================================================================
    // ECS Perception Component
    // ============================================================================

    /// Unique identifier type for perceived entities.
    /// Uses uint32_t to stay lightweight and ECS-friendly. Map to your
    /// actual EntityID type at integration time.
    using PerceptionEntityID = uint32_t;

    /// ECS component that gives an entity the ability to perceive others.
    ///
    /// Attach this to any entity that needs sight and hearing. The
    /// PerceptionSystem processes these components each frame, updating
    /// the memories map based on what the entity can currently see and hear.
    struct PerceptionComponent
    {
        // --- Sight ---
        float sightRange = 30.0f; ///< Maximum distance the entity can see
        float sightFOV = 120.0f;  ///< Field of view in degrees (full cone angle)

        // --- Hearing ---
        float hearingRange = 20.0f; ///< Maximum distance the entity can hear

        // --- Memory ---
        float memoryDecayRate = 0.1f; ///< Confidence loss per second (0.1 = 10s to full decay)
        float minConfidence = 0.05f;  ///< Memories below this confidence are pruned

        /// Active memories of perceived entities, keyed by their entity ID.
        std::unordered_map<PerceptionEntityID, PerceptionMemory> memories;

        // -----------------------------------------------------------------
        // Convenience methods that operate on this component's memory store
        // -----------------------------------------------------------------

        /// Record a fresh observation for an entity
        inline void Remember(PerceptionEntityID entityId, const XMFLOAT3& position, float currentTime)
        {
            memories[entityId].Update(position, currentTime);
        }

        /// Decay all memories and prune those that have faded away
        inline void DecayAndPrune(float currentTime)
        {
            auto it = memories.begin();
            while (it != memories.end())
            {
                it->second.Decay(currentTime, memoryDecayRate);
                if (!it->second.IsValid(minConfidence))
                {
                    it = memories.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        /// Retrieve memory of a specific entity, or nullptr if forgotten
        inline const PerceptionMemory* GetMemory(PerceptionEntityID entityId) const
        {
            auto it = memories.find(entityId);
            if (it != memories.end())
                return &it->second;
            return nullptr;
        }

        /// Get the entity we are most confident about
        inline PerceptionEntityID GetMostConfidentTarget() const
        {
            PerceptionEntityID best = 0;
            float bestConf = -1.0f;
            for (const auto& [id, mem] : memories)
            {
                if (mem.confidence > bestConf)
                {
                    bestConf = mem.confidence;
                    best = id;
                }
            }
            return (bestConf > 0.0f) ? best : 0;
        }

        /// Forget everything
        inline void ClearMemories() { memories.clear(); }
    };

    // ============================================================================
    // Perception Checks
    // ============================================================================

    namespace Perception
    {

        /// Internal helpers
        namespace Detail
        {

            inline float LengthSq(const XMFLOAT3& v)
            {
                return v.x * v.x + v.y * v.y + v.z * v.z;
            }

            inline float Length(const XMFLOAT3& v)
            {
                return std::sqrt(LengthSq(v));
            }

            inline XMFLOAT3 Sub(const XMFLOAT3& a, const XMFLOAT3& b)
            {
                return XMFLOAT3(a.x - b.x, a.y - b.y, a.z - b.z);
            }

            inline float Dot(const XMFLOAT3& a, const XMFLOAT3& b)
            {
                return a.x * b.x + a.y * b.y + a.z * b.z;
            }

            inline XMFLOAT3 Normalize(const XMFLOAT3& v)
            {
                float len = Length(v);
                if (len < 1e-6f)
                    return XMFLOAT3(0.0f, 0.0f, 0.0f);
                float inv = 1.0f / len;
                return XMFLOAT3(v.x * inv, v.y * inv, v.z * inv);
            }

        } // namespace Detail

        /// Vision cone check: determine whether an observer can see a target.
        ///
        /// Tests two conditions:
        /// 1. The target is within maxRange of the observer.
        /// 2. The angle between the observer's forward direction and the
        ///    direction to the target is within half the field-of-view cone.
        ///
        /// This is a pure geometric test -- it does not perform ray-casting for
        /// occlusion. For line-of-sight occlusion checks, combine this with a
        /// physics raycast in your game code.
        ///
        /// @param observerPos      Position of the observer
        /// @param observerForward  Unit forward vector of the observer (must be normalized)
        /// @param targetPos        Position of the target to check
        /// @param fovDegrees       Full cone angle of the observer's field of view, in degrees
        /// @param maxRange         Maximum sight distance
        /// @return True if the target is within the vision cone and range
        inline bool CanSee(const XMFLOAT3& observerPos, const XMFLOAT3& observerForward, const XMFLOAT3& targetPos,
                           float fovDegrees, float maxRange)
        {
            XMFLOAT3 toTarget = Detail::Sub(targetPos, observerPos);
            float distSq = Detail::LengthSq(toTarget);

            // Range check (squared to avoid sqrt)
            if (distSq > maxRange * maxRange)
                return false;

            // Zero distance means the target is at the observer position -- always visible
            if (distSq < 1e-12f)
                return true;

            // Normalize direction to target
            float dist = std::sqrt(distSq);
            XMFLOAT3 dirToTarget(toTarget.x / dist, toTarget.y / dist, toTarget.z / dist);

            // Compute angle between forward and direction to target via dot product
            float dot = Detail::Dot(observerForward, dirToTarget);

            // Clamp to [-1, 1] to handle floating-point imprecision
            if (dot > 1.0f)
                dot = 1.0f;
            if (dot < -1.0f)
                dot = -1.0f;

            float halfFOVRad = (fovDegrees * 0.5f) * (XM_PI / 180.0f);

            // Compare cosines to avoid acos (cos is monotonically decreasing on [0, pi])
            // Target is in FOV if cos(angle) >= cos(halfFOV)
            return dot >= std::cos(halfFOVRad);
        }

        /// Hearing check: determine whether a listener can hear a sound.
        ///
        /// Performs a simple sphere-intersection test. The sound is audible if
        /// the listener is within the sound's emission radius.
        ///
        /// @param listenerPos  Position of the listener
        /// @param soundPos     Origin of the sound
        /// @param soundRadius  Maximum distance at which the sound is audible
        /// @return True if the listener is within the sound's audible radius
        inline bool CanHear(const XMFLOAT3& listenerPos, const XMFLOAT3& soundPos, float soundRadius)
        {
            XMFLOAT3 diff = Detail::Sub(listenerPos, soundPos);
            float distSq = Detail::LengthSq(diff);
            return distSq <= soundRadius * soundRadius;
        }

        /// Extended hearing check that also returns an attenuation factor.
        ///
        /// @param listenerPos   Position of the listener
        /// @param soundPos      Origin of the sound
        /// @param soundRadius   Maximum distance at which the sound is audible
        /// @param[out] loudness Normalized loudness at the listener position (1.0 at source, 0.0 at radius)
        /// @return True if the sound is audible
        inline bool CanHearWithLoudness(const XMFLOAT3& listenerPos, const XMFLOAT3& soundPos, float soundRadius,
                                        float& loudness)
        {
            XMFLOAT3 diff = Detail::Sub(listenerPos, soundPos);
            float distSq = Detail::LengthSq(diff);
            float radiusSq = soundRadius * soundRadius;

            if (distSq > radiusSq)
            {
                loudness = 0.0f;
                return false;
            }

            float dist = std::sqrt(distSq);
            loudness = 1.0f - (dist / (soundRadius + 1e-6f));
            if (loudness < 0.0f)
                loudness = 0.0f;
            return true;
        }

        /// Process perception for a single entity, updating its PerceptionComponent.
        ///
        /// This is a helper that performs both sight and hearing checks against
        /// a set of potential targets and updates the component's memory map.
        /// Intended to be called once per frame per perceiving entity.
        ///
        /// @param component         The perceiver's PerceptionComponent (modified in place)
        /// @param observerPos       Position of the perceiving entity
        /// @param observerForward   Normalized forward direction of the perceiver
        /// @param currentTime       Current world time (seconds)
        /// @param targetIds         Entity IDs of potential targets
        /// @param targetPositions   Positions of potential targets (parallel with targetIds)
        /// @param soundPositions    Positions of active sounds this frame (optional)
        /// @param soundRadii        Radii of active sounds (parallel with soundPositions)
        /// @param soundSourceIds    Entity IDs that produced each sound (parallel with soundPositions)
        inline void UpdatePerception(PerceptionComponent& component, const XMFLOAT3& observerPos,
                                     const XMFLOAT3& observerForward, float currentTime,
                                     const std::vector<PerceptionEntityID>& targetIds,
                                     const std::vector<XMFLOAT3>& targetPositions,
                                     const std::vector<XMFLOAT3>& soundPositions = {},
                                     const std::vector<float>& soundRadii = {},
                                     const std::vector<PerceptionEntityID>& soundSourceIds = {})
        {
            // --- Sight checks ---
            for (size_t i = 0; i < targetIds.size() && i < targetPositions.size(); ++i)
            {
                if (CanSee(observerPos, observerForward, targetPositions[i], component.sightFOV, component.sightRange))
                {
                    component.Remember(targetIds[i], targetPositions[i], currentTime);
                }
            }

            // --- Hearing checks ---
            size_t soundCount = soundPositions.size();
            if (soundRadii.size() < soundCount)
                soundCount = soundRadii.size();
            if (soundSourceIds.size() < soundCount)
                soundCount = soundSourceIds.size();

            for (size_t i = 0; i < soundCount; ++i)
            {
                if (CanHear(observerPos, soundPositions[i], soundRadii[i]))
                {
                    // We record the sound origin as the perceived position
                    component.Remember(soundSourceIds[i], soundPositions[i], currentTime);
                }
            }

            // --- Memory decay and pruning ---
            component.DecayAndPrune(currentTime);
        }

    } // namespace Perception

} // namespace Spark::AI

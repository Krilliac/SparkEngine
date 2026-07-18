/**
 * @file PhysicsSpatialQueriesInternal.h
 * @brief Shared internal helpers for the PhysicsSystem spatial-query translation units
 * @author Spark Engine Team
 * @date 2025
 *
 * Internal header — include only from the PhysicsSpatialQueries*.cpp files.
 * Holds the layer-mask body filter shared by the filtered raycast, overlap,
 * and shape cast (sweep) queries. The anonymous namespace is intentional:
 * each including translation unit gets its own internal-linkage copy, exactly
 * as when the class lived in a single .cpp.
 */

#pragma once

#include "PhysicsBody.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyFilter.h>

#include <cstdint>

// ============================================================================
// LAYER-MASK BODY FILTER
// ============================================================================

namespace
{
    /**
     * @brief Jolt BodyFilter that applies the engine's collision-layer bitmask
     *        (PhysicsBody::GetCollisionGroup() vs. query layerMask) and an
     *        optional single-body exclusion.
     *
     * Bodies without a PhysicsBody wrapper (userData == 0) always pass the
     * layer test so raw Jolt-internal bodies (e.g. world geometry created
     * outside CreateBody) are never silently skipped.
     */
    class LayerMaskBodyFilter final : public JPH::BodyFilter
    {
      public:
        LayerMaskBodyFilter(uint16_t layerMask, const PhysicsBody* ignoreBody)
            : m_layerMask(layerMask), m_ignoreBody(ignoreBody)
        {
        }

        bool ShouldCollide(const JPH::BodyID& inBodyID) const override
        {
            // Cheap broadphase-level rejection of the ignored body.
            if (m_ignoreBody != nullptr && inBodyID.GetIndexAndSequenceNumber() == m_ignoreBody->GetJoltBodyID())
            {
                return false;
            }
            return true;
        }

        bool ShouldCollideLocked(const JPH::Body& inBody) const override
        {
            const auto* body = reinterpret_cast<const PhysicsBody*>(inBody.GetUserData());
            if (body == nullptr)
            {
                return true; // No wrapper — cannot classify; include (see class doc)
            }
            if (body == m_ignoreBody)
            {
                return false;
            }
            return (body->GetCollisionGroup() & m_layerMask) != 0;
        }

      private:
        uint16_t m_layerMask;
        const PhysicsBody* m_ignoreBody;
    };
} // namespace

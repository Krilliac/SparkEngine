/**
 * @file TFDeployablePlacement.cpp
 * @brief W6 placement validation (slope / spacing / region ownership / physics
 *        overlap) + the ShieldWall static Jolt blocker body.
 *
 * All validation runs server-side in ServerTryPlaceDeployable BEFORE any state
 * changes. Physics is strictly optional: every probe null-checks
 * IEngineContext::GetPhysics() and degrades to the analytic checks (terrain
 * slope from TerrainHeightAt, spacing from the deployable records, region
 * ownership from TFRegionSystem). This module NEVER steps the physics world —
 * stepping stays with its single owner (see PhysicsSystem.h "Stepping
 * contract"); we only create/remove static bodies and run overlap queries.
 */
#include "Game/TFDeployableSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFDeployableTypes.h"
#include "World/TFRegionSystem.h"
#include "World/TFWorldSetup.h"

#include "Physics/PhysicsSystem.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace Terrafront
{

    namespace
    {

        constexpr float kRadToDeg = 57.2957795f;
        constexpr float kSlopeSampleM = 1.0f;  // central-difference half step
        constexpr float kOverlapLiftM = 0.15f; // probe clearance above the terrain

        // ShieldWall blocker half extents (metres): a ~4.4 m wide, 2.2 m tall slab.
        // Matches the stretched-crate visual (spec visualScaleMult {3.0, 2.0, 0.45}).
        constexpr float kWallHalfX = 2.2f, kWallHalfY = 1.1f, kWallHalfZ = 0.35f;

        // A drop point farther than this factor of the min region-center spacing from
        // EVERY region center counts as unowned wilderness (hex corner reach is
        // ~0.577x the center spacing, so 0.65x covers a full hex with a small skirt).
        constexpr float kRegionInfluenceFactor = 0.65f;

        inline float Dist2XZ(float ax, float az, float bx, float bz)
        {
            const float dx = ax - bx, dz = az - bz;
            return dx * dx + dz * dz;
        }

    } // namespace

    // ---------------------------------------------------------------------------
    // Validation pipeline (cheap analytic checks first, physics probe last)
    // ---------------------------------------------------------------------------

    TFDeployResult TFDeployableSystem::ValidatePlacement(const TFDeployableSpec& spec, FactionId placer,
                                                         const float pos[3], EntityId ignoreEntity)
    {
        // Terrain grade at the drop point.
        if (TerrainSlopeTanAt(pos[0], pos[2]) > spec.maxSlopeTan)
            return TFDeployResult::TooSteep;

        // Spacing vs every other deployable (server records only run this path).
        // The required gap is the larger of the two kinds' minSpacingM, so a big
        // station keeps small packs away just as packs keep away from stations.
        for (const auto& [entity, rec] : m_deployables)
        {
            if (entity == ignoreEntity)
                continue; // about to be replaced by this placement
            const TFDeployableSpec* other = TFDeploySpecOf(rec.view.kind);
            const float gap = std::max(spec.minSpacingM, other ? other->minSpacingM : 0.0f);
            const float dx = pos[0] - rec.view.pos[0];
            const float dy = pos[1] - rec.view.pos[1];
            const float dz = pos[2] - rec.view.pos[2];
            if (dx * dx + dy * dy + dz * dz < gap * gap)
                return TFDeployResult::TooClose;
        }

        // Region ownership: enemy-held ground refuses (neutral/friendly/wilderness ok).
        if (RegionBlocksPlacement(placer, pos))
            return TFDeployResult::HostileRegion;

        // Physics overlap probe (only when Jolt is live; stub/absent = skip).
        if (PhysicsOverlapBlocked(spec, pos))
            return TFDeployResult::Blocked;

        return TFDeployResult::Ok;
    }

    float TFDeployableSystem::TerrainSlopeTanAt(float x, float z) const
    {
        if (!m_ctx || !m_ctx->world)
            return 0.0f; // headless harness: flat
        const TFWorldSetup& w = *m_ctx->world;
        const float dhdx = (w.TerrainHeightAt(x + kSlopeSampleM, z) - w.TerrainHeightAt(x - kSlopeSampleM, z)) /
                           (2.0f * kSlopeSampleM);
        const float dhdz = (w.TerrainHeightAt(x, z + kSlopeSampleM) - w.TerrainHeightAt(x, z - kSlopeSampleM)) /
                           (2.0f * kSlopeSampleM);
        return std::sqrt(dhdx * dhdx + dhdz * dhdz); // steepest grade, rise/run
    }

    bool TFDeployableSystem::RegionBlocksPlacement(FactionId placer, const float pos[3])
    {
        if (!m_ctx || !m_ctx->regions || !m_ctx->data || !m_ctx->data->IsLoaded())
            return false; // no territory model loaded — nothing to refuse on
        const auto& regions = m_ctx->data->GetContinent().regions;
        if (regions.size() < 2)
            return false;

        // Hex lattice: the containing hex is the nearest region center (Voronoi of
        // hex centers == the hexes). Lazily cache the min center spacing so points
        // beyond every hex's reach count as unowned wilderness.
        if (m_regionSpacing < 0.0f)
        {
            float best2 = std::numeric_limits<float>::max();
            for (size_t i = 0; i < regions.size(); ++i)
                for (size_t j = i + 1; j < regions.size(); ++j)
                    best2 = std::min(
                        best2, Dist2XZ(regions[i].centerX, regions[i].centerZ, regions[j].centerX, regions[j].centerZ));
            m_regionSpacing = std::sqrt(best2);
        }

        const RegionDef* nearest = nullptr;
        float nearest2 = std::numeric_limits<float>::max();
        for (const RegionDef& r : regions)
        {
            const float d2 = Dist2XZ(pos[0], pos[2], r.centerX, r.centerZ);
            if (d2 < nearest2)
            {
                nearest2 = d2;
                nearest = &r;
            }
        }
        const float influence = m_regionSpacing * kRegionInfluenceFactor;
        if (!nearest || nearest2 > influence * influence)
            return false; // wilderness between/outside hexes

        const FactionId owner = m_ctx->regions->OwnerOf(nearest->id);
        return owner != FactionId::None && owner != placer;
    }

    bool TFDeployableSystem::PhysicsOverlapBlocked(const TFDeployableSpec& spec, const float pos[3]) const
    {
        ::PhysicsSystem* physics = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetPhysics() : nullptr;
        if (!physics)
            return false; // Jolt absent/stubbed — analytic checks already ran

        // Probe sphere floats overlapRadiusM above the surface so flat terrain
        // directly underneath never self-blocks; slope is gated separately.
        const XMFLOAT3 center{pos[0], pos[1] + spec.overlapRadiusM + kOverlapLiftM, pos[2]};
        std::vector<PhysicsBody*> hits;
        return physics->SphereOverlapFiltered(center, spec.overlapRadiusM, CollisionLayers::PlacementMask, hits);
    }

    // ---------------------------------------------------------------------------
    // ShieldWall static blocker (authority only; clients keep the visual mirror)
    // ---------------------------------------------------------------------------

    void TFDeployableSystem::CreateWallBody(Rec& rec)
    {
        if (!m_ctx || !m_ctx->IsAuthority() || rec.view.kind != kDeployShieldWall || rec.wallBody)
            return;
        ::PhysicsSystem* physics = m_ctx->engine ? m_ctx->engine->GetPhysics() : nullptr;
        if (!physics)
            return; // contract: wall is a pure visual/hp object without Jolt

        PhysicsBodyDesc desc;
        desc.type = PhysicsBodyType::Static;
        desc.mass = 0.0f;
        desc.shape.type = CollisionShapeType::Box;
        desc.shape.dimensions = {kWallHalfX, kWallHalfY, kWallHalfZ};
        desc.position = {rec.view.pos[0], rec.view.pos[1] + kWallHalfY, rec.view.pos[2]};
        desc.rotation = {0.0f, rec.view.yaw * kRadToDeg, 0.0f};
        desc.collisionGroup = CollisionLayers::Deployable;
        desc.collisionMask = CollisionLayers::All;
        desc.entityId = (rec.local != 0) ? rec.local : rec.view.entity;
        desc.name = "TF_ShieldWall_" + std::to_string(rec.view.entity);

        rec.wallBody = physics->CreateBody(desc);
        if (!rec.wallBody)
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] ShieldWall %u: Jolt body creation failed (visual-only wall)",
                           rec.view.entity);
    }

    void TFDeployableSystem::ReleaseWallBody(Rec& rec)
    {
        if (!rec.wallBody)
            return;
        ::PhysicsSystem* physics = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetPhysics() : nullptr;
        if (physics)
            physics->RemoveBody(rec.wallBody);
        rec.wallBody.reset();
    }

} // namespace Terrafront

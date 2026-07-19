/**
 * @file TFRegionDecorLayout.cpp
 * @brief TFRegionDecor role-agnostic layout (W10): ClearOfGameplay,
 *        TryComputeLayout and the generation-time clearance / corridor /
 *        approach enforcement (EnforceCollisionClearance). Split from
 *        TFRegionDecor.cpp per the repo file-size rules (same class — mirrors
 *        the TFWorldSetup/-Draw/-Render split); decor.json parsing lives in
 *        TFRegionDecorData.cpp, presentation in TFRegionDecorVisuals.cpp.
 *
 * Layout follows the TFRegionSystem::UpdateCaptureVisuals readiness bar minus
 * the ECS world (dedicated servers may run without one — the 2026-07-10
 * headless-boot lesson); see the determinism contract in TFRegionDecor.h.
 */
#include "World/TFRegionDecor.h"

#include "Data/TFDataTables.h"
#include "World/TFWorldSetup.h" // TerrainHeightAt

#include "Utils/LogMacros.h"

#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace Terrafront
{

    namespace
    {

        constexpr uint32_t kMaxDecorPerRegion = 20; // lane budget (cap tower/banner/terminals not counted here)
        constexpr float kDecorSeparationM = 5.0f;   // scatter props keep clear of placed decor
        constexpr int kScatterAttempts = 12;        // rejection-sampling tries per scatter prop
        constexpr float kTwoPi = 6.2831853f;
        constexpr float kDegToRad = 0.01745329252f; // W11: part footprints (same constant as TFWorldCollision)

        /// Tiny deterministic LCG (Numerical Recipes constants) so every role
        /// scatters identically from the same RegionId seed. NOT std::rand /
        /// std::mt19937: no cross-CRT/no-header-weight, and trivially stable.
        struct DecorRng
        {
            uint32_t s;
            explicit DecorRng(uint32_t seed) : s(seed ^ 0x9E3779B9u) {}
            uint32_t Next()
            {
                s = s * 1664525u + 1013904223u;
                return s;
            }
            float NextFloat01() { return static_cast<float>(Next() >> 8) * (1.0f / 16777216.0f); }
        };

    } // namespace

    // ---------------------------------------------------------------------------
    // Role-agnostic layout (W10) — see the determinism contract in the header
    // ---------------------------------------------------------------------------

    bool TFRegionDecor::ClearOfGameplay(const RegionDef& r, float x, float z) const
    {
        const float clearSq = m_clearanceM * m_clearanceM;
        const auto tooClose = [&](float px, float pz)
        {
            const float dx = x - px;
            const float dz = z - pz;
            return dx * dx + dz * dz < clearSq;
        };
        for (const auto& pt : r.capturePoints)
            if (tooClose(pt[0], pt[1]))
                return false;
        for (const auto& sp : r.spawns)
            if (tooClose(sp[0], sp[1]))
                return false;
        if (r.vehicleTerminal.has_value() && tooClose((*r.vehicleTerminal)[0], (*r.vehicleTerminal)[1]))
            return false;
        return true;
    }

    bool TFRegionDecor::TryComputeLayout()
    {
        // Readiness bar: data tables + analytic terrain. Deliberately NOT the
        // ECS world — dedicated servers may run headless without one, and the
        // layout (and its collision) must exist there regardless.
        if (!m_ctx->world || !m_ctx->data || !m_ctx->data->IsLoaded())
            return false;
        const auto& regions = m_ctx->data->GetContinent().regions;
        if (regions.empty())
            return false;
        if (!LoadTemplates())
        {
            m_layoutDone = true; // fail-soft: don't retry every frame, regions stay bare
            return true;
        }

        uint32_t stampedRegions = 0;
        for (const RegionDef& r : regions)
        {
            const auto it = m_templates.find(r.tier);
            if (it == m_templates.end())
                continue;
            const TierTemplate& tmpl = it->second;

            // Skyanchors never flip, so their decor tints by home faction;
            // everything else stays neutral (no per-frame retint loop — decor
            // is fire-and-forget, unlike the owner-tinted banners/terminals).
            const FactionId tint = (r.tier == "skyanchor") ? r.homeFaction : FactionId::None;
            const float centerY = m_ctx->world->TerrainHeightAt(r.centerX, r.centerZ);

            uint32_t placed = 0;
            std::vector<std::array<float, 2>> placedXZ; // scatter separation targets
            placedXZ.reserve(kMaxDecorPerRegion);

            const auto addPiece = [&](const std::string& model, const std::string& material, float x, float z,
                                      float yawDeg, bool terrainAlign, bool castShadows, float emissive, bool collide,
                                      const std::vector<DecorCollidePart>* collideParts) -> bool
            {
                if (placed >= kMaxDecorPerRegion)
                {
                    ++m_skippedBudget;
                    return false;
                }
                if (!ClearOfGameplay(r, x, z))
                {
                    ++m_skippedClearance;
                    return false;
                }
                LayoutPiece piece;
                piece.model = model;
                piece.material = material;
                piece.tint = tint;
                piece.region = r.id;
                piece.x = x;
                piece.y = terrainAlign ? m_ctx->world->TerrainHeightAt(x, z) : centerY;
                piece.z = z;
                piece.yawDeg = yawDeg;
                piece.castShadows = castShadows;
                piece.emissive = emissive;
                piece.collide = collide;
                if (collideParts) // W11: multi-part collision shape rides the layout
                    piece.collideParts = *collideParts;
                m_layout.push_back(std::move(piece));
                placedXZ.push_back({x, z});
                ++placed;
                return true;
            };

            // 1) Fixed template pieces — identical layout on every role.
            for (const DecorPiece& p : tmpl.pieces)
                addPiece(p.model, p.material, r.centerX + p.offX, r.centerZ + p.offZ, p.yawDeg, p.terrainAlign,
                         p.castShadows, p.emissive, p.collide, &p.collideParts);

            // 2) Seeded prop scatter — RegionId-seeded so every role rolls the
            //    same props at the same spots (rejection sampling is fine: same
            //    code + same data => same accept/reject sequence everywhere).
            //    Scatter props NEVER collide (small clutter stays walk-through).
            const ScatterSpec& sc = tmpl.scatter;
            if (!sc.models.empty() && sc.countMax > 0 && sc.radiusMax > 0.0f)
            {
                DecorRng rng(static_cast<uint32_t>(r.id) * 2654435761u + 1u);
                const int want =
                    sc.countMin + static_cast<int>(rng.Next() % static_cast<uint32_t>(sc.countMax - sc.countMin + 1));
                for (int n = 0; n < want; ++n)
                {
                    for (int attempt = 0; attempt < kScatterAttempts; ++attempt)
                    {
                        const float ang = rng.NextFloat01() * kTwoPi;
                        const float rad = sc.radiusMin + rng.NextFloat01() * (sc.radiusMax - sc.radiusMin);
                        const float x = r.centerX + std::sin(ang) * rad;
                        const float z = r.centerZ + std::cos(ang) * rad;
                        const std::string& model = sc.models[rng.Next() % sc.models.size()];
                        const float yaw = rng.NextFloat01() * 360.0f;

                        // Keep clutter out of the stamped buildings' footprints.
                        bool nearDecor = false;
                        for (const auto& q : placedXZ)
                        {
                            const float dx = x - q[0];
                            const float dz = z - q[1];
                            if (dx * dx + dz * dz < kDecorSeparationM * kDecorSeparationM)
                            {
                                nearDecor = true;
                                break;
                            }
                        }
                        if (nearDecor || !ClearOfGameplay(r, x, z))
                            continue;
                        addPiece(model, std::string(), x, z, yaw, true, true, 0.0f, false, nullptr);
                        break;
                    }
                }
            }

            if (placed > 0)
                ++stampedRegions;
        }

        EnforceCollisionClearance();

        m_layoutDone = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game,
                       "[TF] decor: layout %u regions, %zu pieces (skipped %u clearance, %u budget; %u demoted)",
                       stampedRegions, m_layout.size(), m_skippedClearance, m_skippedBudget, m_colDemoted);
        return true;
    }

    void TFRegionDecor::EnforceCollisionClearance()
    {
        // W10 sanity gate, enforced AT GENERATION: no collidable decor may sit
        // within m_clearanceM of ANY region's capture points/spawns/terminals —
        // ClearOfGameplay above already guaranteed the piece's OWN region, this
        // closes the cross-region edge (adjacent gameplay points near a shared
        // border). Violators are demoted to visual-only, deterministically on
        // both roles (pure function of the same layout + region table), loudly:
        // a demotion means decor.json offsets need re-authoring.
        const auto& regions = m_ctx->data->GetContinent().regions;
        for (LayoutPiece& p : m_layout)
        {
            if (!p.collide)
                continue;

            // W11 gate-passages: probe the piece origin AND every collide-part
            // center (parts sit off-origin — a gate pillar 2.6 m out can crowd
            // a gameplay point the origin check would miss). Part world XZ =
            // piece XZ + Ry(pieceYaw) * part offset — the same yaw mapping as
            // TFWorldCollision (x' = x*c + z*s, z' = -x*s + z*c). A violation
            // by ANY probe demotes the WHOLE piece (partial gates would look
            // arbitrary and complicate determinism reasoning for no benefit).
            std::vector<std::array<float, 2>> probes;
            probes.reserve(1 + p.collideParts.size());
            probes.push_back({p.x, p.z});
            const float ryRad = p.yawDeg * kDegToRad;
            const float cyaw = std::cos(ryRad), syaw = std::sin(ryRad);
            for (const DecorCollidePart& part : p.collideParts)
                probes.push_back(
                    {part.off[0] * cyaw + part.off[2] * syaw + p.x, -part.off[0] * syaw + part.off[2] * cyaw + p.z});

            bool demoted = false;
            for (const RegionDef& r : regions)
            {
                for (const auto& q : probes)
                {
                    if (ClearOfGameplay(r, q[0], q[1]))
                        continue;
                    SPARK_LOG_ERROR(
                        Spark::LogCategory::Game,
                        "[TF] decor: CLEARANCE VIOLATION — collidable '%s' (region %u) within %.1fm of region "
                        "%u gameplay point; demoted to visual-only (fix decor.json offsets)",
                        p.model.c_str(), static_cast<unsigned>(p.region), static_cast<double>(m_clearanceM),
                        static_cast<unsigned>(r.id));
                    p.collide = false;
                    ++m_colDemoted;
                    demoted = true;
                    break;
                }
                if (demoted)
                    break;
            }
        }

        // -------------------------------------------------------------------
        // W12 part-validation (gate-parts-reauthor lane). The W11 leg-cage
        // lesson: four thin watchtower legs formed a convex pocket local
        // avoidance could not escape, so the collideParts DATA was disabled.
        // The data is now re-authored cage-free (single tower column, archway
        // gates, two-support gantry) and these two passes are the guard rail
        // that keeps FUTURE decor.json edits honest:
        //   A) no two ground-blocking parts of DIFFERENT pieces in the same
        //      region may pinch a corridor narrower than 1.2 m (bot trap) —
        //      the smaller piece is demoted to visual-only;
        //   B) every capture point keeps a >= 120-degree open ground approach
        //      (12 directions, samples out to 6 m) — blocking pieces demoted.
        // Both passes are pure functions of the finished layout + decor.json +
        // the region table with fixed iteration order and the same float math
        // on every role — the same determinism contract as the W10 pass above.
        // Whole-model-OBB pieces are NOT probed here: their bounds live in
        // TFWorldCollision's OBJ cache (not reachable from layout, by design);
        // solid buildings are covered by the 8 m clearance gate instead.
        // -------------------------------------------------------------------
        constexpr float kCorridorMinM = 1.2f;    // narrower than this between pieces = bot trap
        constexpr float kTouchEpsM = 0.01f;      // gaps below this are one merged solid, not a corridor
        constexpr float kPartBaseWalkY = 2.0f;   // parts starting this far up never block ground movement
        constexpr float kApproachRadiusM = 6.0f; // capture-point approach probe radius
        constexpr int kApproachDirs = 12;        // 30-degree sampling
        constexpr int kApproachRunNeeded = 4;    // 4 consecutive open samples ~= a 120-degree wedge

        // Ground-blocking part footprints as conservative world-XZ AABBs of
        // each yaw-rotated part box. Elevated parts (gate lintels, the gantry
        // span — model-local bottom >= 2 m) are walk-under and excluded.
        struct PartFootprint
        {
            float minX, minZ, maxX, maxZ;
            size_t piece; // index into m_layout (ascending build order)
        };
        std::vector<PartFootprint> feet;
        std::vector<float> pieceArea(m_layout.size(), 0.0f); // summed ground footprint per piece
        for (size_t i = 0; i < m_layout.size(); ++i)
        {
            const LayoutPiece& p = m_layout[i];
            if (!p.collide || p.collideParts.empty())
                continue;
            const float yawRad = p.yawDeg * kDegToRad;
            const float cy = std::cos(yawRad), sy = std::sin(yawRad);
            for (const DecorCollidePart& part : p.collideParts)
            {
                if (part.off[1] - part.size[1] * 0.5f >= kPartBaseWalkY)
                    continue; // lintel/span: pawns walk under it
                // Same part-center yaw mapping as the clearance probes above.
                const float cx = part.off[0] * cy + part.off[2] * sy + p.x;
                const float cz = -part.off[0] * sy + part.off[2] * cy + p.z;
                const float totRad = (p.yawDeg + part.yawDeg) * kDegToRad;
                const float tc = std::cos(totRad), ts = std::sin(totRad);
                const float hx = part.size[0] * 0.5f, hz = part.size[2] * 0.5f;
                const float ex = std::fabs(hx * tc) + std::fabs(hz * ts);
                const float ez = std::fabs(hx * ts) + std::fabs(hz * tc);
                feet.push_back({cx - ex, cz - ez, cx + ex, cz + ez, i});
                pieceArea[i] += 4.0f * ex * ez;
            }
        }

        // Pass A: pairwise corridor-pocket check within a region. Same-piece
        // pairs are skipped (an archway's own pillar spacing is authored
        // intent). Interpenetrating/touching parts are one merged solid — the
        // hazard is strictly the 0 < gap < 1.2 m pinch. Demotion goes to the
        // smaller summed footprint (less silhouette lost); ties demote the
        // later layout index — both deterministic.
        const auto axisGap = [](float minA, float maxA, float minB, float maxB)
        {
            const float g1 = minA - maxB;
            const float g2 = minB - maxA;
            const float g = g1 > g2 ? g1 : g2;
            return g > 0.0f ? g : 0.0f;
        };
        for (size_t a = 0; a < feet.size(); ++a)
        {
            for (size_t b = a + 1; b < feet.size(); ++b)
            {
                const PartFootprint& fa = feet[a];
                const PartFootprint& fb = feet[b];
                if (fa.piece == fb.piece)
                    continue;
                LayoutPiece& pa = m_layout[fa.piece];
                LayoutPiece& pb = m_layout[fb.piece];
                if (!pa.collide || !pb.collide || pa.region != pb.region)
                    continue; // already demoted, or different regions (8 m clearance covers borders)
                const float gx = axisGap(fa.minX, fa.maxX, fb.minX, fb.maxX);
                const float gz = axisGap(fa.minZ, fa.maxZ, fb.minZ, fb.maxZ);
                const float gap = std::sqrt(gx * gx + gz * gz);
                if (gap <= kTouchEpsM || gap >= kCorridorMinM)
                    continue;
                LayoutPiece& loser = (pieceArea[fa.piece] < pieceArea[fb.piece]) ? pa : pb;
                SPARK_LOG_WARN(Spark::LogCategory::Game,
                               "[TF] decor: CORRIDOR POCKET — parts of '%s' and '%s' (region %u) pinch a %.2fm "
                               "gap (< %.1fm); '%s' demoted to visual-only (re-author decor.json offsets)",
                               pa.model.c_str(), pb.model.c_str(), static_cast<unsigned>(pa.region),
                               static_cast<double>(gap), static_cast<double>(kCorridorMinM), loser.model.c_str());
                loser.collide = false;
                ++m_colDemoted;
            }
        }

        // Pass B: capture-point approach audit. A direction is open when none
        // of its samples (2/4/6 m — the mid samples catch footprints straddling
        // the ring) lies inside a LIVE ground footprint; the point passes with
        // >= 4 CONSECUTIVE open directions (circular scan), i.e. a ~120-degree
        // wedge (3 x 30 degrees between extremes + half-spacing margins). On
        // failure EVERY blocking piece is demoted — with all blockers gone the
        // wedge trivially opens, and partial demotion would need an arbitrary
        // tie-break for no navigation benefit.
        for (const RegionDef& r : regions)
        {
            for (const auto& cp : r.capturePoints)
            {
                bool open[kApproachDirs];
                std::vector<char> isBlocker(m_layout.size(), 0);
                bool anyBlocked = false;
                for (int d = 0; d < kApproachDirs; ++d)
                {
                    open[d] = true;
                    const float ang = (kTwoPi * static_cast<float>(d)) / static_cast<float>(kApproachDirs);
                    const float dirX = std::sin(ang), dirZ = std::cos(ang);
                    for (int step = 1; step <= 3; ++step)
                    {
                        const float t = kApproachRadiusM * (static_cast<float>(step) / 3.0f);
                        const float sx = cp[0] + dirX * t;
                        const float sz = cp[1] + dirZ * t;
                        for (const PartFootprint& f : feet)
                        {
                            if (!m_layout[f.piece].collide)
                                continue; // demoted in pass A (or by this pass for an earlier point)
                            if (sx < f.minX || sx > f.maxX || sz < f.minZ || sz > f.maxZ)
                                continue;
                            open[d] = false;
                            anyBlocked = true;
                            isBlocker[f.piece] = 1;
                        }
                    }
                }
                if (!anyBlocked)
                    continue;
                int best = 0, run = 0;
                for (int d = 0; d < kApproachDirs * 2 && best < kApproachDirs; ++d) // doubled scan = circular wrap
                {
                    run = open[d % kApproachDirs] ? run + 1 : 0;
                    if (run > best)
                        best = run;
                }
                if (best >= kApproachRunNeeded)
                    continue;
                for (size_t i = 0; i < m_layout.size(); ++i)
                {
                    if (!isBlocker[i] || !m_layout[i].collide)
                        continue;
                    SPARK_LOG_WARN(Spark::LogCategory::Game,
                                   "[TF] decor: APPROACH BLOCKED — capture point (%.0f, %.0f) of region %u keeps "
                                   "< 120 degrees of open approach; '%s' (region %u) demoted to visual-only",
                                   static_cast<double>(cp[0]), static_cast<double>(cp[1]), static_cast<unsigned>(r.id),
                                   m_layout[i].model.c_str(), static_cast<unsigned>(m_layout[i].region));
                    m_layout[i].collide = false;
                    ++m_colDemoted;
                }
            }
        }
    }

} // namespace Terrafront

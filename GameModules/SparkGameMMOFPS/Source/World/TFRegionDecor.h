/**
 * @file TFRegionDecor.h
 * @brief Per-tier region decor: a deterministic ROLE-AGNOSTIC layout (region
 *        defs + decor.json templates + RegionId-derived seed) consumed by two
 *        independent passes — a viewer-only visual stamp (HasLocalPlayer) and
 *        static Jolt OBB registration for collidable pieces (BOTH roles).
 *
 * OWNERSHIP: decor-collision lane (W10; layout/visuals landed W9) — this
 * header + TFRegionDecor.cpp + Assets/MMOFPS/Data/decor.json. Driven by
 * TFRegionSystem (constructed with it; W10: Update() is called on EVERY role,
 * not just inside its HasLocalPlayer block) so no Main.cpp wiring is needed.
 *
 * ## W10 split: role-agnostic layout, role-gated presentation
 * TryComputeLayout() is a pure deterministic function producing world-space
 * {model, position, yaw, collide} pieces. Both roles run it identically; only
 * what CONSUMES the layout is gated:
 *  - VISUALS (HasLocalPlayer only): one Transform+MeshRenderer entity per
 *    piece — dedicated servers never build these.
 *  - COLLISION (both roles): every piece whose decor.json entry says
 *    "collide": true (buildings; small props / holo signs stay walk-through,
 *    and scatter props NEVER collide) gets one static yaw-rotated Jolt OBB via
 *    TFWorldCollision::AddModelObb — the same OBJ-bounds streaming and
 *    rotated-box math (decor yaw DEGREES -> Jolt RADIANS) as the collision-v2
 *    scene bodies, registered into TFWorldSetup's scene collision set AFTER
 *    the .scene bodies (world init built those), with exactly ONE
 *    OptimizeBroadPhase() re-compact after the last decor body.
 *    W11 gate-passages: a piece may instead author "collideParts" (model-local
 *    boxes: offset/size/yawDeg) — then one rotated OBB per part is registered
 *    via TFWorldCollision::AddObbPart and the whole-model OBB is SKIPPED, so
 *    gate archways, watchtower undersides and gantry spans are genuinely
 *    walkable while their pillars/legs still block.
 *
 * ## Determinism contract (why the layout is bit-identical server/client)
 *  - Iteration order is DATA order only: regions in regions.json order; per
 *    region, template pieces in decor.json array order; the scatter rolls come
 *    from a private LCG seeded from RegionId alone. Same code + same data
 *    files => the same accept/reject sequence and the same floats everywhere.
 *  - Heights come from TFWorldSetup::TerrainHeightAt, itself part of the
 *    cross-role determinism contract (same params, same math, both sides).
 *  - No wall-clock, no shared RNG state, and NO role branches inside layout:
 *    HasLocalPlayer gates only the visual stamp, never layout content.
 *  - The W10 generation-time clearance enforcement (a collidable piece within
 *    clearanceM of ANY region's capture point/spawn/vehicle terminal is
 *    demoted to visual-only) is a pure function of the finished layout + the
 *    region table, so both roles demote the same pieces.
 */
#pragma once

#include "Core/TFTypes.h"
#include "World/TFDecorCulling.h" // W10 distance-culling lane: cull entry + ranges

#include <DirectXMath.h> // W12 decor-instancing: view/proj + per-instance world matrices

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class PhysicsBody;    // Physics/PhysicsBody.h (engine, global namespace)
class GraphicsEngine; // Graphics/GraphicsEngine.h (W12 decor-instancing)
namespace Spark
{
    class WorldMeshCache; // Graphics/WorldBasicRenderer.h (W12 decor-instancing)
} // namespace Spark

namespace Terrafront
{

    struct RegionDef; // Data/TFDataTables.h

    class TFRegionDecor
    {
      public:
        TFRegionDecor();
        ~TFRegionDecor();

        bool Initialize(TFGameContext& ctx);
        /// Spawn-once poll; called by TFRegionSystem::Update on EVERY role
        /// (W10). Computes the layout once when data + terrain are live, then
        /// registers collision OBBs (both roles) and stamps the visual
        /// entities (HasLocalPlayer roles). Cheap no-op after all passes ran.
        void Update();
        void Shutdown();

        uint32_t SpawnedCount() const { return static_cast<uint32_t>(m_entities.size()); }
        uint32_t CollisionBodyCount() const { return static_cast<uint32_t>(m_colBodies.size()); }

        /// W12 decor-instancing lane: draw the GROUPED decor with one
        /// GraphicsEngine::DrawMeshInstanced per mesh+material group. Called
        /// from TFWorldSetup::RenderWorld right after the per-entity ECS draw
        /// loop (same frame, same view/proj, b1 frame constants already set).
        /// On the first call it partitions the stamped decor into groups of
        /// >= 4 identical (model, material, emissive) pieces, flips those
        /// entities' MeshRenderer.visible off so the contended ECS loop skips
        /// them, and keeps everything else (small groups, or when the engine
        /// reports no instanced pipeline) on the per-entity path unchanged.
        /// VISUAL ONLY — never touches layout, collision or determinism.
        void RenderInstanced(GraphicsEngine* gfx, const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj);

        // W10 distance-culling lane: counters from the last cull pass, for the
        // region debug UI / tf_decor_debug (measurable win, not vibes).
        uint32_t VisibleDecorCount() const { return m_cullVisible; }
        uint32_t CulledDecorCount() const { return m_cullHidden; }

      private:
        /// W11 gate-passages: one authored collision box of a multi-part piece
        /// (decor.json "collideParts"). MODEL-LOCAL meters: `off` is the box
        /// center relative to the piece origin, `size` the FULL box size,
        /// `yawDeg` a local yaw about Y (DEGREES) composed with the piece yaw.
        /// When a piece authors parts, RegisterCollision creates one rotated
        /// OBB per part INSTEAD of the whole-model OBB — so gate archways /
        /// tower undersides stay walkable. Parts inherit the piece's
        /// determinism (pure data, same on both roles) and its clearance
        /// demotion; EnforceCollisionClearance additionally probes every
        /// part's world footprint, not just the piece origin.
        struct DecorCollidePart
        {
            float off[3] = {0.0f, 0.0f, 0.0f};
            float size[3] = {1.0f, 1.0f, 1.0f};
            float yawDeg = 0.0f;
        };

        struct DecorPiece
        {
            std::string model;        ///< OBJ path (Assets/Models/MMOFPS/...)
            std::string material;     ///< empty = faction/neutral structure material
            float offX = 0.0f;        ///< meters east of region center
            float offZ = 0.0f;        ///< meters north of region center
            float yawDeg = 0.0f;      ///< Transform.rotation is DEGREES
            bool terrainAlign = true; ///< false = region-center height (level runs)
            bool castShadows = true;
            float emissive = 0.0f;
            bool collide = false; ///< W10: "collide": true => static OBB on both roles
            /// W11: non-empty => per-part OBBs replace the whole-model OBB
            /// (authoring parts implies collide, see LoadTemplates).
            std::vector<DecorCollidePart> collideParts;
        };

        struct ScatterSpec
        {
            std::vector<std::string> models;
            int countMin = 0, countMax = 0;
            float radiusMin = 0.0f, radiusMax = 0.0f;
        };

        struct TierTemplate
        {
            std::vector<DecorPiece> pieces;
            ScatterSpec scatter;
        };

        /// One resolved world-space placement — the role-agnostic layout unit.
        struct LayoutPiece
        {
            std::string model;
            std::string material;             ///< empty = structure material (visual pass resolves)
            FactionId tint = FactionId::None; ///< skyanchor home faction, else None
            RegionId region = 0;              ///< owning region (body naming / debug)
            float x = 0.0f, y = 0.0f, z = 0.0f;
            float yawDeg = 0.0f;
            bool castShadows = true;
            float emissive = 0.0f;
            bool collide = false;
            /// W11: copied from the template piece; non-empty => per-part OBBs.
            std::vector<DecorCollidePart> collideParts;
        };

        bool LoadTemplates(); ///< decor.json -> m_templates (fail-soft: no decor)
        /// One deterministic layout pass over all regions (see the contract in
        /// the file header). Returns false while prerequisites (data tables,
        /// terrain) are not live yet; sets m_layoutDone once it ran (including
        /// the fail-soft empty-template case).
        bool TryComputeLayout();
        /// W10 generation-time clearance enforcement over the finished layout:
        /// collidable pieces within m_clearanceM (XZ) of ANY region's capture
        /// point/spawn/vehicle terminal are demoted to visual-only (logged +
        /// counted). Belt-and-braces on top of the per-region placement check.
        void EnforceCollisionClearance();
        /// Both roles: one static OBB per collidable layout piece, registered
        /// into TFWorldSetup's scene TFWorldCollision, then ONE broadphase
        /// re-compact. Runs once.
        void RegisterCollision();
        /// HasLocalPlayer roles: one Transform+MeshRenderer entity per layout
        /// piece. Runs once (waits for the ECS world).
        void SpawnVisuals();
        /// W10 distance-culling lane (SEPARATE function by design — see
        /// World/TFDecorCulling.h): flips MeshRenderer.visible per decor entity
        /// from camera XZ distance vs the spawn-time cull class (buildings
        /// 600 m / props 250 m, +10% hide hysteresis), every ~0.25 s. VISUAL
        /// ONLY — never touches the layout, collision, or determinism surface.
        void UpdateCulling();
        /// >= m_clearanceM (XZ) from every capturePoint/spawn/vehicleTerminal.
        bool ClearOfGameplay(const RegionDef& r, float x, float z) const;

        // ------------------------------------------------------------------
        // W12 decor-instancing lane (render-path opt-in; SEPARATE section)
        // ------------------------------------------------------------------

        /// One instanced-draw group: every stamped decor piece sharing the
        /// same (model, resolved material, emissive). `cullIdx[k]` indexes
        /// m_cull (visibility source) and `worlds[k]` is that member's
        /// precomputed world matrix — decor never moves, so both are filled
        /// once at group build. Groups only exist for >= 4 members AND an
        /// available engine instanced pipeline; everything else stays on the
        /// per-entity ECS path.
        struct DecorInstanceGroup
        {
            std::string model;    ///< OBJ path (group mesh)
            std::string material; ///< resolved material JSON (faction tint already applied)
            float emissive = 0.0f;
            std::vector<uint32_t> cullIdx;           ///< members, as indexes into m_cull
            std::vector<DirectX::XMFLOAT4X4> worlds; ///< parallel to cullIdx (static)
        };

        /// One-shot partition of the stamped decor into instance groups (see
        /// RenderInstanced). Requires m_layout / m_entities / m_cull to be in
        /// lock-step (SpawnVisuals pushes all three per piece, same order);
        /// bails to the per-entity path if that invariant ever breaks.
        void BuildInstanceGroups(GraphicsEngine* gfx);
        /// Emergency un-opt-in: restore grouped entities' MeshRenderer.visible
        /// from their cull state and drop all groups (permanent per-entity
        /// fallback). Used if a DrawMeshInstanced call fails at runtime.
        void DissolveInstanceGroups();

        std::vector<DecorInstanceGroup> m_groups;           ///< instanced groups (empty = per-entity)
        std::vector<uint8_t> m_grouped;                     ///< parallel to m_cull: 1 = consumed by a group
        std::vector<DirectX::XMFLOAT4X4> m_instScratch;     ///< per-frame visible-subset fill buffer
        std::unique_ptr<Spark::WorldMeshCache> m_meshCache; ///< group meshes (tinyobjloader path)
        bool m_groupsBuilt{false};                          ///< build latch (tf_decor_inst can re-arm)
        bool m_instCmd{false};                              ///< tf_decor_inst registered by this instance
        uint32_t m_instDrawsLast{0};                        ///< instanced draw calls last frame (debug)
        uint32_t m_instInstancesLast{0};                    ///< instances drawn last frame (debug)

        TFGameContext* m_ctx{nullptr};
        bool m_initialized{false};
        bool m_loadTried{false};
        bool m_loaded{false};

        // One-shot pass latches (W10). Layout gates the other two.
        bool m_layoutDone{false};
        bool m_collisionDone{false};
        bool m_visualsDone{false};

        float m_clearanceM{8.0f};
        std::unordered_map<std::string, TierTemplate> m_templates; ///< by tier name

        std::vector<LayoutPiece> m_layout; ///< role-agnostic resolved placements

        std::vector<uint32_t> m_entities; ///< every decor entity, for Shutdown

        // W10 distance-culling lane (visual-side only; parallel to m_entities
        // but self-contained — carries its own entity ids so layout-side
        // restructures cannot desync it).
        std::vector<TFDecorCullEntry> m_cull; ///< spawn-time XZ + cull class per entity
        uint32_t m_cullFrameCounter{0};       ///< pass every kDecorCullIntervalFrames
        uint32_t m_cullVisible{0};            ///< entities shown by the last pass
        uint32_t m_cullHidden{0};             ///< entities culled by the last pass
        /// Static OBB handles registered via TFWorldCollision::AddModelObb —
        /// removed one-by-one in Shutdown (Main.cpp shuts TFRegionSystem down
        /// BEFORE TFWorldSetup, so the collision set still exists then).
        std::vector<std::shared_ptr<::PhysicsBody>> m_colBodies;

        uint32_t m_skippedClearance{0}; ///< pieces dropped by the gameplay-clearance check
        uint32_t m_skippedBudget{0};    ///< pieces dropped by the 20-per-region cap
        uint32_t m_colSkipped{0};       ///< collidable pieces with no body (no Jolt / bad OBJ / cap)
        uint32_t m_colDemoted{0};       ///< collide -> visual-only demotions (clearance enforcement)
        bool m_debugCmd{false};         ///< tf_decor_debug registered by this instance
    };

} // namespace Terrafront

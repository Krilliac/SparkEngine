/**
 * @file TFSanctuaryDecor.cpp
 * @brief Sanctuary Haven home-base decor + class terminal (see the header).
 *
 * Follows TFRegionDecor to the letter: spawn once when the ECS world +
 * TFWorldSetup are live, viewer-only (HasLocalPlayer gate), Transform+
 * MeshRenderer entities with NO physics bodies. The layout is a constexpr
 * table below instead of decor.json — the sanctuary is one authored place,
 * not a per-tier template, and a compile-time table is deterministic across
 * clients by construction.
 *
 * All positions sit on the sanctuary pad plateau (TFSanctuaryZone.h:
 * center 320/3776, plateau radius 200, pad Y 24) and keep clear of the
 * gameplay points: >= 6 m from the travel terminal (320/3720) and the four
 * spawn pads (296|344, 3744|3808) — except the sanc_spawn_pad visuals, which
 * intentionally sit UNDER the pads (flat 15 cm plinths, sunk so arrivals
 * don't wade through geometry).
 */
#include "World/TFSanctuaryDecor.h"

#include "Game/TFPlayerSystem.h"
#include "Game/TFUiSounds.h" // W10 audio-wave-2: class-terminal bleeps
#include "UI/TFMapScreen.h"
#include "UI/TFSpawnScreen.h"
#include "World/TFSanctuaryZone.h"
#include "World/TFWorldSetup.h"

#include "Engine/ECS/Components.h" // Transform, MeshRenderer
#include "Input/InputManager.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#ifdef SPARK_HAS_IMGUI
#include "UI/TFUiCommon.h"
#include <imgui.h>
#endif

#include <algorithm>
#include <cstdio>
#include <iterator> // std::size

namespace Terrafront
{

    namespace
    {

        // Class terminal: north of the spawn-pad rows, facing them. 30 m from
        // the nearest pad, 26 m from the plaza core, 106 m from the travel
        // terminal — so its [E] can never collide with the travel prompt.
        constexpr float kClassTermX = 320.0f;
        constexpr float kClassTermZ = 3826.0f;
        constexpr float kClassTerminalUseM = 4.5f;
        constexpr float kInteractDebounceSec = 0.25f; // travel-terminal pattern

        constexpr const char* kMatSanctuary = "Assets/Materials/MMOFPS/Structure_Sanctuary.json";
        constexpr const char* kMatMetal = "Assets/Materials/MMOFPS/Structure_Metal.json";
        constexpr const char* kMatConcrete = "Assets/Materials/MMOFPS/Structure_Concrete.json";

        /// One decor piece. yOff is relative to TerrainHeightAt(x, z) (24.0
        /// everywhere on the pad plateau); yaw in DEGREES (Transform Euler is
        /// degrees — the RADIANS rule is PhysicsBody-only).
        struct Piece
        {
            const char* model;
            const char* material;
            float x, z;
            float yawDeg;
            float sx, sy, sz;
            float emissive;
            bool castShadows;
            float yOff;
        };

        // Deterministic sanctuary layout. Grouped: spawn-pad plinths, plaza
        // core + balustrade runs, holo trees/signs, utility props, class
        // terminal (LAST — SpawnAll remembers its entity by position in this
        // table only implicitly; the terminal is located by the constants
        // above, not by entity, so order is purely cosmetic).
        constexpr Piece kLayout[] = {
            // sanc_spawn_pad plinths UNDER the four arrival pads (sunk 0.85 of
            // their 0.99 m height => ~15 cm proud of the deck, walk-over flat).
            {"Assets/Models/MMOFPS/buildings/sanc_spawn_pad.obj", kMatSanctuary, 296.0f, 3808.0f, 0.0f, 1.2f, 1.0f,
             1.2f, 0.5f, false, -0.85f},
            {"Assets/Models/MMOFPS/buildings/sanc_spawn_pad.obj", kMatSanctuary, 344.0f, 3808.0f, 0.0f, 1.2f, 1.0f,
             1.2f, 0.5f, false, -0.85f},
            {"Assets/Models/MMOFPS/buildings/sanc_spawn_pad.obj", kMatSanctuary, 296.0f, 3744.0f, 0.0f, 1.2f, 1.0f,
             1.2f, 0.5f, false, -0.85f},
            {"Assets/Models/MMOFPS/buildings/sanc_spawn_pad.obj", kMatSanctuary, 344.0f, 3744.0f, 0.0f, 1.2f, 1.0f,
             1.2f, 0.5f, false, -0.85f},

            // Plaza centerpiece north of the pad rows (27 m spire).
            {"Assets/Models/MMOFPS/buildings/sanc_plaza_core.obj", kMatSanctuary, 320.0f, 3852.0f, 0.0f, 1.0f, 1.0f,
             1.0f, 0.3f, true, 0.0f},

            // Balustrade runs framing the north plaza edge (4 m segments along
            // X, gapped at x=320 for the plaza-core approach).
            {"Assets/Models/MMOFPS/buildings/sanc_balustrade.obj", kMatSanctuary, 300.0f, 3864.0f, 0.0f, 1.0f, 1.0f,
             1.0f, 0.2f, false, 0.0f},
            {"Assets/Models/MMOFPS/buildings/sanc_balustrade.obj", kMatSanctuary, 304.0f, 3864.0f, 0.0f, 1.0f, 1.0f,
             1.0f, 0.2f, false, 0.0f},
            {"Assets/Models/MMOFPS/buildings/sanc_balustrade.obj", kMatSanctuary, 308.0f, 3864.0f, 0.0f, 1.0f, 1.0f,
             1.0f, 0.2f, false, 0.0f},
            {"Assets/Models/MMOFPS/buildings/sanc_balustrade.obj", kMatSanctuary, 312.0f, 3864.0f, 0.0f, 1.0f, 1.0f,
             1.0f, 0.2f, false, 0.0f},
            {"Assets/Models/MMOFPS/buildings/sanc_balustrade.obj", kMatSanctuary, 328.0f, 3864.0f, 0.0f, 1.0f, 1.0f,
             1.0f, 0.2f, false, 0.0f},
            {"Assets/Models/MMOFPS/buildings/sanc_balustrade.obj", kMatSanctuary, 332.0f, 3864.0f, 0.0f, 1.0f, 1.0f,
             1.0f, 0.2f, false, 0.0f},
            {"Assets/Models/MMOFPS/buildings/sanc_balustrade.obj", kMatSanctuary, 336.0f, 3864.0f, 0.0f, 1.0f, 1.0f,
             1.0f, 0.2f, false, 0.0f},
            {"Assets/Models/MMOFPS/buildings/sanc_balustrade.obj", kMatSanctuary, 340.0f, 3864.0f, 0.0f, 1.0f, 1.0f,
             1.0f, 0.2f, false, 0.0f},
            // Travel-terminal walkway rails (segments along Z; >= 14 m clear of
            // the terminal itself and of the south pad row).
            {"Assets/Models/MMOFPS/buildings/sanc_balustrade.obj", kMatSanctuary, 312.0f, 3734.0f, 90.0f, 1.0f, 1.0f,
             1.0f, 0.2f, false, 0.0f},
            {"Assets/Models/MMOFPS/buildings/sanc_balustrade.obj", kMatSanctuary, 312.0f, 3738.0f, 90.0f, 1.0f, 1.0f,
             1.0f, 0.2f, false, 0.0f},
            {"Assets/Models/MMOFPS/buildings/sanc_balustrade.obj", kMatSanctuary, 328.0f, 3734.0f, 90.0f, 1.0f, 1.0f,
             1.0f, 0.2f, false, 0.0f},
            {"Assets/Models/MMOFPS/buildings/sanc_balustrade.obj", kMatSanctuary, 328.0f, 3738.0f, 90.0f, 1.0f, 1.0f,
             1.0f, 0.2f, false, 0.0f},

            // Holo trees at the plaza corners (holo pieces glow, cast nothing —
            // decor.json holo convention).
            {"Assets/Models/MMOFPS/buildings/sanc_holo_tree.obj", kMatSanctuary, 284.0f, 3826.0f, 0.0f, 1.0f, 1.0f,
             1.0f, 0.8f, false, 0.0f},
            {"Assets/Models/MMOFPS/buildings/sanc_holo_tree.obj", kMatSanctuary, 356.0f, 3826.0f, 0.0f, 1.0f, 1.0f,
             1.0f, 0.8f, false, 0.0f},
            {"Assets/Models/MMOFPS/buildings/sanc_holo_tree.obj", kMatSanctuary, 292.0f, 3866.0f, 0.0f, 1.0f, 1.0f,
             1.0f, 0.8f, false, 0.0f},
            {"Assets/Models/MMOFPS/buildings/sanc_holo_tree.obj", kMatSanctuary, 348.0f, 3866.0f, 0.0f, 1.0f, 1.0f,
             1.0f, 0.8f, false, 0.0f},

            // Holo signage flanking the travel-terminal approach (11 m out).
            {"Assets/Models/MMOFPS/buildings/holo_sign_post.obj", kMatSanctuary, 312.0f, 3712.0f, 40.0f, 1.0f, 1.0f,
             1.0f, 0.8f, false, 0.0f},
            {"Assets/Models/MMOFPS/buildings/holo_sign_post.obj", kMatSanctuary, 328.0f, 3712.0f, -40.0f, 1.0f, 1.0f,
             1.0f, 0.8f, false, 0.0f},

            // Utility props: plaza floodlights, a holomap table by the core,
            // crate clutter near the west shelters.
            {"Assets/Models/MMOFPS/props/prop_floodlight.obj", kMatMetal, 296.0f, 3860.0f, 30.0f, 1.0f, 1.0f, 1.0f,
             0.4f, true, 0.0f},
            {"Assets/Models/MMOFPS/props/prop_floodlight.obj", kMatMetal, 344.0f, 3860.0f, -30.0f, 1.0f, 1.0f, 1.0f,
             0.4f, true, 0.0f},
            {"Assets/Models/MMOFPS/props/prop_holomap_table.obj", kMatMetal, 334.0f, 3788.0f, -20.0f, 1.0f, 1.0f, 1.0f,
             0.5f, true, 0.0f},
            {"Assets/Models/MMOFPS/props/prop_crate_m.obj", kMatConcrete, 262.0f, 3798.0f, 15.0f, 1.0f, 1.0f, 1.0f,
             0.0f, true, 0.0f},
            {"Assets/Models/MMOFPS/props/prop_crate_m.obj", kMatConcrete, 264.0f, 3795.5f, 70.0f, 1.0f, 1.0f, 1.0f,
             0.0f, true, 0.0f},
            {"Assets/Models/MMOFPS/props/prop_crate_s.obj", kMatConcrete, 262.8f, 3796.6f, 40.0f, 1.0f, 1.0f, 1.0f,
             0.0f, true, 0.9f},

            // Class terminal (kClassTermX/Z; yaw 180 = console faces the pads).
            {"Assets/Models/MMOFPS/buildings/sanc_terminal.obj", kMatSanctuary, kClassTermX, kClassTermZ, 180.0f, 1.0f,
             1.0f, 1.0f, 0.5f, true, 0.0f},
        };

    } // namespace

    TFSanctuaryDecor::TFSanctuaryDecor() = default;
    TFSanctuaryDecor::~TFSanctuaryDecor()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFSanctuaryDecor::Initialize(TFGameContext& ctx)
    {
        m_ctx = &ctx;
        m_initialized = true;

        auto& console = Spark::SimpleConsole::GetInstance();
        if (!console.HasCommand("tf_sanctuary_debug"))
        {
            console.RegisterCommand(
                "tf_sanctuary_debug",
                [this](const std::vector<std::string>&) -> std::string
                {
                    char buf[192];
                    std::snprintf(buf, sizeof(buf),
                                  "[TF] sanctuary decor: spawned=%d entities=%u (layout %zu) nearClassTerm=%d",
                                  m_spawned ? 1 : 0, SpawnedCount(), std::size(kLayout), m_nearClassTerm ? 1 : 0);
                    return std::string(buf);
                },
                "Sanctuary decor status: spawn state, entity total, class-terminal proximity", "TERRAFRONT",
                "tf_sanctuary_debug");
            m_debugCmd = true;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFSanctuaryDecor initialized (class terminal %.0f/%.0f)",
                       kClassTermX, kClassTermZ);
        return true;
    }

    void TFSanctuaryDecor::Shutdown()
    {
        if (!m_initialized)
            return;

        if (m_debugCmd)
        {
            Spark::SimpleConsole::GetInstance().UnregisterCommand("tf_sanctuary_debug");
            m_debugCmd = false;
        }

        World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;
        if (world)
        {
            for (const uint32_t ent : m_entities)
            {
                const auto e = static_cast<EntityID>(ent);
                if (ent != 0u && world->GetRegistry().valid(e))
                    world->DestroyEntity(e);
            }
        }
        m_entities.clear();
        m_spawned = false;
        m_initialized = false;
    }

    bool TFSanctuaryDecor::LocalPawn(float outPos[3]) const
    {
        if (!m_ctx || !m_ctx->players)
            return false;
        PawnInfo pawn{};
        if (!m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, pawn) || !pawn.alive)
            return false;
        outPos[0] = pawn.pos[0];
        outPos[1] = pawn.pos[1];
        outPos[2] = pawn.pos[2];
        return true;
    }

    void TFSanctuaryDecor::SpawnAll()
    {
        // Same readiness bar as TFRegionDecor minus the data tables (the layout
        // is compile-time): engine ECS world + TFWorldSetup (TerrainHeightAt).
        World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
        if (!world || !m_ctx->world)
            return;

        m_entities.reserve(std::size(kLayout));
        for (const Piece& p : kLayout)
        {
            const float y = m_ctx->world->TerrainHeightAt(p.x, p.z) + p.yOff;
            const auto ent = world->CreateEntity("TF_SancDecor");
            Transform& tr = world->AddComponent<Transform>(ent);
            tr.position = {p.x, y, p.z};
            tr.rotation.y = p.yawDeg; // Transform Euler is DEGREES
            tr.scale = {p.sx, p.sy, p.sz};
            MeshRenderer& mr = world->AddComponent<MeshRenderer>(ent);
            mr.meshPath = p.model;
            mr.materialPath = p.material;
            mr.castShadows = p.castShadows;
            mr.emissive = p.emissive;
            m_entities.push_back(static_cast<uint32_t>(ent));
        }

        m_spawned = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] sanctuary decor: stamped %zu entities", m_entities.size());
    }

    void TFSanctuaryDecor::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx)
            return;
        m_interactDebounce = std::max(0.0f, m_interactDebounce - deltaTime);

        if (!m_ctx->HasLocalPlayer())
            return; // dedicated server: never spawns decor, never polls input
        if (!m_spawned)
            SpawnAll();

        // ---- class-terminal interaction (travel-terminal UX pattern) ----------
        m_nearClassTerm = false;
        if (m_ctx->map && m_ctx->map->IsOpen())
            return;

        InputManager* input = m_ctx->engine ? m_ctx->engine->GetInput() : nullptr;
        float pos[3];
        if (!input || !LocalPawn(pos))
            return;

        const float dx = pos[0] - kClassTermX;
        const float dz = pos[2] - kClassTermZ;
        const bool nearTerm = dx * dx + dz * dz <= kClassTerminalUseM * kClassTerminalUseM;

        TFSpawnScreen* spawnUI = m_ctx->spawnUI;
        if (spawnUI && spawnUI->IsOpen())
        {
            // We only toggle-close the screen when WE opened it (terminal
            // mode); the death/deploy screen keeps its own lifecycle.
            if (spawnUI->IsTerminalMode() && nearTerm && input->WasKeyPressed('E') && m_interactDebounce <= 0.0f)
            {
                m_interactDebounce = kInteractDebounceSec;
                spawnUI->Close();
                TFUiSounds_Play(m_ctx, TFUiBleep::Close); // W10 audio-wave-2
            }
            return;
        }

        if (nearTerm)
        {
            m_nearClassTerm = true;
            if (spawnUI && input->WasKeyPressed('E') && m_interactDebounce <= 0.0f)
            {
                m_interactDebounce = kInteractDebounceSec;
                spawnUI->OpenClassTerminal();
                TFUiSounds_Play(m_ctx, TFUiBleep::Open); // W10 audio-wave-2
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Prompt
    // ---------------------------------------------------------------------------

#ifdef SPARK_HAS_IMGUI

    void TFSanctuaryDecor::RenderUI()
    {
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer() || !m_nearClassTerm)
            return;
        if ((m_ctx->map && m_ctx->map->IsOpen()) || (m_ctx->spawnUI && m_ctx->spawnUI->IsOpen()))
            return;

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const ImVec2 promptAt(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.58f);
        TFUi::AddTextCentered(fg, 17.0f, promptAt, IM_COL32(230, 230, 230, 220), "[E] Class terminal");
        TFUi::AddTextCentered(fg, 13.0f, ImVec2(promptAt.x, promptAt.y + 20.0f), IM_COL32(180, 220, 255, 190),
                              "select class for your next deploy");
    }

#else // !SPARK_HAS_IMGUI

    void TFSanctuaryDecor::RenderUI() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront

/**
 * @file TFNameplates.cpp
 * @brief Over-pawn nameplate rendering + the client-side damage-reveal
 *        tracker. See TFNameplates.h for the data-source and visibility
 *        contract.
 *
 * Drawn straight onto ImGui::GetForegroundDrawList() — no window, no input.
 * Suppressed while any fullscreen UI owns the screen (map / spawn screen /
 * scoreboard) and while the local pawn is dead (no first-person camera).
 */
#include "UI/TFNameplates.h"

#include "Data/TFDataTables.h"
#include "Game/TFAbilitySystem.h"
#include "Game/TFOutfitSystem.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFSocialSystem.h"
#include "Game/TFSquadSystem.h"
#include "Net/TFClientNet.h" // kTFLocalHostPlayer (scoreboard-style fallback label)
#include "UI/TFMapScreen.h"
#include "UI/TFScoreboard.h"
#include "UI/TFSpawnScreen.h"
#include "UI/TFUiCommon.h"
#include "World/TFWorldSetup.h"
#include "Camera/SparkEngineCamera.h"
#include "Utils/LogMacros.h"

#include "Core/Platform.h" // DirectXMath on Windows / vector-math stubs elsewhere
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace Terrafront
{

    TFNameplates::TFNameplates() = default;

    TFNameplates::~TFNameplates()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFNameplates::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;

        // Attacker reveal: when the LOCAL pawn takes damage, the attacker's
        // plate shows for the reveal window even if the shooter's own vitals
        // never move. Fires on authority (TFDamageSystem) and on pure clients
        // (TFClientNet::OnDamageEvent re-fire) — both set victim = local pawn
        // for local damage, so the gate below behaves identically per role.
        events.Subscribe<EvPlayerDamaged>([this](const EvPlayerDamaged& e) { OnPlayerDamagedBus(e); });

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFNameplates initialized");
        return true;
    }

    void TFNameplates::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx)
            return;
        m_clock += deltaTime;
        if (!m_ctx->HasLocalPlayer())
            return; // dedicated server: no UI, no tracking work
        SweepReplicatedVitals();
    }

    void TFNameplates::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime; // presentation-only system; nothing simulates
    }

    void TFNameplates::Shutdown()
    {
        if (!m_initialized)
            return;
        m_prevVitals.clear();
        m_curVitals.clear();
        m_revealUntil.clear();
        m_initialized = false;
    }

    // ---------------------------------------------------------------------------
    // Damage-reveal tracking (no ImGui — runs headless-safe)
    // ---------------------------------------------------------------------------

    void TFNameplates::RevealPawn(EntityId pawnNetEntity)
    {
        if (pawnNetEntity == 0)
            return;
        m_revealUntil[pawnNetEntity] = m_clock + kTFNameplateRevealSec;
    }

    void TFNameplates::OnPlayerDamagedBus(const EvPlayerDamaged& ev)
    {
        if (!m_initialized || !m_ctx || !m_ctx->players || ev.attacker == 0)
            return;
        // Only the local pawn's damage may reveal the attacker: on authority
        // roles this event fires for EVERY hit in the world, and revealing
        // third-party attackers would give the listen host intel pure clients
        // never get (parity rule).
        PawnInfo me{};
        if (!m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, me) || ev.victim != me.entity)
            return;
        RevealPawn(ev.attacker);
    }

    void TFNameplates::SweepReplicatedVitals()
    {
        if (!m_ctx->players)
            return;

        const PlayerId local = m_ctx->localPlayer;
        m_curVitals.clear();
        m_ctx->players->ForEachAlivePawn(
            [&](const PawnInfo& p)
            {
                if (p.owner == local)
                    return;
                const float vitals = p.health + p.shield;
                m_curVitals.emplace(p.entity, vitals);
                const auto it = m_prevVitals.find(p.entity);
                if (it != m_prevVitals.end() && vitals < it->second - 0.25f)
                    RevealPawn(p.entity); // replicated vitals dropped == damaged
            });
        std::swap(m_prevVitals, m_curVitals);

        // Prune expired reveals + entities that no longer exist (respawn gives
        // a fresh entity id, so stale keys only cost memory, never a wrong plate).
        uint32_t live = 0;
        std::erase_if(m_revealUntil,
                      [&](const auto& kv)
                      {
                          if (kv.second <= m_clock)
                              return true;
                          ++live;
                          return false;
                      });
        m_revealedNow = live;
    }

    // ---------------------------------------------------------------------------
    // Rendering
    // ---------------------------------------------------------------------------

#ifdef SPARK_HAS_IMGUI

    namespace
    {

        /// Scoreboard-parity fallback when the social roster has no name
        /// (bots, pre-roster joiners): "HOST" / "P<n>" (TFScoreboard::PlayerLabel).
        void FallbackLabel(PlayerId id, char out[16])
        {
            if (id == kTFLocalHostPlayer)
                std::snprintf(out, 16, "HOST");
            else
                std::snprintf(out, 16, "P%u", id);
        }

        /// World -> screen through the SAME matrices the frame was rendered
        /// with. MUST MATCH TFWorldSetup::ComputeViewProj's first-person
        /// branch (LookAtLH from the module camera pose; PerspectiveFovLH
        /// with XM_PIDIV4 * 1.6, window aspect, 0.3, 6000). ComputeViewProj
        /// is private to the world lane, so the build is mirrored here —
        /// if the render FOV or clip planes ever change, change this too or
        /// plates drift off the pawns.
        bool ProjectToScreen(const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj, const ImGuiViewport& vp,
                             const float world[3], ImVec2& out)
        {
            using namespace DirectX;
            const XMVECTOR ws = XMVectorSet(world[0], world[1], world[2], 1.0f);
            const XMVECTOR vs = XMVector3TransformCoord(ws, view);
            if (XMVectorGetZ(vs) < 0.3f)
                return false; // behind / inside the near plane (LH: +z forward)
            const XMVECTOR ndc = XMVector3TransformCoord(vs, proj);
            const float nx = XMVectorGetX(ndc);
            const float ny = XMVectorGetY(ndc);
            if (nx < -1.25f || nx > 1.25f || ny < -1.25f || ny > 1.25f)
                return false; // comfortably off-screen
            out.x = vp.Pos.x + (nx * 0.5f + 0.5f) * vp.Size.x;
            out.y = vp.Pos.y + (0.5f - ny * 0.5f) * vp.Size.y;
            return true;
        }

    } // namespace

    void TFNameplates::RenderUI()
    {
        m_platesDrawn = 0;
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer() || !m_ctx->InWorld())
            return;
        if (!m_ctx->players || !m_ctx->world)
            return;
        // Fullscreen UIs own the screen; foreground-drawlist plates would sit
        // on top of them.
        if ((m_ctx->map && m_ctx->map->IsOpen()) || (m_ctx->spawnUI && m_ctx->spawnUI->IsOpen()) ||
            (m_ctx->scoreboard && m_ctx->scoreboard->IsOpen()))
            return;
        SparkEngineCamera* cam = m_ctx->world->GetCamera();
        if (!cam)
            return;
        PawnInfo me{};
        if (!m_ctx->players->GetPawnByPlayer(m_ctx->localPlayer, me) || !me.alive)
            return; // dead: no first-person camera to project through

        using namespace DirectX;
        const XMFLOAT3 cp = cam->GetPosition();
        const XMFLOAT3 cf = cam->GetForward();
        float fx = cf.x, fy = cf.y, fz = cf.z;
        const float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
        if (flen < 1e-4f)
            return;
        fx /= flen;
        fy /= flen;
        fz /= flen;

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        if (!vp || vp->Size.x <= 0.0f || vp->Size.y <= 0.0f)
            return;
        const XMVECTOR eye = XMLoadFloat3(&cp);
        const XMVECTOR fwd = XMVectorSet(fx, fy, fz, 0.0f);
        const XMMATRIX view = XMMatrixLookAtLH(eye, XMVectorAdd(eye, fwd), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
        const XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4 * 1.6f, vp->Size.x / vp->Size.y, 0.3f, 6000.0f);

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const FactionId myFaction = m_ctx->localFaction;
        const PlayerId local = m_ctx->localPlayer;

        m_ctx->players->ForEachAlivePawn(
            [&](const PawnInfo& p)
            {
                if (p.owner == local)
                    return;

                // Plate anchor: head, ~1.9 m above replicated feet position.
                const float head[3] = {p.pos[0], p.pos[1] + kTFNameplateHeadM, p.pos[2]};
                const float dx = head[0] - cp.x, dy = head[1] - cp.y, dz = head[2] - cp.z;
                const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (dist > kTFNameplateMaxDistM || dist < 0.75f)
                    return;

                const bool friendly = myFaction != FactionId::None && p.faction == myFaction;

                // Cloak coordination: an enemy veiled Ghost's mesh is ghosted
                // by TFAbilitySystem::UpdateVeilVisuals (same faction test) —
                // its plate vanishes for exactly the same frames. Friendly
                // veiled Ghosts keep mesh AND plate.
                if (!friendly && m_ctx->abilities && m_ctx->abilities->IsPawnVeiled(p.entity))
                    return;

                if (!friendly)
                {
                    const float ax = dx / dist, ay = dy / dist, az = dz / dist;
                    const bool aimed = (ax * fx + ay * fy + az * fz) >= kTFNameplateAimConeCos;
                    const auto it = m_revealUntil.find(p.entity);
                    const bool revealed = it != m_revealUntil.end() && it->second > m_clock;
                    if (!aimed && !revealed)
                        return;
                }

                ImVec2 sp;
                if (!ProjectToScreen(view, proj, *vp, head, sp))
                    return;

                // Distance fade: full inside 30 m, gone at 80 m.
                float alpha = 1.0f;
                if (dist > kTFNameplateFadeStartM)
                    alpha = 1.0f - (dist - kTFNameplateFadeStartM) / (kTFNameplateMaxDistM - kTFNameplateFadeStartM);
                alpha = std::clamp(alpha, 0.0f, 1.0f);
                if (alpha <= 0.02f)
                    return;
                const int a8 = static_cast<int>(alpha * 255.0f);

                // Name: social roster (what chat/social display) with the
                // scoreboard "HOST"/"P<n>" fallback, plus "[TAG]" outfit prefix.
                char base[32];
                std::string rosterName;
                if (m_ctx->social && m_ctx->social->NameOfPlayer(p.owner, rosterName))
                    std::snprintf(base, sizeof(base), "%s", rosterName.c_str());
                else
                    FallbackLabel(p.owner, base);
                char label[48];
                OutfitTaggedLabel(m_ctx->outfits ? m_ctx->outfits->GetOutfitTag(p.owner) : "", base, label,
                                  sizeof(label));

                const bool squadmate = friendly && m_ctx->squads && m_ctx->squads->IsLocalSquadMember(p.owner);
                const ImU32 nameCol = squadmate ? IM_COL32(110, 235, 140, a8) : TFUi::FactionCol(p.faction, alpha);

                // Name (1 px drop shadow for readability over bright terrain),
                // slightly larger up close.
                const float fontSize = ImGui::GetFontSize() * (1.15f - 0.35f * (dist / kTFNameplateMaxDistM));
                const ImVec2 namePos(sp.x, sp.y - fontSize * 0.5f - 4.0f);
                TFUi::AddTextCentered(dl, fontSize, ImVec2(namePos.x + 1.0f, namePos.y + 1.0f),
                                      IM_COL32(0, 0, 0, (a8 * 3) / 4), label);
                TFUi::AddTextCentered(dl, fontSize, namePos, nameCol, label);

                // Vitals: class max pools from classes.json (HUD PawnView
                // pattern); replicated current values from the pawn registry.
                float maxHealth = 500.0f, maxShield = 500.0f;
                if (m_ctx->data && m_ctx->data->IsLoaded())
                {
                    if (const ClassDef* cd = m_ctx->data->GetClass(p.cls))
                    {
                        maxHealth = cd->health;
                        maxShield = cd->shield;
                    }
                }
                const float hp01 = maxHealth > 0.0f ? std::clamp(p.health / maxHealth, 0.0f, 1.0f) : 0.0f;
                const float sh01 = maxShield > 0.0f ? std::clamp(p.shield / maxShield, 0.0f, 1.0f) : 0.0f;

                const float barW = 46.0f, barH = 4.0f;
                const ImVec2 b0(sp.x - barW * 0.5f, sp.y + 3.0f);
                const ImVec2 b1(sp.x + barW * 0.5f, sp.y + 3.0f + barH);
                dl->AddRectFilled(b0, b1, IM_COL32(12, 14, 18, (a8 * 2) / 3), 1.0f);
                if (hp01 > 0.0f)
                    dl->AddRectFilled(b0, ImVec2(b0.x + barW * hp01, b1.y), TFUi::FactionCol(p.faction, 0.92f * alpha),
                                      1.0f);
                if (sh01 > 0.0f) // thin shield strip above (HUD vitals cyan)
                    dl->AddRectFilled(ImVec2(b0.x, b0.y - 3.0f), ImVec2(b0.x + barW * sh01, b0.y - 1.0f),
                                      IM_COL32(120, 210, 255, (a8 * 4) / 5), 1.0f);

                ++m_platesDrawn;
            });
    }

    void TFNameplates::RenderDebugUI()
    {
        if (!ImGui::CollapsingHeader("TF Nameplates"))
            return;
        ImGui::Text("plates drawn : %u", m_platesDrawn);
        ImGui::Text("revealed now : %u", m_revealedNow);
        ImGui::Text("tracked pawns: %zu", m_prevVitals.size());
    }

#else // !SPARK_HAS_IMGUI — headless: reveal tracking only

    void TFNameplates::RenderUI() {}
    void TFNameplates::RenderDebugUI() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront

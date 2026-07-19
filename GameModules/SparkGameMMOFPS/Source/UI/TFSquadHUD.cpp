/**
 * @file TFSquadHUD.cpp
 * @brief W11 squad-v2: left-edge squadmate list, waypoint world beacon, and
 *        leader-only waypoint request pings. See TFSquadHUD.h for the data
 *        sources and gating contract.
 *
 * The list lives in a background-dimmed, no-decoration ImGui window (rows use
 * the window drawlist; the leader's SET/X/CLEAR controls are real buttons —
 * the same live-HUD ImGui interaction the outfit panel and vehicle terminals
 * already rely on). The beacon draws straight onto the foreground drawlist
 * (TFNameplates pattern: no window, no input) and lives in
 * TFSquadHUDBeacon.cpp with the debug panel (repo file-size rule, TFHUDDraw
 * pattern); the shared SquadCol accent lives in TFSquadHUDInternal.h.
 */
#include "UI/TFSquadHUD.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFSocialSystem.h"
#include "Game/TFSquadSystem.h"
#include "Net/TFClientNet.h" // kTFLocalHostPlayer (scoreboard-style fallback label)
#include "UI/TFMapScreen.h"
#include "UI/TFScoreboard.h"
#include "UI/TFSpawnScreen.h"
#include "UI/TFSquadHUDInternal.h"
#include "UI/TFUiCommon.h"
#include "World/TFWorldSetup.h"
#include "Camera/SparkEngineCamera.h"
#include "Utils/LogMacros.h"

#include "Core/Platform.h" // DirectXMath on Windows / vector-math stubs elsewhere
#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace Terrafront
{

    using namespace SquadHudDetail; // TFSquadHUDInternal.h: SquadCol shared with the beacon part

    TFSquadHUD::TFSquadHUD() = default;

    TFSquadHUD::~TFSquadHUD()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFSquadHUD::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;
        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFSquadHUD initialized");
        return true;
    }

    void TFSquadHUD::Update(float deltaTime)
    {
        if (!m_initialized)
            return;
        m_clock += deltaTime; // beacon pulse / ping blink only
    }

    void TFSquadHUD::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime; // presentation-only system; nothing simulates
    }

    void TFSquadHUD::Shutdown()
    {
        if (!m_initialized)
            return;
        m_initialized = false;
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

        /// Two-letter class tag + a stable per-class accent for the tag box.
        const char* ClassTag(ClassId cls)
        {
            switch (cls)
            {
            case ClassId::Ghost:
                return "GH";
            case ClassId::Striker:
                return "ST";
            case ClassId::Medtech:
                return "MD";
            case ClassId::Fabricator:
                return "FB";
            case ClassId::Bulwark:
                return "BW";
            case ClassId::Colossus:
                return "CO";
            default:
                return "--";
            }
        }

        ImU32 ClassCol(ClassId cls, int a8)
        {
            switch (cls)
            {
            case ClassId::Ghost:
                return IM_COL32(168, 140, 220, a8); // recon violet
            case ClassId::Striker:
                return IM_COL32(235, 160, 80, a8); // jump-jet orange
            case ClassId::Medtech:
                return IM_COL32(120, 220, 130, a8); // heal green
            case ClassId::Fabricator:
                return IM_COL32(225, 205, 95, a8); // engineer yellow
            case ClassId::Bulwark:
                return IM_COL32(110, 170, 240, a8); // shield blue
            case ClassId::Colossus:
                return IM_COL32(230, 105, 100, a8); // exosuit red
            default:
                return IM_COL32(160, 160, 160, a8);
            }
        }

        /// Leader star: two overlapped filled triangles (hexagram) — star
        /// shapes are concave, so no single AddConvexPolyFilled call works.
        void AddLeaderStar(ImDrawList* dl, ImVec2 c, float r, ImU32 col)
        {
            const float h = r * 0.55f;
            dl->AddTriangleFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r * 0.87f, c.y + h),
                                  ImVec2(c.x - r * 0.87f, c.y + h), col);
            dl->AddTriangleFilled(ImVec2(c.x, c.y + r), ImVec2(c.x + r * 0.87f, c.y - h),
                                  ImVec2(c.x - r * 0.87f, c.y - h), col);
        }

        /// Small triangle chevron rotated by `angleRad` (0 = straight up/ahead).
        void AddChevron(ImDrawList* dl, ImVec2 c, float r, float angleRad, ImU32 col)
        {
            const float s = std::sin(angleRad), co = std::cos(angleRad);
            auto rot = [&](float x, float y) { return ImVec2(c.x + x * co - y * s, c.y + x * s + y * co); };
            const ImVec2 tri[3] = {rot(0.0f, -r), rot(0.72f * r, r * 0.85f), rot(-0.72f * r, r * 0.85f)};
            dl->AddTriangleFilled(tri[0], tri[1], tri[2], col);
        }

    } // namespace

    void TFSquadHUD::RenderUI()
    {
        m_rowsDrawn = 0;
        m_beaconDrawn = false;
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer() || !m_ctx->InWorld())
            return;
        if (!m_ctx->squads || !m_ctx->players)
            return;
        // Fullscreen UIs own the screen (nameplates gating).
        if ((m_ctx->map && m_ctx->map->IsOpen()) || (m_ctx->spawnUI && m_ctx->spawnUI->IsOpen()) ||
            (m_ctx->scoreboard && m_ctx->scoreboard->IsOpen()))
            return;

        DrawWaypointBeacon(); // world-space first: the list overlays it
        DrawSquadList();
    }

    void TFSquadHUD::DrawSquadList()
    {
        const TFSquadSystem::LocalSquadView view = m_ctx->squads->GetLocalSquadView();
        if (view.squad == kInvalidSquad)
            return;

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        if (!vp || vp->Size.x <= 0.0f || vp->Size.y <= 0.0f)
            return;

        // Leader first, then the rest in roster order (stable, no flicker).
        std::vector<PlayerId> ordered;
        ordered.reserve(view.members.size());
        if (view.leader != kInvalidPlayer)
            ordered.push_back(view.leader);
        for (const PlayerId member : view.members)
            if (member != view.leader)
                ordered.push_back(member);

        const PlayerId local = m_ctx->localPlayer;
        const bool isLeader = view.leader == local;

        PawnInfo me{};
        const bool haveMyPawn = m_ctx->players->GetPawnByPlayer(local, me) && me.alive;

        // Camera XZ forward for the direction chevrons (skipped while dead).
        float camFX = 0.0f, camFZ = 1.0f;
        bool haveCamDir = false;
        if (haveMyPawn && m_ctx->world)
        {
            if (SparkEngineCamera* cam = m_ctx->world->GetCamera())
            {
                const auto cf = cam->GetForward();
                const float len = std::sqrt(cf.x * cf.x + cf.z * cf.z);
                if (len > 1e-4f)
                {
                    camFX = cf.x / len;
                    camFZ = cf.z / len;
                    haveCamDir = true;
                }
            }
        }

        constexpr float kListW = 236.0f;
        constexpr float kRowH = 34.0f;
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 10.0f, vp->Pos.y + vp->Size.y * kTFSquadHudListAnchorY01));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.06f, 0.08f, 0.42f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
        if (!ImGui::Begin("##tf_squadhud", nullptr,
                          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
                              ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                              ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::End();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
            return;
        }

        ImGui::TextColored(ImVec4(0.43f, 0.92f, 0.55f, 0.95f), "SQUAD %u", view.squad);
        ImGui::Separator();

        ImDrawList* dl = ImGui::GetWindowDrawList();

        for (const PlayerId member : ordered)
        {
            PawnInfo pi{};
            const bool hasPawn = m_ctx->players->GetPawnByPlayer(member, pi);
            const bool alive = hasPawn && pi.alive;

            std::string rosterName;
            const bool named = m_ctx->social && m_ctx->social->NameOfPlayer(member, rosterName);
            const bool offline = !named && !hasPawn && m_ctx->social != nullptr;

            char base[32];
            if (named)
                std::snprintf(base, sizeof(base), "%s", rosterName.c_str());
            else
                FallbackLabel(member, base);

            const ImVec2 row = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(kListW - 18.0f, kRowH)); // reserves the row rect

            const int a8 = (alive && !offline) ? 255 : 110; // dead/offline grayed
            float rowX = row.x + 2.0f;

            // Leader star.
            if (member == view.leader)
                AddLeaderStar(dl, ImVec2(rowX + 5.0f, row.y + 9.0f), 6.0f, IM_COL32(255, 208, 70, a8));
            rowX += 14.0f;

            // Class tag box (last replicated class; unknown when no pawn).
            const ClassId cls = hasPawn ? pi.cls : ClassId::COUNT;
            const ImVec2 tb0(rowX, row.y + 2.0f);
            const ImVec2 tb1(rowX + 24.0f, row.y + 16.0f);
            dl->AddRectFilled(tb0, tb1, IM_COL32(16, 18, 22, (a8 * 3) / 4), 2.0f);
            dl->AddRect(tb0, tb1, ClassCol(cls, a8), 2.0f);
            TFUi::AddTextCentered(dl, ImGui::GetFontSize() * 0.78f,
                                  ImVec2((tb0.x + tb1.x) * 0.5f, (tb0.y + tb1.y) * 0.5f), ClassCol(cls, a8),
                                  ClassTag(cls));
            rowX += 30.0f;

            // Name (squad green; gray when dead/offline; YOU marker for self).
            char label[48];
            std::snprintf(label, sizeof(label), "%s%s", base, member == local ? " (YOU)" : "");
            const ImU32 nameCol = (alive && !offline) ? SquadCol(255) : IM_COL32(170, 174, 180, 150);
            dl->AddText(ImVec2(rowX, row.y + 1.0f), nameCol, label);

            // Health bar (nameplates vitals pattern) or DOWN/OFFLINE state.
            const ImVec2 b0(rowX, row.y + 19.0f);
            const ImVec2 b1(rowX + 92.0f, row.y + 23.0f);
            dl->AddRectFilled(b0, b1, IM_COL32(12, 14, 18, 170), 1.0f);
            if (alive)
            {
                float maxHealth = 500.0f;
                if (m_ctx->data && m_ctx->data->IsLoaded())
                    if (const ClassDef* cd = m_ctx->data->GetClass(pi.cls))
                        maxHealth = cd->health;
                const float hp01 = maxHealth > 0.0f ? std::clamp(pi.health / maxHealth, 0.0f, 1.0f) : 0.0f;
                if (hp01 > 0.0f)
                    dl->AddRectFilled(b0, ImVec2(b0.x + 92.0f * hp01, b1.y), SquadCol(230), 1.0f);
            }
            else
            {
                dl->AddText(ImVec2(b0.x, row.y + 16.0f),
                            offline ? IM_COL32(150, 152, 156, 170) : IM_COL32(235, 96, 80, 200),
                            offline ? "OFFLINE" : "DOWN");
            }

            // Distance + direction chevron (needs both pawns; chevron needs camera).
            if (haveMyPawn && alive && member != local)
            {
                const float dx = pi.pos[0] - me.pos[0];
                const float dz = pi.pos[2] - me.pos[2];
                const float dist = std::sqrt(dx * dx + dz * dz);
                char dtxt[16];
                std::snprintf(dtxt, sizeof(dtxt), "%.0fm", dist);
                const ImVec2 dsz = ImGui::CalcTextSize(dtxt);
                dl->AddText(ImVec2(row.x + kListW - 34.0f - dsz.x, row.y + 1.0f), IM_COL32(208, 212, 218, a8), dtxt);
                if (haveCamDir && dist > 1.0f)
                {
                    // Signed angle from camera forward to the member (XZ):
                    // 0 = ahead, +right, -left; chevron up == ahead.
                    const float nx = dx / dist, nz = dz / dist;
                    const float rel = std::atan2(nx * camFZ - nz * camFX, nx * camFX + nz * camFZ);
                    AddChevron(dl, ImVec2(row.x + kListW - 26.0f, row.y + 8.0f), 6.0f, rel,
                               IM_COL32(208, 212, 218, a8));
                }
            }

            ++m_rowsDrawn;
        }

        // ---- waypoint status + leader controls ---------------------------------
        float wp[3];
        const bool haveWp = m_ctx->squads->GetLocalWaypoint(wp);
        if (haveWp)
        {
            ImGui::Separator();
            char wtxt[48];
            if (haveMyPawn)
            {
                const float dx = wp[0] - me.pos[0], dz = wp[2] - me.pos[2];
                std::snprintf(wtxt, sizeof(wtxt), "WAYPOINT  %.0fm", std::sqrt(dx * dx + dz * dz));
            }
            else
                std::snprintf(wtxt, sizeof(wtxt), "WAYPOINT");
            ImGui::TextColored(ImVec4(0.43f, 0.92f, 0.55f, 0.95f), "%s", wtxt);
            if (isLeader)
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("CLEAR##tfwp"))
                    m_ctx->squads->UiClearWaypoint();
            }
        }
        else if (isLeader)
        {
            ImGui::Separator();
            ImGui::TextDisabled("ctrl-click map: set waypoint");
        }

        // ---- leader-only waypoint request pings ---------------------------------
        if (isLeader)
        {
            const auto& requests = m_ctx->squads->PendingWaypointRequests();
            if (!requests.empty())
            {
                ImGui::Separator();
                const float blink = 0.75f + 0.25f * std::sin(m_clock * 5.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.30f, blink), "WAYPOINT REQUESTS");
                for (size_t i = 0; i < requests.size(); ++i)
                {
                    const TFSquadSystem::WaypointRequest& rq = requests[i];
                    std::string fromName;
                    char from[32];
                    if (m_ctx->social && m_ctx->social->NameOfPlayer(rq.from, fromName))
                        std::snprintf(from, sizeof(from), "%s", fromName.c_str());
                    else
                        FallbackLabel(rq.from, from);
                    char rtxt[64];
                    if (haveMyPawn)
                    {
                        const float dx = rq.wp[0] - me.pos[0], dz = rq.wp[2] - me.pos[2];
                        std::snprintf(rtxt, sizeof(rtxt), "%s  %.0fm", from, std::sqrt(dx * dx + dz * dz));
                    }
                    else
                        std::snprintf(rtxt, sizeof(rtxt), "%s", from);
                    ImGui::TextUnformatted(rtxt);
                    ImGui::SameLine(kListW - 88.0f);
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::SmallButton("SET"))
                    {
                        m_ctx->squads->PromoteWaypointRequest(i);
                        ImGui::PopID();
                        break; // vector mutated — resume next frame
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("X"))
                    {
                        m_ctx->squads->DismissWaypointRequest(i);
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
            }
        }

        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

#else // !SPARK_HAS_IMGUI — headless: nothing to draw

    void TFSquadHUD::RenderUI() {}
    void TFSquadHUD::DrawSquadList() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront

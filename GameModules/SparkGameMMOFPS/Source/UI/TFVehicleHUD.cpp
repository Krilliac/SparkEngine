/**
 * @file TFVehicleHUD.cpp
 * @brief W10 vehicle HUD bodies — seated cockpit widget (name / hull HP /
 *        speed / seat diagram / Aegis deploy state / Vulture altitude) and the
 *        per-seat crosshair (turret reticle + weapon name for armed seats,
 *        minimal dot for passengers). See TFVehicleHUD.h for ownership and the
 *        read-only contract; ImGui bodies compile only under SPARK_HAS_IMGUI.
 */
#include "UI/TFVehicleHUD.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFVehicleSystem.h"
#include "Net/TFClientNet.h"
#include "UI/TFMapScreen.h"
#include "UI/TFSpawnScreen.h"
#include "World/TFWorldSetup.h"

#ifdef SPARK_HAS_IMGUI
#include "UI/TFUiCommon.h"
#include <imgui.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace Terrafront
{

#ifdef SPARK_HAS_IMGUI

    namespace
    {

        constexpr float kPanelW = 300.0f;      ///< widget width, bottom-center
        constexpr float kHpBarH = 14.0f;       ///< hull HP bar height
        constexpr float kSeatBoxW = 26.0f;     ///< seat diagram cell width
        constexpr float kSeatBoxH = 20.0f;     ///< seat diagram cell height
        constexpr float kSeatBoxGap = 4.0f;    ///< gap between cells
        constexpr float kSpeedSnapMS = 150.0f; ///< instantaneous estimate above this = teleport, ignore

        /// Display initial for a vehicles.json seat role ("driver" -> 'D').
        char RoleInitial(const std::string& role)
        {
            if (role.empty())
                return 'S';
            return static_cast<char>(std::toupper(static_cast<unsigned char>(role[0])));
        }

    } // namespace

    void TFVehicleHUD::Render(TFGameContext& ctx, TFVehicleSystem& vehicles)
    {
        if (!ctx.data || !ctx.data->IsLoaded())
        {
            m_hasLast = false;
            return;
        }
        // Fullscreen UIs own the screen; keep the speed estimate warm though.
        if ((ctx.map && ctx.map->IsOpen()) || (ctx.spawnUI && ctx.spawnUI->IsOpen()))
            return;

        PlayerId pid = ctx.localPlayer;
        if (pid == kInvalidPlayer && ctx.clientNet)
            pid = ctx.clientNet->LocalPlayerId();
        if (pid == kInvalidPlayer || !ctx.players)
        {
            m_hasLast = false;
            return;
        }
        PawnInfo pawn{};
        if (!ctx.players->GetPawnByPlayer(pid, pawn) || !pawn.alive)
        {
            m_hasLast = false;
            return;
        }

        EntityId veh = 0;
        uint8_t mySeat = 0;
        if (!vehicles.GetSeatOf(pid, veh, mySeat))
        {
            m_hasLast = false;
            m_lastVehicle = 0;
            return;
        }
        TFVehicleInfo info;
        if (!vehicles.GetVehicleInfo(veh, info))
            return;
        const VehicleDef* def = ctx.data->GetVehicle(info.vehId);

        // --- speed estimate (frame-to-frame hull motion, both roles) -----------
        if (veh != m_lastVehicle)
        {
            m_lastVehicle = veh;
            m_hasLast = false;
            m_speedSmooth = 0.0f;
        }
        const float dt = ImGui::GetIO().DeltaTime;
        if (m_hasLast && dt > 1.0e-4f)
        {
            const float dx = info.pos[0] - m_lastPos[0];
            const float dy = info.pos[1] - m_lastPos[1];
            const float dz = info.pos[2] - m_lastPos[2];
            const float inst = std::sqrt(dx * dx + dy * dy + dz * dz) / dt;
            if (inst < kSpeedSnapMS) // ignore create/teleport snaps
                m_speedSmooth += (inst - m_speedSmooth) * std::min(1.0f, dt * 8.0f);
        }
        m_lastPos[0] = info.pos[0];
        m_lastPos[1] = info.pos[1];
        m_lastPos[2] = info.pos[2];
        m_hasLast = true;

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const float cx = vp->Pos.x + vp->Size.x * 0.5f;
        char buf[128];

        // --- crosshair: armed seat = turret reticle, unarmed = minimal dot -----
        const bool armed = def && static_cast<size_t>(mySeat) < def->seats.size() &&
                           !def->seats[static_cast<size_t>(mySeat)].weaponKey.empty();
        const ImVec2 sc(cx, vp->Pos.y + vp->Size.y * 0.5f);
        if (armed)
        {
            const ImU32 col = IM_COL32(255, 235, 170, 220);
            const float r = 13.0f, tick = 6.0f;
            fg->AddCircle(sc, r, col, 24, 1.5f);
            fg->AddLine(ImVec2(sc.x, sc.y - r - tick), ImVec2(sc.x, sc.y - r + 2.0f), col, 2.0f);
            fg->AddLine(ImVec2(sc.x, sc.y + r - 2.0f), ImVec2(sc.x, sc.y + r + tick), col, 2.0f);
            fg->AddLine(ImVec2(sc.x - r - tick, sc.y), ImVec2(sc.x - r + 2.0f, sc.y), col, 2.0f);
            fg->AddLine(ImVec2(sc.x + r - 2.0f, sc.y), ImVec2(sc.x + r + tick, sc.y), col, 2.0f);
            fg->AddCircleFilled(sc, 1.5f, col);
            const WeaponDef* w = ctx.data->GetWeaponByKey(def->seats[static_cast<size_t>(mySeat)].weaponKey);
            std::snprintf(buf, sizeof(buf), "%s", w ? w->name.c_str() : "turret");
            TFUi::AddTextCentered(fg, 14.0f, ImVec2(sc.x, sc.y + r + tick + 12.0f), IM_COL32(255, 235, 170, 190), buf);
        }
        else
        {
            fg->AddCircleFilled(sc, 2.0f, IM_COL32(255, 255, 255, 170));
        }

        // --- cockpit widget (bottom-center) -------------------------------------
        const float panelX = cx - kPanelW * 0.5f;
        float y = vp->Pos.y + vp->Size.y - 148.0f;
        fg->AddRectFilled(ImVec2(panelX - 10.0f, y - 8.0f),
                          ImVec2(panelX + kPanelW + 10.0f, vp->Pos.y + vp->Size.y - 40.0f), IM_COL32(10, 12, 16, 150),
                          5.0f);

        // name (+ my role)
        const char* myRole = (def && static_cast<size_t>(mySeat) < def->seats.size())
                                 ? def->seats[static_cast<size_t>(mySeat)].role.c_str()
                                 : "seat";
        std::snprintf(buf, sizeof(buf), "%s  -  %s", def ? def->name.c_str() : "VEHICLE", myRole);
        TFUi::AddTextCentered(fg, 16.0f, ImVec2(cx, y), IM_COL32(235, 235, 235, 230), buf);
        y += 14.0f;

        // hull HP bar (style carried over from the W3 seated bar)
        if (info.maxHp > 0.0f)
        {
            const float frac = std::clamp(info.hp / info.maxHp, 0.0f, 1.0f);
            fg->AddRectFilled(ImVec2(panelX, y), ImVec2(panelX + kPanelW, y + kHpBarH), IM_COL32(10, 12, 16, 200),
                              3.0f);
            const ImU32 fill = frac > 0.5f    ? IM_COL32(120, 200, 130, 230)
                               : frac > 0.25f ? IM_COL32(230, 190, 80, 230)
                                              : IM_COL32(220, 80, 70, 230);
            fg->AddRectFilled(ImVec2(panelX + 2.0f, y + 2.0f),
                              ImVec2(panelX + 2.0f + (kPanelW - 4.0f) * frac, y + kHpBarH - 2.0f), fill, 2.0f);
            fg->AddRect(ImVec2(panelX, y), ImVec2(panelX + kPanelW, y + kHpBarH), IM_COL32(200, 200, 200, 120), 3.0f);
            std::snprintf(buf, sizeof(buf), "%.0f / %.0f", info.hp, info.maxHp);
            TFUi::AddTextCentered(fg, 12.0f, ImVec2(cx, y + kHpBarH * 0.5f), IM_COL32(240, 240, 240, 230), buf);
        }
        y += kHpBarH + 10.0f;

        // speed + per-vehicle extras (Vulture altitude, Aegis deploy state)
        int n = std::snprintf(buf, sizeof(buf), "%.0f m/s", m_speedSmooth);
        if (info.vehId == VehicleId::Vulture && ctx.world && n > 0 && n < static_cast<int>(sizeof(buf)))
        {
            const float agl = info.pos[1] - ctx.world->TerrainHeightAt(info.pos[0], info.pos[2]);
            std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), "   ALT %.0f m", std::max(0.0f, agl));
        }
        TFUi::AddTextCentered(fg, 14.0f, ImVec2(cx, y), IM_COL32(210, 220, 235, 220), buf);
        y += 15.0f;
        if (def && def->hasDeploySpawn)
        {
            std::snprintf(buf, sizeof(buf), "%s", info.deployed ? "DEPLOYED - mobile spawn active" : "not deployed");
            TFUi::AddTextCentered(fg, 13.0f, ImVec2(cx, y),
                                  info.deployed ? IM_COL32(140, 235, 160, 230) : IM_COL32(170, 170, 170, 180), buf);
            y += 15.0f;
        }

        // seat diagram: one cell per seat, role initial + seat number; occupied
        // cells highlighted, my seat marked with the faction color + "YOU".
        const int seatN = std::max(1, static_cast<int>(std::min<uint8_t>(info.seatCount, 8)));
        const float rowW = static_cast<float>(seatN) * kSeatBoxW + static_cast<float>(seatN - 1) * kSeatBoxGap;
        float bx = cx - rowW * 0.5f;
        bool anyFree = false;
        for (int i = 0; i < seatN; ++i)
        {
            const bool mine = (i == static_cast<int>(mySeat));
            const bool occupied = info.seats[i] != kInvalidPlayer;
            if (!occupied)
                anyFree = true;
            const ImVec2 a(bx, y), b(bx + kSeatBoxW, y + kSeatBoxH);
            const ImU32 bg = mine       ? TFUi::FactionCol(info.faction, 0.85f)
                             : occupied ? IM_COL32(90, 110, 140, 220)
                                        : IM_COL32(40, 44, 52, 180);
            fg->AddRectFilled(a, b, bg, 3.0f);
            fg->AddRect(a, b, mine ? IM_COL32(255, 255, 255, 230) : IM_COL32(160, 160, 160, 120), 3.0f, 0,
                        mine ? 2.0f : 1.0f);
            const char role = (def && static_cast<size_t>(i) < def->seats.size())
                                  ? RoleInitial(def->seats[static_cast<size_t>(i)].role)
                                  : 'S';
            std::snprintf(buf, sizeof(buf), "%d%c", i + 1, role);
            TFUi::AddTextCentered(fg, 12.0f, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f),
                                  occupied || mine ? IM_COL32(250, 250, 250, 240) : IM_COL32(150, 150, 150, 160), buf);
            if (mine)
                TFUi::AddTextCentered(fg, 10.0f, ImVec2((a.x + b.x) * 0.5f, b.y + 7.0f), IM_COL32(255, 255, 255, 200),
                                      "YOU");
            bx += kSeatBoxW + kSeatBoxGap;
        }
        y += kSeatBoxH + 16.0f;

        // key hints (swap keys only make sense with a free seat to move to)
        if (anyFree)
            std::snprintf(buf, sizeof(buf), "[1-%d] seat   [F] cycle   [E] exit", seatN);
        else
            std::snprintf(buf, sizeof(buf), "[E] exit");
        TFUi::AddTextCentered(fg, 12.0f, ImVec2(cx, y), IM_COL32(160, 165, 175, 180), buf);
    }

#else // !SPARK_HAS_IMGUI — headless builds keep the symbol, drop the pixels

    void TFVehicleHUD::Render(TFGameContext& ctx, TFVehicleSystem& vehicles)
    {
        (void)ctx;
        (void)vehicles;
    }

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront

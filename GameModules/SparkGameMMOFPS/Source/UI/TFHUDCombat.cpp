/**
 * @file TFHUDCombat.cpp
 * @brief TFHUD combat half (W6): killfeed with weapon + headshot marker and
 *        local-row highlight, death panel (killer/weapon/distance) + clickable
 *        DEPLOY/MAP actions, world-anchored damage pings with the octant-flash
 *        dedup, and the player-centered minimap v2 (same-faction deployables +
 *        friendly/squad pawn markers).
 *
 * Split from TFHUD.cpp per the repo file-size rule (TFPlayerSystemClient.cpp
 * pattern — same class, feature-owned translation units). TFHUD.cpp keeps the
 * lifecycle, feed-in setters, vitals/weapon/crosshair/compass/chat drawing.
 *
 * Data-visibility notes (what a pure client actually knows):
 *  - Pawns: replicated for every player (TFReplication RemotePawn store ->
 *    TFPlayerSystem client records), so ForEachAlivePawn/GetPawnBy* serve
 *    owner/faction/pos on all roles. Enemy pawns are deliberately NOT drawn
 *    on the minimap (friendly/squad intel only).
 *  - Deployables: TFDeployableSystem's client mirror (0x54FC-0x54FE).
 *  - Squad membership: TFSquadSystem::SquadOf only resolves the LOCAL player
 *    on pure clients (the roster mirror is private), so squadmates render as
 *    plain friendlies there; on authority roles the full registry resolves.
 *    A public roster accessor is requested in the wave wiring notes.
 */
#include "UI/TFHUD.h"

#include "Data/TFDataTables.h"
#include "Game/TFDeployableSystem.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFSquadSystem.h"
#include "Game/TFVehicleSystem.h"
#include "Camera/SparkEngineCamera.h"
#include "Net/TFClientNet.h"
#include "Net/TFNetProtocol.h"
#include "UI/TFMapScreen.h"
#include "UI/TFSpawnScreen.h"
#include "World/TFRegionSystem.h"
#include "World/TFWorldSetup.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Terrafront
{

    namespace
    {

        constexpr float kKillfeedTTL = 8.0f; // seconds, fade over last 2
        constexpr float kKillfeedFadeSec = 2.0f;
        constexpr size_t kKillfeedMax = 6;
        constexpr float kPendingKillTTL = 0.30f;    // EvPlayerKilled <-> PushKillfeed pairing window
        constexpr float kOctantSuppressSec = 0.05f; // world ping already covered this hit
        constexpr float kPingMergeDot = 0.94f;      // ~20 deg: refresh instead of new ping
        constexpr float kMinimapSpanM = 340.0f;     // player-centered minimap world span
        constexpr float kDeployDebounceSec = 1.0f;

        /// Local mirror of the TFClientNetHandlers PlayerLabel convention — keeps the
        /// death panel consistent with the names TFClientNet feeds the killfeed.
        void HudPlayerLabel(PlayerId id, PlayerId localId, char out[16])
        {
            if (id == kInvalidPlayer)
                std::snprintf(out, 16, "-");
            else if (id == localId)
                std::snprintf(out, 16, "YOU");
            else if (id == kTFLocalHostPlayer)
                std::snprintf(out, 16, "HOST");
            else
                std::snprintf(out, 16, "p%u", id);
        }

    } // namespace

    // ---------------------------------------------------------------------------
    // Killfeed feed-in (frozen 5-arg surface + W6 extended overload)
    // ---------------------------------------------------------------------------

    void TFHUD::PushKillfeed(const char* killer, const char* weapon, const char* victim, FactionId killerF,
                             FactionId victimF)
    {
        // Legacy call sites (TFClientNet) carry no ids/headshot; the extended
        // overload pairs the entry with a pending EvPlayerKilled when possible.
        PushKillfeed(killer, weapon, victim, killerF, victimF, false, kInvalidPlayer, kInvalidPlayer);
    }

    void TFHUD::PushKillfeed(const char* killer, const char* weapon, const char* victim, FactionId killerF,
                             FactionId victimF, bool headshot, PlayerId killerPlayer, PlayerId victimPlayer)
    {
        const PlayerId local = m_ctx ? m_ctx->localPlayer : kInvalidPlayer;

        KillfeedEntry e;
        e.killer = killer ? killer : "?";
        e.weapon = weapon ? weapon : "?";
        e.victim = victim ? victim : "?";
        e.killerF = killerF;
        e.victimF = victimF;
        e.headshot = headshot;
        e.annotated = (killerPlayer != kInvalidPlayer || victimPlayer != kInvalidPlayer);
        e.ttl = kKillfeedTTL;

        e.involvesLocal = (local != kInvalidPlayer && (killerPlayer == local || victimPlayer == local));
        // Name-convention fallback for un-upgraded 5-arg call sites (the labels
        // TFClientNet builds render the local player as "YOU").
        if (!e.involvesLocal)
            e.involvesLocal = (e.killer == "YOU" || e.victim == "YOU");

        // EvPlayerKilled fired BEFORE this push (bus subscriber order is not
        // guaranteed on authority roles): adopt the stashed details.
        if (!e.annotated && m_pendingKill.valid && m_pendingKill.ttl > 0.0f)
        {
            e.headshot = m_pendingKill.headshot;
            e.annotated = true;
            if (local != kInvalidPlayer && (m_pendingKill.killer == local || m_pendingKill.victim == local))
                e.involvesLocal = true;
            if (victimPlayer == kInvalidPlayer)
                victimPlayer = m_pendingKill.victim;
            if (killerPlayer == kInvalidPlayer)
                killerPlayer = m_pendingKill.killer;
            m_pendingKill = PendingKill{};
        }

        // Death panel capture for pure clients (no EvPlayerKilled bus there):
        // ids when the call site was upgraded, "YOU" label as the fallback.
        const bool localDied = (local != kInvalidPlayer && victimPlayer == local) || e.victim == "YOU";
        if (localDied)
        {
            m_death.valid = true;
            m_death.headshot = e.headshot;
            m_death.killerName = e.killer;
            m_death.weaponName = e.weapon;
            m_death.distanceM = -1.0f;
            float kp[3];
            if (killerPlayer != kInvalidPlayer && ResolvePlayerPosition(killerPlayer, kp))
            {
                const float dx = kp[0] - m_lastAlivePos[0];
                const float dy = kp[1] - m_lastAlivePos[1];
                const float dz = kp[2] - m_lastAlivePos[2];
                m_death.distanceM = std::sqrt(dx * dx + dy * dy + dz * dz);
            }
        }

        m_killfeed.push_front(std::move(e));
        while (m_killfeed.size() > kKillfeedMax)
            m_killfeed.pop_back();
    }

    // ---------------------------------------------------------------------------
    // Bus handlers (authority roles; pure clients never see these events fire
    // with full ids — see the pure-client capture path in PushKillfeed above)
    // ---------------------------------------------------------------------------

    void TFHUD::OnPlayerKilledBus(const EvPlayerKilled& ev)
    {
        const PlayerId local = m_ctx ? m_ctx->localPlayer : kInvalidPlayer;

        if (local != kInvalidPlayer && ev.victim == local)
            RecordDeath(ev.killer, ev.weapon, ev.headshot);

        // Annotate the matching killfeed entry if PushKillfeed already ran for
        // this kill; otherwise stash the details for the imminent push.
        for (KillfeedEntry& e : m_killfeed)
        {
            if (e.annotated || e.ttl < kKillfeedTTL - kPendingKillTTL)
                continue; // older entry — belongs to a previous kill
            e.headshot = ev.headshot;
            e.annotated = true;
            if (local != kInvalidPlayer && (ev.killer == local || ev.victim == local))
                e.involvesLocal = true;
            return;
        }
        m_pendingKill.valid = true;
        m_pendingKill.headshot = ev.headshot;
        m_pendingKill.killer = ev.killer;
        m_pendingKill.victim = ev.victim;
        m_pendingKill.ttl = kPendingKillTTL;
    }

    void TFHUD::OnPlayerDamagedBus(const EvPlayerDamaged& ev)
    {
        if (!m_ctx || !m_ctx->players)
            return;
        PlayerId local = m_ctx->localPlayer;
        if (local == kInvalidPlayer && m_ctx->clientNet)
            local = m_ctx->clientNet->LocalPlayerId();
        if (local == kInvalidPlayer)
            return;

        PawnInfo me{};
        if (!m_ctx->players->GetPawnByPlayer(local, me))
            return;
        if (ev.victim != me.entity || ev.attacker == 0 || ev.attacker == me.entity)
            return; // not our hit, environmental, or self-inflicted

        float attackerPos[3];
        if (ResolveEntityPosition(ev.attacker, attackerPos))
            AddWorldPing(attackerPos, me.pos);
        // Unresolvable attacker (already despawned): the octant flash from
        // ShowDamageFrom stays as the fallback indicator.
    }

    void TFHUD::RecordDeath(PlayerId killer, WeaponId weapon, bool headshot)
    {
        const PlayerId local = m_ctx ? m_ctx->localPlayer : kInvalidPlayer;

        m_death.valid = true;
        m_death.headshot = headshot;

        char name[16];
        HudPlayerLabel(killer, local, name);
        m_death.killerName = name;

        m_death.weaponName = "-";
        if (m_ctx && m_ctx->data && m_ctx->data->IsLoaded())
        {
            if (const WeaponDef* wd = m_ctx->data->GetWeapon(weapon))
                if (!wd->name.empty())
                    m_death.weaponName = wd->name;
        }

        m_death.distanceM = -1.0f;
        float kp[3];
        if (ResolvePlayerPosition(killer, kp))
        {
            const float dx = kp[0] - m_lastAlivePos[0];
            const float dy = kp[1] - m_lastAlivePos[1];
            const float dz = kp[2] - m_lastAlivePos[2];
            m_death.distanceM = std::sqrt(dx * dx + dy * dy + dz * dz);
        }
    }

    // ---------------------------------------------------------------------------
    // World-anchored damage pings
    // ---------------------------------------------------------------------------

    void TFHUD::AddWorldPing(const float attackerPos[3], const float myPos[3])
    {
        float dx = attackerPos[0] - myPos[0];
        float dz = attackerPos[2] - myPos[2];
        const float len = std::sqrt(dx * dx + dz * dz);
        if (len < 0.5f)
            return; // point blank / same spot: direction is meaningless
        dx /= len;
        dz /= len;

        // Same-hit dedup vs the octant flash: ShowDamageFrom may run before or
        // after this handler (synchronous bus, unspecified subscriber order).
        m_octantSuppress = kOctantSuppressSec; // flash arriving after us
        if (m_lastOctant >= 0 && m_lastOctantAge <= kOctantSuppressSec)
        {
            m_octant[m_lastOctant] = 0.0f; // flash arrived before us
            m_lastOctant = -1;
        }

        // Sustained fire from one direction refreshes instead of stacking.
        for (DamagePing& p : m_pings)
        {
            if (p.intensity > 0.0f && p.dirX * dx + p.dirZ * dz >= kPingMergeDot)
            {
                p.dirX = dx;
                p.dirZ = dz;
                p.intensity = 1.0f;
                return;
            }
        }
        DamagePing* slot = &m_pings[0];
        for (DamagePing& p : m_pings)
            if (p.intensity < slot->intensity)
                slot = &p;
        slot->dirX = dx;
        slot->dirZ = dz;
        slot->intensity = 1.0f;
    }

    // ---------------------------------------------------------------------------
    // Position lookups (net entity / player id -> world position)
    // ---------------------------------------------------------------------------

    bool TFHUD::ResolvePlayerPosition(PlayerId player, float out[3]) const
    {
        if (!m_ctx || !m_ctx->players || player == kInvalidPlayer)
            return false;
        PawnInfo p{};
        if (!m_ctx->players->GetPawnByPlayer(player, p))
            return false;
        out[0] = p.pos[0];
        out[1] = p.pos[1];
        out[2] = p.pos[2];
        return true;
    }

    bool TFHUD::ResolveEntityPosition(EntityId entity, float out[3]) const
    {
        if (!m_ctx || entity == 0)
            return false;
        if (m_ctx->players)
        {
            PawnInfo p{};
            if (m_ctx->players->GetPawnByEntity(entity, p))
            {
                out[0] = p.pos[0];
                out[1] = p.pos[1];
                out[2] = p.pos[2];
                return true;
            }
        }
        if (m_ctx->deployables)
        {
            TFDeployableView d{};
            if (m_ctx->deployables->GetDeployable(entity, d))
            {
                out[0] = d.pos[0];
                out[1] = d.pos[1];
                out[2] = d.pos[2];
                return true;
            }
        }
        if (m_ctx->vehicles)
        {
            TFVehicleInfo v{};
            if (m_ctx->vehicles->GetVehicleInfo(entity, v))
            {
                out[0] = v.pos[0];
                out[1] = v.pos[1];
                out[2] = v.pos[2];
                return true;
            }
        }
        return false;
    }

    // ---------------------------------------------------------------------------
    // Drawing
    // ---------------------------------------------------------------------------

#ifdef SPARK_HAS_IMGUI

    namespace
    {

        ImU32 CombatFactionCol(FactionId f, float alpha = 1.0f)
        {
            float c[4];
            FactionColor(f, c);
            return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], alpha));
        }

        void CombatTextCentered(ImDrawList* dl, float fontSize, ImVec2 center, ImU32 col, const char* text)
        {
            ImVec2 sz = ImGui::GetFont()->CalcTextSizeA(fontSize, 1e9f, 0.0f, text);
            dl->AddText(ImGui::GetFont(), fontSize, ImVec2(center.x - sz.x * 0.5f, center.y - sz.y * 0.5f), col, text);
        }

    } // namespace

    void TFHUD::DrawKillfeed()
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiViewport* vp = ImGui::GetMainViewport();
        float y = vp->Pos.y + 18.0f;
        const float right = vp->Pos.x + vp->Size.x - 20.0f;

        for (const KillfeedEntry& e : m_killfeed)
        {
            const float a = std::clamp(e.ttl / kKillfeedFadeSec, 0.0f, 1.0f);
            const std::string mid = "  [" + e.weapon + "]";
            const char* hs = e.headshot ? " HS " : "  ";

            const ImVec2 wK = ImGui::CalcTextSize(e.killer.c_str());
            const ImVec2 wM = ImGui::CalcTextSize(mid.c_str());
            const ImVec2 wH = ImGui::CalcTextSize(hs);
            const ImVec2 wV = ImGui::CalcTextSize(e.victim.c_str());

            float x = right - (wK.x + wM.x + wH.x + wV.x);
            if (e.involvesLocal)
            {
                // Subtle plate behind rows the local player is part of.
                dl->AddRectFilled(ImVec2(x - 6.0f, y - 2.0f), ImVec2(right + 6.0f, y + wK.y + 2.0f),
                                  ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, 0.12f * a)), 3.0f);
            }
            dl->AddText(ImVec2(x, y), CombatFactionCol(e.killerF, a), e.killer.c_str());
            x += wK.x;
            dl->AddText(ImVec2(x, y), ImGui::ColorConvertFloat4ToU32(ImVec4(0.75f, 0.75f, 0.75f, a)), mid.c_str());
            x += wM.x;
            dl->AddText(ImVec2(x, y), ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.35f, 0.25f, a)), hs);
            x += wH.x;
            dl->AddText(ImVec2(x, y), CombatFactionCol(e.victimF, a), e.victim.c_str());
            y += wK.y + 4.0f;
        }
    }

    void TFHUD::DrawDamageOctants()
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiViewport* vp = ImGui::GetMainViewport();
        const ImVec2 c(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f);
        constexpr float kPi = 3.14159265f;

        // Octant flashes (fallback when no attacker position was known).
        const float octRadius = 88.0f;
        for (int i = 0; i < 8; ++i)
        {
            if (m_octant[i] <= 0.0f)
                continue;
            const float a = std::clamp(m_octant[i], 0.0f, 1.0f);
            // Octant 0 = damage from ahead (screen up), increasing clockwise.
            const float center = -kPi * 0.5f + static_cast<float>(i) * (kPi * 0.25f);
            const float half = kPi * 0.125f * 0.8f;
            dl->PathArcTo(c, octRadius, center - half, center + half, 12);
            dl->PathStroke(ImGui::ColorConvertFloat4ToU32(ImVec4(0.95f, 0.15f, 0.12f, a * 0.9f)), 0, 6.0f);
        }

        // World-anchored pings rotate with the camera (direction stored world XZ).
        SparkEngineCamera* cam = (m_ctx && m_ctx->world) ? m_ctx->world->GetCamera() : nullptr;
        if (!cam)
            return;
        const float camYaw = cam->GetRotation().y; // radians, 0 faces +Z (north)
        const float pingRadius = 106.0f;
        for (const DamagePing& p : m_pings)
        {
            if (p.intensity <= 0.0f)
                continue;
            const float a = std::clamp(p.intensity, 0.0f, 1.0f);
            // World bearing (0 = +Z, clockwise) relative to facing; screen up = ahead.
            const float rel = std::atan2(p.dirX, p.dirZ) - camYaw;
            const float center = -kPi * 0.5f + rel;
            const float half = 0.26f + 0.10f * a;
            dl->PathArcTo(c, pingRadius, center - half, center + half, 14);
            dl->PathStroke(ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.42f, 0.10f, a)), 0, 7.0f);
            // Direction tip so the arc reads as an arrow, not a ring segment.
            const float tipX = c.x + std::cos(center) * (pingRadius + 9.0f);
            const float tipY = c.y + std::sin(center) * (pingRadius + 9.0f);
            dl->AddCircleFilled(ImVec2(tipX, tipY), 3.0f, ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.55f, 0.2f, a)));
        }
    }

    void TFHUD::DrawRespawnOverlay()
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiViewport* vp = ImGui::GetMainViewport();
        const ImVec2 c(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f);

        dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), IM_COL32(0, 0, 0, 140));

        CombatTextCentered(dl, 40.0f, ImVec2(c.x, c.y - 80.0f), IM_COL32(235, 60, 55, 235), "YOU ARE DOWN");

        char timer[32];
        std::snprintf(timer, sizeof(timer), "%.1f", m_respawnLeft);
        CombatTextCentered(dl, 64.0f, ImVec2(c.x, c.y - 10.0f), IM_COL32(240, 240, 240, 240), timer);

        // Death summary: who / weapon / distance (+ headshot marker).
        if (m_death.valid)
        {
            char line[192];
            if (m_death.distanceM >= 0.0f)
                std::snprintf(line, sizeof(line), "Killed by %s   [%s]   %.0f m%s", m_death.killerName.c_str(),
                              m_death.weaponName.c_str(), m_death.distanceM, m_death.headshot ? "   HEADSHOT" : "");
            else
                std::snprintf(line, sizeof(line), "Killed by %s   [%s]%s", m_death.killerName.c_str(),
                              m_death.weaponName.c_str(), m_death.headshot ? "   HEADSHOT" : "");
            CombatTextCentered(dl, 18.0f, ImVec2(c.x, c.y + 34.0f),
                               m_death.headshot ? IM_COL32(250, 120, 100, 235) : IM_COL32(215, 215, 220, 225), line);
        }

        if (m_deployCooldown > 0.0f)
        {
            CombatTextCentered(dl, 22.0f, ImVec2(c.x, c.y + 66.0f), IM_COL32(160, 220, 160, 230),
                               "DEPLOY REQUESTED...");
        }
        else
        {
            const bool ready = m_respawnLeft <= 0.0f;
            const float pulse = ready ? 0.65f + 0.35f * std::sin(static_cast<float>(ImGui::GetTime()) * 4.0f) : 0.45f;
            CombatTextCentered(dl, 22.0f, ImVec2(c.x, c.y + 66.0f),
                               ImGui::ColorConvertFloat4ToU32(ImVec4(0.95f, 0.95f, 0.95f, pulse)),
                               "PRESS SPACE TO DEPLOY AT SKYANCHOR");
        }

        // SPACE -> skyanchor spawn request (server validates the respawn timer).
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && m_deployCooldown <= 0.0f && m_ctx->clientNet &&
            m_ctx->clientNet->IsConnected())
        {
            TF_SpawnRequest rq{};
            rq.classId = static_cast<uint8_t>(m_lastClass);
            rq.spawnKind = 0; // skyanchor
            m_ctx->clientNet->SendMsg(TFMsg::SpawnRequest, &rq, sizeof(rq));
            m_deployCooldown = kDeployDebounceSec;
        }
    }

    void TFHUD::DrawDeathActions()
    {
        // Small input-receiving window (the main HUD overlay is NoInputs) — safe
        // to click since the engine gates gameplay mouse capture on ImGui's
        // WantCaptureMouse, and TFClientNet frees the cursor while dead.
        if (!m_ctx)
            return;
        if (m_ctx->map && m_ctx->map->IsOpen())
            return;
        if (m_ctx->spawnUI && m_ctx->spawnUI->IsOpen())
            return;

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f + 96.0f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.0f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_AlwaysAutoResize;
        if (!ImGui::Begin("##TFDeathActions", nullptr, flags))
        {
            ImGui::End();
            return;
        }

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.42f, 0.20f, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.56f, 0.26f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.24f, 0.68f, 0.30f, 1.0f));
        if (ImGui::Button("DEPLOY SCREEN", ImVec2(150.0f, 34.0f)) && m_ctx->spawnUI)
            m_ctx->spawnUI->Open();
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        if (ImGui::Button("MAP", ImVec2(90.0f, 34.0f)) && m_ctx->map)
            m_ctx->map->Open();

        ImGui::End();
    }

    void TFHUD::DrawMinimap()
    {
        if (!m_ctx || !m_ctx->data || !m_ctx->data->IsLoaded() || !m_view.valid)
            return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiViewport* vp = ImGui::GetMainViewport();

        const float mapSz = 168.0f, pad = 16.0f;
        const ImVec2 tl(vp->Pos.x + vp->Size.x - mapSz - pad, vp->Pos.y + pad);
        const ImVec2 br(tl.x + mapSz, tl.y + mapSz);
        const ImVec2 mid(tl.x + mapSz * 0.5f, tl.y + mapSz * 0.5f);
        dl->AddRectFilled(tl, br, IM_COL32(8, 10, 14, 150), 6.0f);

        // Player-centered zoomed view, north (+Z) up, world frame (no rotation —
        // the facing wedge shows heading; matches the compass strip convention).
        const float cx = m_view.pos[0];
        const float cz = m_view.pos[2];
        const float half = kMinimapSpanM * 0.5f;
        const float scale = mapSz / kMinimapSpanM;
        const auto toMini = [&](float wx, float wz) -> ImVec2
        { return ImVec2(mid.x + (wx - cx) * scale, mid.y - (wz - cz) * scale); };

        dl->PushClipRect(tl, br, true);

        // Region centers in range (owner-colored, sized by tier).
        TFRegionSystem* rs = m_ctx->regions;
        for (const RegionDef& r : m_ctx->data->GetContinent().regions)
        {
            if (std::fabs(r.centerX - cx) > half + 30.0f || std::fabs(r.centerZ - cz) > half + 30.0f)
                continue;
            const FactionId owner = rs ? rs->OwnerOf(r.id) : r.homeFaction;
            const ImVec2 p = toMini(r.centerX, r.centerZ);
            const float rad = (r.tier == "skyanchor") ? 6.0f : (r.tier == "facility" ? 5.0f : 4.0f);
            dl->AddCircle(p, rad, CombatFactionCol(owner, 0.9f), 0, 2.0f);
        }

        // Same-faction deployables (client mirror on pure clients).
        const FactionId myFaction = m_ctx->localFaction;
        if (m_ctx->deployables)
        {
            m_ctx->deployables->ForEachDeployable(
                [&](const TFDeployableView& d)
                {
                    if (d.faction != myFaction)
                        return;
                    if (std::fabs(d.pos[0] - cx) > half || std::fabs(d.pos[2] - cz) > half)
                        return;
                    const ImVec2 p = toMini(d.pos[0], d.pos[2]);
                    dl->AddRectFilled(ImVec2(p.x - 2.5f, p.y - 2.5f), ImVec2(p.x + 2.5f, p.y + 2.5f),
                                      CombatFactionCol(myFaction, 0.95f), 1.0f);
                    dl->AddRect(ImVec2(p.x - 2.5f, p.y - 2.5f), ImVec2(p.x + 2.5f, p.y + 2.5f),
                                IM_COL32(240, 240, 240, 160), 1.0f);
                });
        }

        // Friendly pawns; squadmates pop bright green (IsLocalSquadMember resolves
        // other players on pure clients through the replicated mirror roster).
        if (m_ctx->players && myFaction != FactionId::None)
        {
            const PlayerId local = m_ctx->localPlayer;
            m_ctx->players->ForEachAlivePawn(
                [&](const PawnInfo& p)
                {
                    if (p.owner == local || p.faction != myFaction)
                        return;
                    if (std::fabs(p.pos[0] - cx) > half || std::fabs(p.pos[2] - cz) > half)
                        return;
                    const ImVec2 mp = toMini(p.pos[0], p.pos[2]);
                    const bool squadmate = m_ctx->squads && m_ctx->squads->IsLocalSquadMember(p.owner);
                    if (squadmate)
                    {
                        dl->AddCircleFilled(mp, 3.5f, IM_COL32(110, 235, 140, 240));
                        dl->AddCircle(mp, 3.5f, IM_COL32(10, 40, 15, 200), 0, 1.0f);
                    }
                    else
                    {
                        dl->AddCircleFilled(mp, 2.5f, CombatFactionCol(myFaction, 0.9f));
                    }
                });
        }

        // Local player: facing wedge from the camera heading.
        SparkEngineCamera* cam = m_ctx->world ? m_ctx->world->GetCamera() : nullptr;
        const float yaw = cam ? cam->GetRotation().y : 0.0f;
        const ImVec2 tip(mid.x + std::sin(yaw) * 9.0f, mid.y - std::cos(yaw) * 9.0f);
        dl->AddLine(mid, tip, IM_COL32(255, 255, 255, 230), 2.0f);
        dl->AddCircleFilled(mid, 3.0f, IM_COL32(255, 255, 255, 240));

        dl->PopClipRect();

        dl->AddRect(tl, br, IM_COL32(90, 100, 110, 180), 6.0f);
        dl->AddText(ImVec2(mid.x - 4.0f, tl.y + 3.0f), IM_COL32(220, 225, 230, 200), "N");
    }

#else // !SPARK_HAS_IMGUI — headless: combat HUD is state-only

    void TFHUD::DrawKillfeed() {}
    void TFHUD::DrawDamageOctants() {}
    void TFHUD::DrawRespawnOverlay() {}
    void TFHUD::DrawDeathActions() {}
    void TFHUD::DrawMinimap() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront

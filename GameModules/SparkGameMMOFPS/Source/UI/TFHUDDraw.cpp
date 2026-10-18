/**
 * @file TFHUDDraw.cpp
 * @brief TFHUD overlay drawing: the borderless full-viewport HUD window
 *        (RenderUI), vitals, weapon/ammo box, crosshair + hitmarker, capture
 *        bar, compass, and the class-ability slot.
 *
 * Split from TFHUD.cpp per the repo file-size rule (TFPlayerSystemClient
 * pattern — same class, feature-owned translation units). TFHUD.cpp keeps the
 * lifecycle, feed-in setters and pawn-state gather; the chat window lives in
 * TFHUDChat.cpp and the W6 combat half (killfeed, damage pings, death panel +
 * actions, minimap v2) in TFHUDCombat.cpp (state) + TFHUDCombatDraw.cpp
 * (drawing). Shared internals (kHitmarkerDuration, FactionCol) live in
 * TFHUDInternal.h.
 */
#include "UI/TFHUD.h"

#include "Data/TFDataTables.h"
#include "Game/TFAbilitySystem.h" // class-abilities lane (W9): HUD ability slot
#include "Game/TFGrenadeSystem.h" // grenades lane (W10): HUD frag count
#include "Game/TFVehicleSystem.h" // W10 vehicle-hud: seated crosshair suppression
#include "World/TFWorldSetup.h"
#include "Camera/SparkEngineCamera.h"
#include "UI/TFHUDInternal.h"
#include "UI/TFKeybinds.h"
#include "UI/TFMapScreen.h"
#include "Input/InputManager.h"
#include "Spark/IEngineContext.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Terrafront
{

    using namespace HudDetail; // TFHUDInternal.h: FactionCol + kHitmarkerDuration shared with the other parts

#ifdef SPARK_HAS_IMGUI

    namespace
    {

        void DrawBar(ImDrawList* dl, ImVec2 pos, ImVec2 size, float frac, ImU32 fill)
        {
            frac = std::clamp(frac, 0.0f, 1.0f);
            const float rounding = TFUi::Px(2.0f);
            dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(10, 10, 12, 170), rounding);
            if (frac > 0.0f)
                dl->AddRectFilled(pos, ImVec2(pos.x + size.x * frac, pos.y + size.y), fill, rounding);
            dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(220, 220, 220, 90), rounding);
        }

        void AddTextCentered(ImDrawList* dl, float fontSize, ImVec2 center, ImU32 col, const char* text)
        {
            ImFont* font = Spark::Ui::Sdk::HudFont();
            ImVec2 sz = font->CalcTextSizeA(fontSize, 1e9f, 0.0f, text);
            dl->AddText(font, fontSize, ImVec2(center.x - sz.x * 0.5f, center.y - sz.y * 0.5f), col, text);
        }

        const char* ClassFallbackName(ClassId c)
        {
            switch (c)
            {
            case ClassId::Ghost:
                return "Ghost";
            case ClassId::Striker:
                return "Striker";
            case ClassId::Medtech:
                return "Medtech";
            case ClassId::Fabricator:
                return "Fabricator";
            case ClassId::Bulwark:
                return "Bulwark";
            case ClassId::Colossus:
                return "Colossus";
            default:
                return "Unknown";
            }
        }

    } // namespace

    namespace
    {
        // class-abilities lane (W9): ability icon + cooldown/fuel radial next
        // to the weapon box. Self-contained: reads GetLocalHudView per frame.
        void DrawAbilitySlot(const TFGameContext* ctx)
        {
            if (!ctx || !ctx->abilities)
                return;
            const TFAbilitySystem::HudView v = ctx->abilities->GetLocalHudView();
            if (!v.valid)
                return;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImGuiViewport* vp = ImGui::GetMainViewport();
            const float scale = TFUi::UiScale();
            const ImVec2 c(vp->Pos.x + vp->Size.x * 0.5f + 190.0f * scale, vp->Pos.y + vp->Size.y - 92.0f * scale);
            const float r = 22.0f * scale;
            ImU32 ring = IM_COL32(140, 205, 255, 235); // ready / fuel
            if (v.phase == TFAbilityPhase::Active)
                ring = IM_COL32(120, 255, 160, 240);
            else if (v.phase == TFAbilityPhase::Cooldown)
                ring = IM_COL32(255, 190, 80, 220);
            dl->AddCircleFilled(c, r, IM_COL32(16, 20, 26, 200));
            const float frac = v.fraction01 < 0.0f ? 0.0f : (v.fraction01 > 1.0f ? 1.0f : v.fraction01);
            if (frac > 0.001f)
            {
                dl->PathArcTo(c, r - 3.0f * scale, -1.570796f, -1.570796f + 6.283185f * frac, 48);
                dl->PathStroke(ring, 0, 4.0f * scale);
            }
            // Text is centred on the slot from its measured size, so it stays centred at any font size.
            const char key[2] = {v.hotkey, '\0'};
            const ImVec2 keySize = ImGui::CalcTextSize(key);
            dl->AddText(ImVec2(c.x - keySize.x * 0.5f, c.y - keySize.y * 0.5f), IM_COL32(235, 235, 235, 255), key);
            const ImVec2 nameSize = ImGui::CalcTextSize(v.name.c_str());
            dl->AddText(ImVec2(c.x - nameSize.x * 0.5f, c.y - r - 4.0f * scale - nameSize.y),
                        IM_COL32(200, 210, 220, 210), v.name.c_str());
            if (v.phase == TFAbilityPhase::Cooldown && v.remainingSec > 0.05f)
            {
                char cd[16];
                std::snprintf(cd, sizeof(cd), "%.0fs", v.remainingSec);
                const ImVec2 cdSize = ImGui::CalcTextSize(cd);
                dl->AddText(ImVec2(c.x - cdSize.x * 0.5f, c.y + r + 4.0f * scale), IM_COL32(255, 190, 80, 255), cd);
            }
        }
    } // namespace

    void TFHUD::RenderUI()
    {
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer())
            return;

        GatherPawnView();

        // W7 ui-map-keys: the chat-focus key comes from the shared keybind table
        // (Action::FocusChat, default Enter) — the chat lane rebinds via
        // TFKeys::BindKey without touching HUD code. ImGui gating preserved.
        const ImGuiKey chatKey = TFKeys::ImGuiKeyFor(TFKeys::Action::FocusChat);
        // chat-social lane: when TFChatWindow is wired it owns chat entirely —
        // the HUD's built-in chat never opens and never renders (gate below).
        if (!m_ctx->chatWindow && !m_chatOpen && m_ctx->InWorld() && !ImGui::GetIO().WantTextInput &&
            !ImGui::GetIO().WantCaptureKeyboard && chatKey != ImGuiKey_None && ImGui::IsKeyPressed(chatKey, false))
        {
            m_chatOpen = true;
            m_focusChatInput = true;
            if (m_ctx->engine && m_ctx->engine->GetInput())
            {
                InputManager* input = m_ctx->engine->GetInput();
                m_chatReleasedMouse = input->IsMouseCaptured();
                if (m_chatReleasedMouse)
                    input->CaptureMouse(false);
            }
        }
        if (m_chatOpen && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            CloseChat();

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                       ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (ImGui::Begin("##TFHUD", nullptr, flags))
        {
            if (m_view.alive && !m_dead)
                DrawCrosshairAndHitmarker();
            DrawVitals();
            DrawWeaponBox();
            if (m_view.alive && !m_dead)
                DrawAbilitySlot(m_ctx); // class-abilities lane (W9)
            DrawKillfeed();
            DrawDamageOctants();
            DrawCaptureBar();
            if (!m_dead)
            {
                DrawCompass();
                DrawMinimap();
            }
            if (m_dead)
                DrawRespawnOverlay();

            // W7 ui-map-keys: redeploy countdown readout while the map is closed
            // (the map draws its own; this keeps the timer visible after Esc).
            if (!m_dead && m_ctx->map && m_ctx->map->RedeployPending() && !m_ctx->map->IsOpen())
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImGuiViewport* vp2 = ImGui::GetMainViewport();
                const char* regionName = "?";
                if (m_ctx->data && m_ctx->data->IsLoaded())
                {
                    if (const RegionDef* rd = m_ctx->data->GetRegion(m_ctx->map->RedeployTarget()))
                        regionName = rd->name.c_str();
                }
                char line[96];
                std::snprintf(line, sizeof(line), "REDEPLOYING TO %s IN %.1f s", regionName,
                              std::max(0.0f, m_ctx->map->RedeployCountdown()));
                AddTextCentered(dl, TFUi::Px(20.0f),
                                ImVec2(vp2->Pos.x + vp2->Size.x * 0.5f, vp2->Pos.y + vp2->Size.y * 0.22f),
                                IM_COL32(255, 200, 60, 230), line);
            }
        }
        ImGui::End();

        // Input-receiving windows (own Begin/End; the overlay above is NoInputs).
        if (m_dead)
            DrawDeathActions();
        if (!m_ctx->chatWindow) // chat-social lane: TFChatWindow supersedes DrawChat
            DrawChat();
    }

    void TFHUD::DrawCompass()
    {
        SparkEngineCamera* cam = (m_ctx && m_ctx->world) ? m_ctx->world->GetCamera() : nullptr;
        if (!cam)
            return;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiViewport* vp = ImGui::GetMainViewport();

        // Camera heading in degrees; yaw 0 faces +Z which we treat as North.
        const float headingDeg = cam->GetRotation().y * 57.2957795f;
        const float scale = TFUi::UiScale();
        const float stripW = 460.0f * scale;
        const float labelY = 9.0f * scale; // tick labels sit under the tick marks
        const float stripH = std::max(26.0f * scale, labelY + ImGui::GetTextLineHeight() + 2.0f * scale);
        const float cx = vp->Pos.x + vp->Size.x * 0.5f;
        const float top = vp->Pos.y + 12.0f * scale;
        const float halfFov = 60.0f; // degrees mapped across half the strip

        dl->AddRectFilled(ImVec2(cx - stripW * 0.5f, top), ImVec2(cx + stripW * 0.5f, top + stripH),
                          IM_COL32(10, 12, 16, 130), 4.0f * scale);

        struct Tick
        {
            float deg;
            const char* label;
        };
        static const Tick ticks[] = {
            {0, "N"}, {45, "NE"}, {90, "E"}, {135, "SE"}, {180, "S"}, {225, "SW"}, {270, "W"}, {315, "NW"},
        };
        for (const Tick& t : ticks)
        {
            float d = t.deg - headingDeg;
            while (d > 180.0f)
                d -= 360.0f;
            while (d < -180.0f)
                d += 360.0f;
            if (d < -halfFov || d > halfFov)
                continue;
            const float x = cx + (d / halfFov) * (stripW * 0.5f);
            const bool cardinal = (t.deg == 0 || t.deg == 90 || t.deg == 180 || t.deg == 270);
            const ImU32 col = cardinal ? IM_COL32(255, 235, 180, 240) : IM_COL32(190, 190, 200, 200);
            dl->AddLine(ImVec2(x, top + 2.0f * scale), ImVec2(x, top + 8.0f * scale), col, 1.5f * scale);
            const ImVec2 sz = ImGui::CalcTextSize(t.label);
            dl->AddText(ImVec2(x - sz.x * 0.5f, top + labelY), col, t.label);
        }
        // Center heading marker.
        dl->AddTriangleFilled(ImVec2(cx - 5.0f * scale, top), ImVec2(cx + 5.0f * scale, top),
                              ImVec2(cx, top + 6.0f * scale), IM_COL32(255, 255, 255, 230));
    }

    // DrawMinimap (player-centered v2) lives in TFHUDCombat.cpp.

    void TFHUD::DrawVitals()
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiViewport* vp = ImGui::GetMainViewport();
        const FactionId fac = m_ctx->localFaction;

        const float scale = TFUi::UiScale();
        const float barW = 260.0f * scale;
        const float healthH = 14.0f * scale;
        const float shieldH = 8.0f * scale;
        const ImVec2 base(vp->Pos.x + 24.0f * scale, vp->Pos.y + vp->Size.y - 30.0f * scale);

        // Health (faction-colored) with shield bar above it.
        DrawBar(dl, ImVec2(base.x, base.y), ImVec2(barW, healthH),
                m_view.maxHealth > 0 ? m_view.health / m_view.maxHealth : 0.0f, FactionCol(fac, 0.95f));
        DrawBar(dl, ImVec2(base.x, base.y - healthH), ImVec2(barW, shieldH),
                m_view.maxShield > 0 ? m_view.shield / m_view.maxShield : 0.0f, IM_COL32(120, 210, 255, 230));

        char txt[96];
        std::snprintf(txt, sizeof(txt), "%.0f / %.0f", std::max(0.0f, m_view.health), m_view.maxHealth);
        dl->AddText(ImVec2(base.x + barW + 8.0f * scale, base.y + (healthH - ImGui::GetTextLineHeight()) * 0.5f),
                    IM_COL32(235, 235, 235, 220), txt);

        // Class + rank + faction tag line above the bars.
        const char* clsName = ClassFallbackName(m_view.valid ? m_view.cls : m_lastClass);
        if (m_ctx->data && m_ctx->data->IsLoaded())
        {
            if (const ClassDef* cd = m_ctx->data->GetClass(m_view.valid ? m_view.cls : m_lastClass))
                if (!cd->name.empty())
                    clsName = cd->name.c_str();
        }
        std::snprintf(txt, sizeof(txt), "%s  |  Rank %u", clsName, static_cast<unsigned>(m_rank));
        // Above the shield bar with a small gap, measured from the font so the line never overlaps the bars.
        const float classLineY = base.y - healthH - 6.0f * scale - ImGui::GetTextLineHeight();
        dl->AddText(ImVec2(base.x, classLineY), IM_COL32(220, 220, 220, 220), txt);
        dl->AddText(ImVec2(base.x + ImGui::CalcTextSize(txt).x + 10.0f * scale, classLineY), FactionCol(fac),
                    FactionTag(fac));
    }

    void TFHUD::DrawWeaponBox()
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiViewport* vp = ImGui::GetMainViewport();
        const float scale = TFUi::UiScale();
        const ImVec2 corner(vp->Pos.x + vp->Size.x - 24.0f * scale, vp->Pos.y + vp->Size.y - 30.0f * scale);

        const char* name = m_weapName.empty() ? "NO WEAPON" : m_weapName.c_str();
        ImVec2 nameSz = ImGui::CalcTextSize(name);
        dl->AddText(ImVec2(corner.x - nameSz.x, corner.y - 52.0f * scale),
                    m_weapName.empty() ? IM_COL32(150, 150, 150, 180) : IM_COL32(230, 230, 230, 230), name);

        char ammo[48];
        if (m_weapMag >= 0)
            std::snprintf(ammo, sizeof(ammo), "%d | %d", m_weapMag, std::max(0, m_weapReserve));
        else
            std::snprintf(ammo, sizeof(ammo), "-- | --"); // not fed yet (SetWeaponStatus)
        const float ammoSize = 30.0f * scale;
        ImFont* ammoFont = Spark::Ui::Sdk::HudFontBold();
        ImVec2 ammoSz = ammoFont->CalcTextSizeA(ammoSize, 1e9f, 0.0f, ammo);
        dl->AddText(ammoFont, ammoSize, ImVec2(corner.x - ammoSz.x, corner.y - 34.0f * scale),
                    IM_COL32(240, 240, 240, 235), ammo);

        // grenades lane (W10): frag count above the weapon name in the ammo
        // cluster (authoritative via TF_GrenadeSpawn's `remaining` echo).
        if (m_ctx->grenades)
        {
            const int frags = m_ctx->grenades->GetLocalGrenadeCount();
            if (frags >= 0)
            {
                char gr[32];
                std::snprintf(gr, sizeof(gr), "FRAG x%d", frags);
                ImVec2 grSz = ImGui::CalcTextSize(gr);
                dl->AddText(ImVec2(corner.x - grSz.x, corner.y - 70.0f * scale),
                            frags > 0 ? IM_COL32(220, 220, 220, 220) : IM_COL32(150, 150, 150, 180), gr);
            }
        }

        if (m_weapReloading)
        {
            // Slow pulse so it reads as activity, not an error.
            const float pulse = 0.6f + 0.4f * std::sin(static_cast<float>(ImGui::GetTime()) * 8.0f);
            const char* rl = "RELOADING";
            ImVec2 rlSz = ImGui::CalcTextSize(rl);
            dl->AddText(ImVec2(corner.x - rlSz.x, corner.y + 2.0f * scale),
                        ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.75f, 0.25f, pulse)), rl);
        }
    }

    void TFHUD::DrawCrosshairAndHitmarker()
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiViewport* vp = ImGui::GetMainViewport();
        const ImVec2 c(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f);
        const float scale = TFUi::UiScale();

        // W10 vehicle-hud: seated players get their per-seat reticle from
        // TFVehicleHUD; keep the hitmarker, skip the infantry crosshair.
        const bool seated = m_ctx && m_ctx->vehicles && m_ctx->vehicles->IsSeated(m_ctx->localPlayer);
        if (!seated)
        {
            // Spread-reactive: widen with horizontal speed (cheap stand-in for weapon bloom).
            const float gap = (4.0f + std::clamp(m_view.speed * 1.4f, 0.0f, 18.0f)) * scale;
            const float len = 8.0f * scale;
            const float thickness = 2.0f * scale;
            const ImU32 col = IM_COL32(255, 255, 255, 210);
            dl->AddLine(ImVec2(c.x, c.y - gap - len), ImVec2(c.x, c.y - gap), col, thickness);
            dl->AddLine(ImVec2(c.x, c.y + gap), ImVec2(c.x, c.y + gap + len), col, thickness);
            dl->AddLine(ImVec2(c.x - gap - len, c.y), ImVec2(c.x - gap, c.y), col, thickness);
            dl->AddLine(ImVec2(c.x + gap, c.y), ImVec2(c.x + gap + len, c.y), col, thickness);
            dl->AddCircleFilled(c, 1.5f * scale, col);
        }

        if (m_hitTimer > 0.0f)
        {
            const float dur = m_hitKilled ? kHitmarkerDuration * 2.0f : kHitmarkerDuration;
            const float a = std::clamp(m_hitTimer / dur, 0.0f, 1.0f);
            const ImU32 hcol = m_hitKilled ? ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.25f, 0.2f, a))
                                           : ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, a));
            const float g = (m_hitKilled ? 7.0f : 5.0f) * scale;  // inner gap
            const float l = (m_hitKilled ? 12.0f : 9.0f) * scale; // stroke length
            const float d = 0.70710678f;                          // 45 degrees
            for (int sx = -1; sx <= 1; sx += 2)
                for (int sy = -1; sy <= 1; sy += 2)
                    dl->AddLine(ImVec2(c.x + sx * g * d, c.y + sy * g * d),
                                ImVec2(c.x + sx * (g + l) * d, c.y + sy * (g + l) * d), hcol, 2.5f * scale);
        }
    }

    // DrawKillfeed / DrawDamageOctants live in TFHUDCombat.cpp.

    void TFHUD::DrawCaptureBar()
    {
        if (!m_captureVisible || m_captureTTL <= 0.0f)
            return; // TF-W2: TFRegionSystem drives this via TF_CaptureTick

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiViewport* vp = ImGui::GetMainViewport();
        const float scale = TFUi::UiScale();
        const float w = 300.0f * scale;
        const ImVec2 pos(vp->Pos.x + (vp->Size.x - w) * 0.5f, vp->Pos.y + vp->Size.y * 0.80f);

        DrawBar(dl, pos, ImVec2(w, 10.0f * scale), m_captureProgress, FactionCol(m_captureFaction, 0.9f));
        char label[64];
        std::snprintf(label, sizeof(label), "Capturing - %s  %d%%", FactionName(m_captureFaction),
                      static_cast<int>(m_captureProgress * 100.0f));
        AddTextCentered(dl, ImGui::GetFontSize(),
                        ImVec2(pos.x + w * 0.5f, pos.y - 4.0f * scale - ImGui::GetFontSize() * 0.5f),
                        IM_COL32(235, 235, 235, 220), label);
    }

    // DrawRespawnOverlay / DrawDeathActions live in TFHUDCombat.cpp.

#else // !SPARK_HAS_IMGUI — headless / no ImGui: HUD is state-only.
    // (Combat-half stubs — killfeed/octants/respawn/death-actions/minimap —
    // live in TFHUDCombat.cpp's own #else branch; the DrawChat stub lives in
    // TFHUDChat.cpp's.)

    void TFHUD::RenderUI() {}
    void TFHUD::DrawVitals() {}
    void TFHUD::DrawWeaponBox() {}
    void TFHUD::DrawCrosshairAndHitmarker() {}
    void TFHUD::DrawCaptureBar() {}
    void TFHUD::DrawCompass() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront

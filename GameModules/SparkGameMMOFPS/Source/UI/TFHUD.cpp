/**
 * @file TFHUD.cpp
 * @brief ImGui HUD: vitals, weapon/ammo, crosshair + hitmarker, killfeed,
 *        damage direction octants, capture bar placeholder, respawn overlay.
 *
 * Rendered as one borderless, transparent, input-transparent overlay window
 * covering the main viewport. All state is fed either by frozen setters
 * (called from TFClientNet / TFWeaponSystem) or read from the frozen
 * TFPlayerSystem pawn API each frame.
 */
#include "UI/TFHUD.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Net/TFClientNet.h"
#include "Net/TFNetProtocol.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Terrafront {

namespace {

constexpr float kHitmarkerDuration   = 0.30f;   // seconds
constexpr float kKillfeedTTL         = 8.0f;    // seconds, fade over last 2
constexpr float kKillfeedFadeSec     = 2.0f;
constexpr size_t kKillfeedMax        = 6;
constexpr float kOctantDecayPerSec   = 1.6f;    // damage flash fade speed
constexpr float kCaptureBarTTL       = 3.0f;    // hide if no tick refreshes it
constexpr float kDeployDebounceSec   = 1.0f;

const char* ClassFallbackName(ClassId c)
{
    switch (c) {
        case ClassId::Ghost:      return "Ghost";
        case ClassId::Striker:    return "Striker";
        case ClassId::Medtech:    return "Medtech";
        case ClassId::Fabricator: return "Fabricator";
        case ClassId::Bulwark:    return "Bulwark";
        case ClassId::Colossus:   return "Colossus";
        default:                  return "Unknown";
    }
}

} // namespace

TFHUD::TFHUD() = default;
TFHUD::~TFHUD() { if (m_initialized) Shutdown(); }

bool TFHUD::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;

    // Belt-and-braces alongside the direct TFClientNet -> setter calls:
    // the frozen bus events also drive the respawn overlay and rank text.
    events.Subscribe<EvLocalPlayerDied>([this](const EvLocalPlayerDied& e) {
        if (m_ctx && e.player == m_ctx->localPlayer)
            SetRespawnState(true, e.respawnDelay);
    });
    events.Subscribe<EvPlayerSpawned>([this](const EvPlayerSpawned& e) {
        if (m_ctx && e.player == m_ctx->localPlayer)
            SetRespawnState(false, 0.0f);
    });
    events.Subscribe<EvRankUp>([this](const EvRankUp& e) {
        if (m_ctx && e.player == m_ctx->localPlayer)
            m_rank = e.newRank;
    });

    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFHUD initialized");
    return true;
}

void TFHUD::Update(float dt)
{
    if (!m_initialized)
        return;

    m_hitTimer = std::max(0.0f, m_hitTimer - dt);
    for (float& o : m_octant)
        o = std::max(0.0f, o - kOctantDecayPerSec * dt);

    for (auto& e : m_killfeed)
        e.ttl -= dt;
    while (!m_killfeed.empty() && m_killfeed.back().ttl <= 0.0f)
        m_killfeed.pop_back();

    m_captureTTL = std::max(0.0f, m_captureTTL - dt);
    m_deployCooldown = std::max(0.0f, m_deployCooldown - dt);
    if (m_dead)
        m_respawnLeft = std::max(0.0f, m_respawnLeft - dt);
}

void TFHUD::FixedUpdate(float fixedDeltaTime) { (void)fixedDeltaTime; }

void TFHUD::Shutdown()
{
    m_killfeed.clear();
    m_initialized = false;
}

void TFHUD::RenderDebugUI() {}  // HUD state is visible on screen; no debug panel needed W1

// ---------------------------------------------------------------------------
// Public feed-ins
// ---------------------------------------------------------------------------

void TFHUD::ShowHitmarker(bool killed)
{
    m_hitTimer  = killed ? kHitmarkerDuration * 2.0f : kHitmarkerDuration;
    m_hitKilled = killed;
}

void TFHUD::PushKillfeed(const char* killer, const char* weapon, const char* victim,
                         FactionId killerF, FactionId victimF)
{
    KillfeedEntry e;
    e.killer  = killer ? killer : "?";
    e.weapon  = weapon ? weapon : "?";
    e.victim  = victim ? victim : "?";
    e.killerF = killerF;
    e.victimF = victimF;
    e.ttl     = kKillfeedTTL;
    m_killfeed.push_front(std::move(e));
    while (m_killfeed.size() > kKillfeedMax)
        m_killfeed.pop_back();
}

void TFHUD::SetCaptureProgress(float progress01, FactionId capturing, bool visible)
{
    m_captureProgress = std::clamp(progress01, 0.0f, 1.0f);
    m_captureFaction  = capturing;
    m_captureVisible  = visible;
    m_captureTTL      = visible ? kCaptureBarTTL : 0.0f;
}

void TFHUD::SetRespawnState(bool dead, float secondsLeft)
{
    m_dead        = dead;
    m_respawnLeft = dead ? std::max(0.0f, secondsLeft) : 0.0f;
}

void TFHUD::ShowDamageFrom(uint8_t dirOctant)
{
    if (dirOctant < 8)
        m_octant[dirOctant] = 1.0f;
}

void TFHUD::SetWeaponStatus(const char* name, int mag, int reserve, bool reloading)
{
    m_weapName      = name ? name : "";
    m_weapMag       = mag;
    m_weapReserve   = reserve;
    m_weapReloading = reloading;
}

void TFHUD::SetRank(uint16_t rank) { m_rank = rank; }

// ---------------------------------------------------------------------------
// Pawn state gather (no ImGui)
// ---------------------------------------------------------------------------

void TFHUD::GatherPawnView()
{
    m_view = PawnView{};
    if (!m_ctx || !m_ctx->players)
        return;

    PlayerId pid = m_ctx->localPlayer;
    if (pid == kInvalidPlayer && m_ctx->clientNet)
        pid = m_ctx->clientNet->LocalPlayerId();
    if (pid == kInvalidPlayer)
        return;

    PawnInfo p{};
    if (!m_ctx->players->GetPawnByPlayer(pid, p))
        return;

    m_view.valid  = true;
    m_view.alive  = p.alive;
    m_view.health = p.health;
    m_view.shield = p.shield;
    m_view.speed  = std::sqrt(p.vel[0] * p.vel[0] + p.vel[2] * p.vel[2]);
    m_view.cls    = p.cls;
    m_lastClass   = p.cls;

    if (m_ctx->data && m_ctx->data->IsLoaded()) {
        if (const ClassDef* cd = m_ctx->data->GetClass(p.cls)) {
            m_view.maxHealth = cd->health;
            m_view.maxShield = cd->shield;
        }
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

#ifdef ENABLE_EDITOR

namespace {

ImU32 FactionCol(FactionId f, float alpha = 1.0f)
{
    float c[4];
    FactionColor(f, c);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], alpha));
}

void DrawBar(ImDrawList* dl, ImVec2 pos, ImVec2 size, float frac, ImU32 fill)
{
    frac = std::clamp(frac, 0.0f, 1.0f);
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                      IM_COL32(10, 10, 12, 170), 2.0f);
    if (frac > 0.0f)
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x * frac, pos.y + size.y), fill, 2.0f);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                IM_COL32(220, 220, 220, 90), 2.0f);
}

void AddTextCentered(ImDrawList* dl, float fontSize, ImVec2 center, ImU32 col, const char* text)
{
    ImVec2 sz = ImGui::GetFont()->CalcTextSizeA(fontSize, 1e9f, 0.0f, text);
    dl->AddText(ImGui::GetFont(), fontSize,
                ImVec2(center.x - sz.x * 0.5f, center.y - sz.y * 0.5f), col, text);
}

} // namespace

void TFHUD::RenderUI()
{
    if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer())
        return;

    GatherPawnView();

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (!ImGui::Begin("##TFHUD", nullptr, flags)) {
        ImGui::End();
        return;
    }

    if (m_view.alive && !m_dead)
        DrawCrosshairAndHitmarker();
    DrawVitals();
    DrawWeaponBox();
    DrawKillfeed();
    DrawDamageOctants();
    DrawCaptureBar();
    if (m_dead)
        DrawRespawnOverlay();

    ImGui::End();
}

void TFHUD::DrawVitals()
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const FactionId fac = m_ctx->localFaction;

    const float barW = 260.0f;
    const ImVec2 base(vp->Pos.x + 24.0f, vp->Pos.y + vp->Size.y - 30.0f);

    // Health (faction-colored) with shield bar above it.
    DrawBar(dl, ImVec2(base.x, base.y), ImVec2(barW, 14.0f),
            m_view.maxHealth > 0 ? m_view.health / m_view.maxHealth : 0.0f,
            FactionCol(fac, 0.95f));
    DrawBar(dl, ImVec2(base.x, base.y - 14.0f), ImVec2(barW, 8.0f),
            m_view.maxShield > 0 ? m_view.shield / m_view.maxShield : 0.0f,
            IM_COL32(120, 210, 255, 230));

    char txt[96];
    std::snprintf(txt, sizeof(txt), "%.0f / %.0f", std::max(0.0f, m_view.health), m_view.maxHealth);
    dl->AddText(ImVec2(base.x + barW + 8.0f, base.y - 1.0f), IM_COL32(235, 235, 235, 220), txt);

    // Class + rank + faction tag line above the bars.
    const char* clsName = ClassFallbackName(m_view.valid ? m_view.cls : m_lastClass);
    if (m_ctx->data && m_ctx->data->IsLoaded()) {
        if (const ClassDef* cd = m_ctx->data->GetClass(m_view.valid ? m_view.cls : m_lastClass))
            if (!cd->name.empty())
                clsName = cd->name.c_str();
    }
    std::snprintf(txt, sizeof(txt), "%s  |  Rank %u", clsName, static_cast<unsigned>(m_rank));
    dl->AddText(ImVec2(base.x, base.y - 36.0f), IM_COL32(220, 220, 220, 220), txt);
    dl->AddText(ImVec2(base.x + ImGui::CalcTextSize(txt).x + 10.0f, base.y - 36.0f),
                FactionCol(fac), FactionTag(fac));
}

void TFHUD::DrawWeaponBox()
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 corner(vp->Pos.x + vp->Size.x - 24.0f, vp->Pos.y + vp->Size.y - 30.0f);

    const char* name = m_weapName.empty() ? "NO WEAPON" : m_weapName.c_str();
    ImVec2 nameSz = ImGui::CalcTextSize(name);
    dl->AddText(ImVec2(corner.x - nameSz.x, corner.y - 52.0f),
                m_weapName.empty() ? IM_COL32(150, 150, 150, 180) : IM_COL32(230, 230, 230, 230),
                name);

    char ammo[48];
    if (m_weapMag >= 0)
        std::snprintf(ammo, sizeof(ammo), "%d | %d", m_weapMag, std::max(0, m_weapReserve));
    else
        std::snprintf(ammo, sizeof(ammo), "-- | --");   // not fed yet (SetWeaponStatus)
    const float ammoSize = 30.0f;
    ImVec2 ammoSz = ImGui::GetFont()->CalcTextSizeA(ammoSize, 1e9f, 0.0f, ammo);
    dl->AddText(ImGui::GetFont(), ammoSize,
                ImVec2(corner.x - ammoSz.x, corner.y - 34.0f),
                IM_COL32(240, 240, 240, 235), ammo);

    if (m_weapReloading) {
        // Slow pulse so it reads as activity, not an error.
        const float pulse = 0.6f + 0.4f * std::sin(static_cast<float>(ImGui::GetTime()) * 8.0f);
        const char* rl = "RELOADING";
        ImVec2 rlSz = ImGui::CalcTextSize(rl);
        dl->AddText(ImVec2(corner.x - rlSz.x, corner.y + 2.0f),
                    ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.75f, 0.25f, pulse)), rl);
    }
}

void TFHUD::DrawCrosshairAndHitmarker()
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 c(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f);

    // Spread-reactive: widen with horizontal speed (cheap stand-in for weapon bloom).
    const float gap = 4.0f + std::clamp(m_view.speed * 1.4f, 0.0f, 18.0f);
    const float len = 8.0f;
    const ImU32 col = IM_COL32(255, 255, 255, 210);
    dl->AddLine(ImVec2(c.x, c.y - gap - len), ImVec2(c.x, c.y - gap), col, 2.0f);
    dl->AddLine(ImVec2(c.x, c.y + gap), ImVec2(c.x, c.y + gap + len), col, 2.0f);
    dl->AddLine(ImVec2(c.x - gap - len, c.y), ImVec2(c.x - gap, c.y), col, 2.0f);
    dl->AddLine(ImVec2(c.x + gap, c.y), ImVec2(c.x + gap + len, c.y), col, 2.0f);
    dl->AddCircleFilled(c, 1.5f, col);

    if (m_hitTimer > 0.0f) {
        const float dur = m_hitKilled ? kHitmarkerDuration * 2.0f : kHitmarkerDuration;
        const float a = std::clamp(m_hitTimer / dur, 0.0f, 1.0f);
        const ImU32 hcol = m_hitKilled
            ? ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.25f, 0.2f, a))
            : ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, a));
        const float g = m_hitKilled ? 7.0f : 5.0f;   // inner gap
        const float l = m_hitKilled ? 12.0f : 9.0f;  // stroke length
        const float d = 0.70710678f;                 // 45 degrees
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2)
                dl->AddLine(ImVec2(c.x + sx * g * d,       c.y + sy * g * d),
                            ImVec2(c.x + sx * (g + l) * d, c.y + sy * (g + l) * d),
                            hcol, 2.5f);
    }
}

void TFHUD::DrawKillfeed()
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float y = vp->Pos.y + 18.0f;
    const float right = vp->Pos.x + vp->Size.x - 20.0f;

    for (const KillfeedEntry& e : m_killfeed) {
        const float a = std::clamp(e.ttl / kKillfeedFadeSec, 0.0f, 1.0f);
        std::string mid = "  [" + e.weapon + "]  ";
        const ImVec2 wK = ImGui::CalcTextSize(e.killer.c_str());
        const ImVec2 wM = ImGui::CalcTextSize(mid.c_str());
        const ImVec2 wV = ImGui::CalcTextSize(e.victim.c_str());

        float x = right - (wK.x + wM.x + wV.x);
        dl->AddText(ImVec2(x, y), FactionCol(e.killerF, a), e.killer.c_str());
        x += wK.x;
        dl->AddText(ImVec2(x, y),
                    ImGui::ColorConvertFloat4ToU32(ImVec4(0.75f, 0.75f, 0.75f, a)),
                    mid.c_str());
        x += wM.x;
        dl->AddText(ImVec2(x, y), FactionCol(e.victimF, a), e.victim.c_str());
        y += wK.y + 4.0f;
    }
}

void TFHUD::DrawDamageOctants()
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 c(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f);
    const float radius = 88.0f;
    constexpr float kPi = 3.14159265f;

    for (int i = 0; i < 8; ++i) {
        if (m_octant[i] <= 0.0f)
            continue;
        const float a = std::clamp(m_octant[i], 0.0f, 1.0f);
        // Octant 0 = damage from ahead (screen up), increasing clockwise.
        const float center = -kPi * 0.5f + static_cast<float>(i) * (kPi * 0.25f);
        const float half   = kPi * 0.125f * 0.8f;
        dl->PathArcTo(c, radius, center - half, center + half, 12);
        dl->PathStroke(ImGui::ColorConvertFloat4ToU32(ImVec4(0.95f, 0.15f, 0.12f, a * 0.9f)),
                       0, 6.0f);
    }
}

void TFHUD::DrawCaptureBar()
{
    if (!m_captureVisible || m_captureTTL <= 0.0f)
        return;   // TF-W2: TFRegionSystem drives this via TF_CaptureTick

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float w = 300.0f;
    const ImVec2 pos(vp->Pos.x + (vp->Size.x - w) * 0.5f, vp->Pos.y + vp->Size.y * 0.80f);

    DrawBar(dl, pos, ImVec2(w, 10.0f), m_captureProgress, FactionCol(m_captureFaction, 0.9f));
    char label[64];
    std::snprintf(label, sizeof(label), "Capturing - %s  %d%%",
                  FactionName(m_captureFaction), static_cast<int>(m_captureProgress * 100.0f));
    AddTextCentered(dl, ImGui::GetFontSize(), ImVec2(pos.x + w * 0.5f, pos.y - 12.0f),
                    IM_COL32(235, 235, 235, 220), label);
}

void TFHUD::DrawRespawnOverlay()
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 c(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f);

    dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                      IM_COL32(0, 0, 0, 140));

    AddTextCentered(dl, 40.0f, ImVec2(c.x, c.y - 80.0f),
                    IM_COL32(235, 60, 55, 235), "YOU ARE DOWN");

    char timer[32];
    std::snprintf(timer, sizeof(timer), "%.1f", m_respawnLeft);
    AddTextCentered(dl, 64.0f, ImVec2(c.x, c.y - 10.0f), IM_COL32(240, 240, 240, 240), timer);

    if (m_deployCooldown > 0.0f) {
        AddTextCentered(dl, 22.0f, ImVec2(c.x, c.y + 60.0f),
                        IM_COL32(160, 220, 160, 230), "DEPLOY REQUESTED...");
    } else {
        const bool ready = m_respawnLeft <= 0.0f;
        const float pulse = ready
            ? 0.65f + 0.35f * std::sin(static_cast<float>(ImGui::GetTime()) * 4.0f)
            : 0.45f;
        AddTextCentered(dl, 22.0f, ImVec2(c.x, c.y + 60.0f),
                        ImGui::ColorConvertFloat4ToU32(ImVec4(0.95f, 0.95f, 0.95f, pulse)),
                        "PRESS SPACE TO DEPLOY AT SKYANCHOR");
    }

    // SPACE -> skyanchor spawn request (server validates the respawn timer).
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && m_deployCooldown <= 0.0f &&
        m_ctx->clientNet && m_ctx->clientNet->IsConnected()) {
        TF_SpawnRequest rq{};
        rq.classId   = static_cast<uint8_t>(m_lastClass);
        rq.spawnKind = 0;   // skyanchor
        m_ctx->clientNet->SendMsg(TFMsg::SpawnRequest, &rq, sizeof(rq));
        m_deployCooldown = kDeployDebounceSec;
    }
}

#else // !ENABLE_EDITOR — headless / no ImGui: HUD is state-only

void TFHUD::RenderUI() {}
void TFHUD::DrawVitals() {}
void TFHUD::DrawWeaponBox() {}
void TFHUD::DrawCrosshairAndHitmarker() {}
void TFHUD::DrawKillfeed() {}
void TFHUD::DrawDamageOctants() {}
void TFHUD::DrawCaptureBar() {}
void TFHUD::DrawRespawnOverlay() {}

#endif // ENABLE_EDITOR

} // namespace Terrafront

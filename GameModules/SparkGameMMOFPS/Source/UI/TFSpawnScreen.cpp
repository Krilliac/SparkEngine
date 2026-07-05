/**
 * @file TFSpawnScreen.cpp
 * @brief W2 death/spawn screen: faction splash (first join), class picker
 *        (classes.json, Colossus greyed out), spawn point list (skyanchor +
 *        every CanSpawnAt region with distance from the death spot), DEPLOY
 *        with respawn-countdown gating, and HUD-overlay coordination.
 *
 * Consumes the W2 TFRegionSystem contract (CanSpawnAt) on both server and
 * client mirror. Sends TF_FactionSelect / TF_SpawnRequest through
 * TFClientNet::SendMsg (which loops back in-process on authority roles).
 */
#include "UI/TFSpawnScreen.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Net/TFClientNet.h"
#include "Net/TFNetProtocol.h"
#include "UI/TFHUD.h"
#include "UI/TFMapScreen.h"
#include "UI/TFUiCommon.h"
#include "World/TFRegionSystem.h"

#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Terrafront {

namespace {

constexpr float kOpenDelaySec       = 2.0f;  // death -> screen (HUD overlay window)
constexpr float kRequestDebounceSec = 1.0f;  // DEPLOY re-enable after a request

} // namespace

TFSpawnScreen::TFSpawnScreen() = default;
TFSpawnScreen::~TFSpawnScreen() { if (m_initialized) Shutdown(); }

bool TFSpawnScreen::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;

    events.Subscribe<EvLocalPlayerDied>([this](const EvLocalPlayerDied& e) {
        OnLocalPlayerDied(e);
    });
    // Authority roles fire EvPlayerSpawned on the shared bus; pure clients are
    // covered by the alive-poll in Update().
    events.Subscribe<EvPlayerSpawned>([this](const EvPlayerSpawned& e) {
        if (m_ctx && e.player == m_ctx->localPlayer) {
            m_pendingOpen = 0.0f;
            Close();
        }
    });

    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFSpawnScreen initialized");
    return true;
}

void TFSpawnScreen::FixedUpdate(float fixedDeltaTime) { (void)fixedDeltaTime; }

void TFSpawnScreen::Shutdown()
{
    m_open = false;
    m_initialized = false;
}

void TFSpawnScreen::RenderDebugUI() {}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

bool TFSpawnScreen::LocalPawnAlive() const
{
    if (!m_ctx || !m_ctx->players)
        return false;
    PlayerId pid = m_ctx->localPlayer;
    if (pid == kInvalidPlayer && m_ctx->clientNet)
        pid = m_ctx->clientNet->LocalPlayerId();
    if (pid == kInvalidPlayer)
        return false;
    PawnInfo p{};
    return m_ctx->players->GetPawnByPlayer(pid, p) && p.alive;
}

void TFSpawnScreen::OnLocalPlayerDied(const EvLocalPlayerDied& ev)
{
    if (!m_ctx || ev.player != m_ctx->localPlayer)
        return;
    m_respawnLeft = std::max(0.0f, ev.respawnDelay);
    if (!m_open)
        m_pendingOpen = kOpenDelaySec;   // HUD owns the first 2 s ("YOU ARE DOWN")

    PawnInfo p{};
    if (m_ctx->players && m_ctx->players->GetPawnByPlayer(ev.player, p)) {
        m_deathPos[0] = p.pos[0];
        m_deathPos[1] = p.pos[1];
        m_deathPos[2] = p.pos[2];
    }
}

void TFSpawnScreen::Open()
{
    if (m_open || !m_initialized)
        return;
    m_open = true;
    m_pendingOpen = 0.0f;
    // Suppress the HUD death overlay while this screen owns the frame.
    if (m_ctx && m_ctx->hud)
        m_ctx->hud->SetRespawnState(false, 0.0f);
}

void TFSpawnScreen::Close()
{
    if (!m_open)
        return;
    m_open = false;
    // Closed while still dead (e.g. via console): hand the remaining countdown
    // back to the HUD overlay so the player is never without death feedback.
    if (m_ctx && m_ctx->hud && !LocalPawnAlive())
        m_ctx->hud->SetRespawnState(true, m_respawnLeft);
}

void TFSpawnScreen::Update(float deltaTime)
{
    if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer())
        return;

    m_respawnLeft = std::max(0.0f, m_respawnLeft - deltaTime);
    m_debounce    = std::max(0.0f, m_debounce - deltaTime);

    // Death -> delayed auto-open.
    if (m_pendingOpen > 0.0f) {
        m_pendingOpen -= deltaTime;
        if (m_pendingOpen <= 0.0f && !LocalPawnAlive())
            Open();
    }

    // First-deploy auto-open: once, when a session is up and no pawn exists
    // (fresh boot / fresh connect). Covers the faction pick + first spawn.
    if (!m_bootOpened) {
        if (LocalPawnAlive()) {
            m_bootOpened = true;   // spawned some other way (tf_spawn)
        } else if (m_ctx->clientNet && m_ctx->clientNet->IsConnected() &&
                   m_ctx->data && m_ctx->data->IsLoaded()) {
            m_bootOpenDelay -= deltaTime;
            if (m_bootOpenDelay <= 0.0f) {
                m_bootOpened = true;
                Open();
            }
        }
    }

    if (m_open) {
        if (LocalPawnAlive()) {
            Close();   // spawn went through (any path, incl. pure client)
        } else if (m_ctx->hud) {
            // Keep the HUD overlay suppressed even if a late TF_SpawnReply
            // (reason=timer) re-armed it via TFClientNet.
            m_ctx->hud->SetRespawnState(false, 0.0f);
        }
    }
}

// ---------------------------------------------------------------------------
// Sends
// ---------------------------------------------------------------------------

void TFSpawnScreen::SendFactionSelect(FactionId f)
{
    if (!m_ctx || !m_ctx->clientNet || !m_ctx->clientNet->IsConnected())
        return;
    TF_FactionSelect sel{};
    sel.faction = static_cast<uint8_t>(f);
    m_ctx->clientNet->SendMsg(TFMsg::FactionSelect, &sel, sizeof(sel));
    // Optimistic local mirror (the loopback route sets it too; the server
    // remains authoritative for remote sessions).
    m_ctx->localFaction = f;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] faction selected: %s", FactionTag(f));
}

void TFSpawnScreen::SendSpawnRequest()
{
    if (!m_ctx || !m_ctx->clientNet || !m_ctx->clientNet->IsConnected())
        return;
    TF_SpawnRequest rq{};
    rq.classId   = static_cast<uint8_t>(m_selClass);
    rq.spawnKind = m_selKind;
    rq.regionId  = (m_selKind == 1) ? m_selRegion : 0;
    m_ctx->clientNet->SendMsg(TFMsg::SpawnRequest, &rq, sizeof(rq));
    m_debounce = kRequestDebounceSec;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

#ifdef ENABLE_EDITOR

void TFSpawnScreen::RenderUI()
{
    if (!m_open || !m_initialized || !m_ctx || !m_ctx->HasLocalPlayer())
        return;
    if (m_ctx->map && m_ctx->map->IsOpen())
        return;   // continent map is on top; we resume when it closes
    if (!m_ctx->data || !m_ctx->data->IsLoaded())
        return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (!ImGui::Begin("##TFSpawnScreen", nullptr, flags)) {
        ImGui::End();
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                      IM_COL32(6, 8, 12, 215));

    const float panelW = std::min(960.0f, vp->Size.x * 0.92f);
    const float panelH = std::min(600.0f, vp->Size.y * 0.88f);
    const float panelX = vp->Pos.x + (vp->Size.x - panelW) * 0.5f;
    const float panelY = vp->Pos.y + (vp->Size.y - panelH) * 0.5f;
    dl->AddRectFilled(ImVec2(panelX, panelY), ImVec2(panelX + panelW, panelY + panelH),
                      IM_COL32(16, 19, 25, 235), 6.0f);
    dl->AddRect(ImVec2(panelX, panelY), ImVec2(panelX + panelW, panelY + panelH),
                TFUi::FactionCol(m_ctx->localFaction, 0.55f), 6.0f, 0, 2.0f);

    if (m_ctx->localFaction == FactionId::None)
        DrawFactionSplash(panelX, panelY, panelW, panelH);
    else
        DrawDeployPanel(panelX, panelY, panelW, panelH);

    ImGui::End();
}

void TFSpawnScreen::DrawFactionSplash(float panelX, float panelY, float panelW, float panelH)
{
    using namespace TFUi;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    AddTextCentered(dl, 30.0f, ImVec2(panelX + panelW * 0.5f, panelY + 44.0f),
                    IM_COL32(235, 235, 235, 235), "CHOOSE YOUR FACTION");
    AddTextCentered(dl, 15.0f, ImVec2(panelX + panelW * 0.5f, panelY + 76.0f),
                    IM_COL32(170, 174, 180, 210),
                    "Veyra is shattered. Three powers war over its flux wells.");

    const float pad = 22.0f;
    const float cardW = (panelW - pad * 4.0f) / 3.0f;
    const float cardH = panelH - 160.0f;
    int i = 0;
    for (FactionId f : {FactionId::MRA, FactionId::AUC, FactionId::HLX}) {
        const float x = panelX + pad + static_cast<float>(i) * (cardW + pad);
        ImGui::SetCursorScreenPos(ImVec2(x, panelY + 104.0f));

        float c[4];
        FactionColor(f, c);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(c[0] * 0.45f, c[1] * 0.45f, c[2] * 0.45f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(c[0] * 0.70f, c[1] * 0.70f, c[2] * 0.70f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(c[0], c[1], c[2], 1.0f));

        char label[96];
        std::snprintf(label, sizeof(label), "%s\n[%s]##fac%d", FactionName(f), FactionTag(f), i);
        if (ImGui::Button(label, ImVec2(cardW, cardH * 0.5f)))
            SendFactionSelect(f);
        ImGui::PopStyleColor(3);

        const FactionDef* fd = m_ctx->data->GetFaction(f);
        ImGui::SetCursorScreenPos(ImVec2(x + 4.0f, panelY + 112.0f + cardH * 0.5f));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cardW - 8.0f);  // window-local
        ImGui::TextWrapped("%s", fd && !fd->blurb.empty() ? fd->blurb.c_str() : "");
        ImGui::PopTextWrapPos();
        ++i;
    }
}

void TFSpawnScreen::DrawDeployPanel(float panelX, float panelY, float panelW, float panelH)
{
    using namespace TFUi;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    TFRegionSystem* rs = m_ctx->regions;
    const FactionId myFaction = m_ctx->localFaction;
    const auto& regions = m_ctx->data->GetContinent().regions;

    char buf[160];
    std::snprintf(buf, sizeof(buf), "DEPLOY  -  %s", FactionName(myFaction));
    AddTextCentered(dl, 26.0f, ImVec2(panelX + panelW * 0.5f, panelY + 32.0f),
                    FactionCol(myFaction, 1.0f), buf);

    const float pad = 20.0f;
    const float topY = panelY + 62.0f;
    const float listH = panelH - 62.0f - 76.0f;
    const float classW = panelW * 0.38f - pad * 1.5f;
    const float spawnW = panelW - classW - pad * 3.0f;

    // ---- class picker ------------------------------------------------------
    ImGui::SetCursorScreenPos(ImVec2(panelX + pad, topY));
    ImGui::BeginChild("##tf_classes", ImVec2(classW, listH), true);
    ImGui::TextDisabled("CLASS");
    ImGui::Separator();
    for (const ClassDef& cd : m_ctx->data->AllClasses()) {
        const bool isColossus = (cd.id == ClassId::Colossus);
        const bool selected = (cd.id == m_selClass);
        std::snprintf(buf, sizeof(buf), "%s\n  %s%s##cls%u", cd.name.c_str(),
                      cd.role.c_str(), isColossus ? "  (exosuit - via terminal)" : "",
                      static_cast<unsigned>(cd.id));
        if (ImGui::Selectable(buf, selected,
                              isColossus ? ImGuiSelectableFlags_Disabled : 0,
                              ImVec2(0.0f, 38.0f)))
            m_selClass = cd.id;
        if (!isColossus && ImGui::IsItemHovered() && !cd.ability.name.empty())
            ImGui::SetTooltip("%s - %s", cd.ability.name.c_str(), cd.ability.desc.c_str());
        ImGui::Spacing();
    }
    ImGui::EndChild();

    // ---- spawn point list ----------------------------------------------------
    // Keep the region selection valid against live territory state.
    if (m_selKind == 1 &&
        (!rs || m_selRegion == kInvalidRegion || !rs->CanSpawnAt(m_selRegion, myFaction)))
    {
        m_selKind = 0;
        m_selRegion = kInvalidRegion;
    }

    auto distTo = [&](const RegionDef& rd) {
        const float dx = rd.centerX - m_deathPos[0];
        const float dz = rd.centerZ - m_deathPos[2];
        return std::sqrt(dx * dx + dz * dz);
    };

    ImGui::SetCursorScreenPos(ImVec2(panelX + pad * 2.0f + classW, topY));
    ImGui::BeginChild("##tf_spawns", ImVec2(spawnW, listH), true);
    ImGui::TextDisabled("SPAWN POINT");
    ImGui::Separator();

    // Skyanchor is always available (spawnKind 0; the server picks the pads).
    {
        const RegionDef* home = nullptr;
        for (const RegionDef& rd : regions)
            if (rd.tier == "skyanchor" && rd.homeFaction == myFaction) { home = &rd; break; }
        if (home)
            std::snprintf(buf, sizeof(buf), "%s   -   HOME   %.0fm##sky",
                          home->name.c_str(), distTo(*home));
        else
            std::snprintf(buf, sizeof(buf), "Skyanchor   -   HOME##sky");
        if (ImGui::Selectable(buf, m_selKind == 0)) {
            m_selKind = 0;
            m_selRegion = kInvalidRegion;
        }
    }

    for (const RegionDef& rd : regions) {
        if (rd.tier == "skyanchor")
            continue;   // covered by the HOME entry
        if (!rs || !rs->CanSpawnAt(rd.id, myFaction))
            continue;
        std::snprintf(buf, sizeof(buf), "%s   -   %s   %.0fm##rg%u", rd.name.c_str(),
                      TierTag(rd.tier), distTo(rd), static_cast<unsigned>(rd.id));
        if (ImGui::Selectable(buf, m_selKind == 1 && m_selRegion == rd.id)) {
            m_selKind = 1;
            m_selRegion = rd.id;
        }
    }
    ImGui::EndChild();

    // ---- footer: countdown + DEPLOY + map shortcut -----------------------------
    const float footY = panelY + panelH - 60.0f;
    ImGui::SetCursorScreenPos(ImVec2(panelX + pad, footY + 8.0f));
    if (m_respawnLeft > 0.0f)
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "Respawn in %.1fs", m_respawnLeft);
    else
        ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f), "Ready to deploy");

    ImGui::SetCursorScreenPos(ImVec2(panelX + pad, footY + 28.0f));
    ImGui::TextDisabled("M - continent map (click a linked region to deploy)");

    ImGui::SetCursorScreenPos(ImVec2(panelX + panelW - 330.0f, footY));
    if (ImGui::Button("OPEN MAP", ImVec2(120.0f, 44.0f)) && m_ctx->map)
        m_ctx->map->Open();

    ImGui::SetCursorScreenPos(ImVec2(panelX + panelW - 195.0f, footY));
    const bool blocked = (m_respawnLeft > 0.0f) || (m_debounce > 0.0f) ||
                         !m_ctx->clientNet || !m_ctx->clientNet->IsConnected();
    ImGui::BeginDisabled(blocked);
    if (m_respawnLeft > 0.0f)
        std::snprintf(buf, sizeof(buf), "DEPLOY (%.1fs)", m_respawnLeft);
    else
        std::snprintf(buf, sizeof(buf), "DEPLOY");
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.42f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.56f, 0.26f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.24f, 0.68f, 0.30f, 1.0f));
    if (ImGui::Button(buf, ImVec2(175.0f, 44.0f)))
        SendSpawnRequest();
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
}

#else // !ENABLE_EDITOR — headless: screen is state-only

void TFSpawnScreen::RenderUI() {}
void TFSpawnScreen::DrawFactionSplash(float, float, float, float) {}
void TFSpawnScreen::DrawDeployPanel(float, float, float, float) {}

#endif // ENABLE_EDITOR

} // namespace Terrafront

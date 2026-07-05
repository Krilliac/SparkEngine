/**
 * @file TFScoreboard.cpp
 * @brief Hold-TAB fullscreen scoreboard: three faction columns of K/D/score,
 *        region counts, Dominion status, local rank/XP/flux footer.
 */
#include "UI/TFScoreboard.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFProgressionSystem.h"
#include "Net/TFClientNet.h"
#include "World/TFRegionSystem.h"

#include "Input/InputManager.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <cstdio>
#include <vector>

namespace Terrafront {

namespace {

constexpr int      kVkTab           = 0x09;
constexpr uint32_t kKillScore       = 100;   // client-side estimate, DESIGN §4
constexpr uint32_t kHeadshotScore   = 25;

} // namespace

TFScoreboard::TFScoreboard() = default;
TFScoreboard::~TFScoreboard() { if (m_initialized) Shutdown(); }

bool TFScoreboard::Initialize(TFGameContext& ctx, TFEventBus& events)
{
    m_ctx = &ctx;
    m_events = &events;

    // Authority roles tally straight off the bus (fired by TFPlayerSystem);
    // connected clients get TF_KillEvent forwarded via ClientNoteKill.
    events.Subscribe<EvPlayerKilled>([this](const EvPlayerKilled& ev) { OnPlayerKilled(ev); });
    events.Subscribe<EvPlayerSpawned>([this](const EvPlayerSpawned& ev) {
        Ensure(ev.player, ev.faction);
    });

    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFScoreboard initialized");
    return true;
}

void TFScoreboard::Update(float deltaTime)
{
    (void)deltaTime;
    if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer())
    {
        m_open = false;
        return;
    }
    InputManager* input = m_ctx->engine ? m_ctx->engine->GetInput() : nullptr;
    m_open = input && input->IsKeyDown(kVkTab);
}

void TFScoreboard::FixedUpdate(float fixedDeltaTime) { (void)fixedDeltaTime; }

void TFScoreboard::Shutdown()
{
    m_rows.clear();
    m_open = false;
    m_initialized = false;
}

// ---------------------------------------------------------------------------
// Tallies
// ---------------------------------------------------------------------------

TFScoreboard::Row& TFScoreboard::Ensure(PlayerId player, FactionId faction)
{
    Row& row = m_rows[player];
    if (faction != FactionId::None)
        row.faction = faction;
    return row;
}

void TFScoreboard::OnPlayerKilled(const EvPlayerKilled& ev)
{
    if (!m_ctx || !m_ctx->IsAuthority())
        return;   // pure clients tally through ClientNoteKill instead
    FactionId killerF = FactionId::None;
    FactionId victimF = FactionId::None;
    if (m_ctx->players)
    {
        killerF = m_ctx->players->FactionOf(ev.killer);
        victimF = m_ctx->players->FactionOf(ev.victim);
    }
    TallyKill(ev.killer, ev.victim, killerF, victimF, ev.headshot);
}

void TFScoreboard::ClientNoteKill(const TF_KillEvent& ke)
{
    if (!m_initialized || !m_ctx || m_ctx->IsAuthority())
        return;   // the bus path already tallied it
    TallyKill(ke.killerPlayer, ke.victimPlayer,
              static_cast<FactionId>(ke.killerFaction),
              static_cast<FactionId>(ke.victimFaction),
              ke.headshot != 0);
}

void TFScoreboard::TallyKill(PlayerId killer, PlayerId victim,
                             FactionId killerF, FactionId victimF, bool headshot)
{
    if (victim != kInvalidPlayer)
        ++Ensure(victim, victimF).deaths;

    if (killer == kInvalidPlayer || killer == victim)
        return;                                   // environment / suicide
    if (killerF != FactionId::None && killerF == victimF)
        return;                                   // team kill: no credit

    Row& kr = Ensure(killer, killerF);
    ++kr.kills;
    kr.score += kKillScore + (headshot ? kHeadshotScore : 0);
}

void TFScoreboard::RefreshRoster()
{
    if (m_ctx && m_ctx->players)
    {
        m_ctx->players->ForEachAlivePawn([this](const PawnInfo& p) {
            Ensure(p.owner, p.faction);
        });
        if (m_ctx->localPlayer != kInvalidPlayer)
            Ensure(m_ctx->localPlayer, m_ctx->players->FactionOf(m_ctx->localPlayer));
    }
}

uint32_t TFScoreboard::ScoreOf(PlayerId player, const Row& row) const
{
    // The authority has the real XP totals; clients show a kill-XP estimate.
    if (m_ctx && m_ctx->IsAuthority() && m_ctx->progression)
        return m_ctx->progression->XPOf(player);
    return row.score;
}

FactionId TFScoreboard::ComputeDominion() const
{
    if (!m_ctx || !m_ctx->regions || !m_ctx->data || !m_ctx->data->IsLoaded())
        return FactionId::None;

    FactionId holder = FactionId::None;
    for (const RegionDef& rd : m_ctx->data->GetContinent().regions)
    {
        if (rd.tier == "skyanchor")
            continue;
        const FactionId owner = m_ctx->regions->OwnerOf(rd.id);
        if (owner == FactionId::None)
            return FactionId::None;               // neutral ground left
        if (holder == FactionId::None)
            holder = owner;
        else if (owner != holder)
            return FactionId::None;               // split ownership
    }
    return holder;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

#ifdef ENABLE_EDITOR

namespace {

void PlayerLabel(PlayerId id, PlayerId localId, char out[16])
{
    if (id == localId)
        std::snprintf(out, 16, "YOU");
    else if (id == kTFLocalHostPlayer)
        std::snprintf(out, 16, "HOST");
    else
        std::snprintf(out, 16, "P%u", id);
}

} // namespace

void TFScoreboard::RenderUI()
{
    if (!m_initialized || !m_open || !m_ctx || !m_ctx->HasLocalPlayer())
        return;

    RefreshRoster();

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f, 0.02f, 0.04f, 0.72f));
    if (!ImGui::Begin("##TFScoreboard", nullptr, flags))
    {
        ImGui::End();
        ImGui::PopStyleColor();
        return;
    }

    ImGui::Dummy(ImVec2(0.0f, vp->Size.y * 0.06f));
    DrawHeader();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Columns(3, "##tfsb_cols", true);
    DrawFactionColumn(FactionId::MRA);
    ImGui::NextColumn();
    DrawFactionColumn(FactionId::AUC);
    ImGui::NextColumn();
    DrawFactionColumn(FactionId::HLX);
    ImGui::Columns(1);

    ImGui::End();
    ImGui::PopStyleColor();
}

void TFScoreboard::DrawHeader()
{
    const char* continent = "Cindral Wastes";
    if (m_ctx->data && m_ctx->data->IsLoaded() && !m_ctx->data->GetContinent().name.empty())
        continent = m_ctx->data->GetContinent().name.c_str();

    char line[128];
    std::snprintf(line, sizeof(line), "TERRAFRONT  --  %s", continent);
    const float w = ImGui::GetWindowWidth();
    ImGui::SetCursorPosX((w - ImGui::CalcTextSize(line).x) * 0.5f);
    ImGui::TextUnformatted(line);

    const FactionId dom = ComputeDominion();
    if (dom != FactionId::None)
    {
        float col[4];
        FactionColor(dom, col);
        std::snprintf(line, sizeof(line), "DOMINION: %s", FactionName(dom));
        ImGui::SetCursorPosX((w - ImGui::CalcTextSize(line).x) * 0.5f);
        ImGui::TextColored(ImVec4(col[0], col[1], col[2], 1.0f), "%s", line);
    }
    else
    {
        std::snprintf(line, sizeof(line), "Territory contested");
        ImGui::SetCursorPosX((w - ImGui::CalcTextSize(line).x) * 0.5f);
        ImGui::TextDisabled("%s", line);
    }

    // Local progression footer (authoritative numbers only exist server-side;
    // clients fall back to the last TF_XPEvent-driven HUD rank).
    if (m_ctx->IsAuthority() && m_ctx->progression && m_ctx->localPlayer != kInvalidPlayer)
    {
        const PlayerId me = m_ctx->localPlayer;
        std::snprintf(line, sizeof(line), "Rank %u   XP %u   Flux %u",
                      m_ctx->progression->RankOf(me),
                      m_ctx->progression->XPOf(me),
                      m_ctx->progression->FluxOf(me));
        ImGui::SetCursorPosX((w - ImGui::CalcTextSize(line).x) * 0.5f);
        ImGui::TextUnformatted(line);
    }
}

void TFScoreboard::DrawFactionColumn(FactionId faction)
{
    float col[4];
    FactionColor(faction, col);
    const ImVec4 fcol(col[0], col[1], col[2], 1.0f);

    uint32_t regions = 0;
    if (m_ctx->regions)
        regions = m_ctx->regions->RegionsHeld(faction);

    ImGui::TextColored(fcol, "%s  [%s]", FactionName(faction), FactionTag(faction));
    ImGui::TextDisabled("regions held: %u", regions);
    ImGui::Separator();

    // Collect + sort this faction's rows by score, then kills.
    std::vector<std::pair<PlayerId, const Row*>> list;
    list.reserve(m_rows.size());
    for (const auto& [id, row] : m_rows)
        if (row.faction == faction)
            list.emplace_back(id, &row);
    std::sort(list.begin(), list.end(),
              [this](const auto& a, const auto& b) {
                  const uint32_t sa = ScoreOf(a.first, *a.second);
                  const uint32_t sb = ScoreOf(b.first, *b.second);
                  if (sa != sb) return sa > sb;
                  if (a.second->kills != b.second->kills)
                      return a.second->kills > b.second->kills;
                  return a.first < b.first;
              });

    ImGui::Text("%-8s %5s %5s %8s", "PLAYER", "K", "D", "SCORE");
    for (const auto& [id, row] : list)
    {
        char name[16];
        PlayerLabel(id, m_ctx->localPlayer, name);
        if (id == m_ctx->localPlayer)
            ImGui::TextColored(ImVec4(1.0f, 0.95f, 0.6f, 1.0f), "%-8s %5u %5u %8u",
                               name, row->kills, row->deaths, ScoreOf(id, *row));
        else
            ImGui::Text("%-8s %5u %5u %8u", name, row->kills, row->deaths, ScoreOf(id, *row));
    }
    if (list.empty())
        ImGui::TextDisabled("(no players)");
}

void TFScoreboard::RenderDebugUI()
{
    if (!ImGui::CollapsingHeader("TF Scoreboard"))
        return;
    ImGui::Text("open : %s", m_open ? "yes (TAB)" : "no");
    ImGui::Text("rows : %zu", m_rows.size());
}

#else // !ENABLE_EDITOR — headless / no ImGui: tallies only

void TFScoreboard::RenderUI() {}
void TFScoreboard::DrawHeader() {}
void TFScoreboard::DrawFactionColumn(FactionId) {}
void TFScoreboard::RenderDebugUI() {}

#endif // ENABLE_EDITOR

} // namespace Terrafront

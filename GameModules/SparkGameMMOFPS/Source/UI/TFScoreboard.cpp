/**
 * @file TFScoreboard.cpp
 * @brief Hold-TAB fullscreen scoreboard: stacked faction sections with
 *        Name | Outfit | Class | Score | K | D | Cap tables (W10 v2), region
 *        counts, Dominion status, local rank/XP/flux footer.
 */
#include "UI/TFScoreboard.h"

#include "Data/TFDataTables.h"
#include "Game/TFMedalSystem.h"  // W10 medals-scoreboard lane: authoritative rows
#include "Game/TFOutfitSystem.h" // Outfits lane: row name tags
#include "Game/TFPlayerSystem.h"
#include "Game/TFProgressionSystem.h"
#include "Net/TFClientNet.h"
#include "UI/TFHUD.h"
#include "UI/TFKeybinds.h"
#include "World/TFRegionSystem.h"

#include "Graphics/GraphicsEngine.h"
#include "Input/InputManager.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cstdio>
#include <vector>

namespace Terrafront
{

    namespace
    {

        constexpr uint32_t kKillScore = 100; // client-side estimate, DESIGN §4
        constexpr uint32_t kHeadshotScore = 25;

    } // namespace

    TFScoreboard::TFScoreboard() = default;
    TFScoreboard::~TFScoreboard()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFScoreboard::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;

        // Authority roles tally straight off the bus (fired by TFPlayerSystem);
        // connected clients get TF_KillEvent forwarded via ClientNoteKill.
        events.Subscribe<EvPlayerKilled>([this](const EvPlayerKilled& ev) { OnPlayerKilled(ev); });
        events.Subscribe<EvPlayerSpawned>([this](const EvPlayerSpawned& ev)
                                          { Ensure(ev.player, ev.faction).cls = ev.cls; });

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
        // W7 ui-map-keys: hold key comes from the shared keybind table; a focused
        // chat input owns the keyboard (Tab must not flash the board mid-typing).
        InputManager* input = m_ctx->engine ? m_ctx->engine->GetInput() : nullptr;
        const bool chatOpen = m_ctx->hud && m_ctx->hud->IsChatOpen();
        m_open = input && !chatOpen && TFKeys::IsActionDown(*input, TFKeys::Action::HoldScoreboard);
    }

    void TFScoreboard::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime;
    }

    void TFScoreboard::Shutdown()
    {
        m_rows.clear();
        m_medals = nullptr;
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
            return; // pure clients tally through ClientNoteKill instead
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
            return; // the bus path already tallied it
        TallyKill(ke.killerPlayer, ke.victimPlayer, static_cast<FactionId>(ke.killerFaction),
                  static_cast<FactionId>(ke.victimFaction), ke.headshot != 0);
    }

    void TFScoreboard::TallyKill(PlayerId killer, PlayerId victim, FactionId killerF, FactionId victimF, bool headshot)
    {
        if (victim != kInvalidPlayer)
            ++Ensure(victim, victimF).deaths;

        if (killer == kInvalidPlayer || killer == victim)
            return; // environment / suicide
        if (killerF != FactionId::None && killerF == victimF)
            return; // team kill: no credit

        Row& kr = Ensure(killer, killerF);
        ++kr.kills;
        kr.score += kKillScore + (headshot ? kHeadshotScore : 0);
    }

    void TFScoreboard::RefreshRoster()
    {
        if (m_ctx && m_ctx->players)
        {
            m_ctx->players->ForEachAlivePawn(
                [this](const PawnInfo& p)
                {
                    Row& row = Ensure(p.owner, p.faction);
                    row.cls = p.cls; // live class beats the last replicated one
                });
            if (m_ctx->localPlayer != kInvalidPlayer)
                Ensure(m_ctx->localPlayer, m_ctx->players->FactionOf(m_ctx->localPlayer));
        }
        MergeMedalRows();
    }

    void TFScoreboard::MergeMedalRows()
    {
        // W10: overlay the authoritative TFMedalSystem rows (server truth on
        // authority roles, TF_ScoreUpdate mirror on pure clients). The W2 kill
        // tallies keep accruing between 4 Hz flushes; each merge overwrites
        // them with the server numbers. Null m_medals == W2 fallback behavior.
        if (!m_medals)
            return;
        m_medals->ForEachScoreRow(
            [this](PlayerId id, const TFScoreRow& sr)
            {
                Row& row = Ensure(id, sr.faction);
                row.hasAuth = true;
                row.authScore = sr.score;
                row.kills = sr.kills;
                row.deaths = sr.deaths;
                row.captures = sr.captures;
                row.medals = sr.medals;
                if (row.cls == ClassId::COUNT && sr.cls != ClassId::COUNT)
                    row.cls = sr.cls; // replicated class fills the gaps
            });
    }

    uint32_t TFScoreboard::ScoreOf(PlayerId player, const Row& row) const
    {
        // W10: the medal system's replicated score formula is the truth
        // everywhere once available (kills*100 + captures*250 + medals*50).
        if (row.hasAuth)
            return row.authScore;
        // W2 fallback: the authority has real XP totals; clients estimate.
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
                return FactionId::None; // neutral ground left
            if (holder == FactionId::None)
                holder = owner;
            else if (owner != holder)
                return FactionId::None; // split ownership
        }
        return holder;
    }

    // ---------------------------------------------------------------------------
    // Rendering
    // ---------------------------------------------------------------------------

#ifdef SPARK_HAS_IMGUI

    namespace
    {

        void PlayerLabel(PlayerId id, PlayerId localId, char out[16])
        {
            if (id == localId)
                std::snprintf(out, 16, "YOU");
            else if (id == kTFLocalHostPlayer)
                std::snprintf(out, 16, "HOST");
            else
                std::snprintf(out, 16, "P%u", id);
        }

        /// class_*.png basename for the shipped ui icon set (nullptr = unknown).
        const char* ClassIconKey(ClassId c)
        {
            switch (c)
            {
            case ClassId::Ghost:
                return "ghost";
            case ClassId::Striker:
                return "striker";
            case ClassId::Medtech:
                return "medtech";
            case ClassId::Fabricator:
                return "fabricator";
            case ClassId::Bulwark:
                return "bulwark";
            case ClassId::Colossus:
                return "colossus";
            default:
                return nullptr;
            }
        }

        /// Two-letter text fallback when the icon texture is unavailable.
        const char* ClassShortTag(ClassId c)
        {
            switch (c)
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

    } // namespace

    void TFScoreboard::RenderUI()
    {
        if (!m_initialized || !m_open || !m_ctx || !m_ctx->HasLocalPlayer())
            return;

        RefreshRoster();

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f, 0.02f, 0.04f, 0.72f));
        if (!ImGui::Begin("##TFScoreboard", nullptr, flags))
        {
            ImGui::End();
            ImGui::PopStyleColor();
            return;
        }

        ImGui::Dummy(ImVec2(0.0f, vp->Size.y * 0.04f));
        DrawHeader();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // W10 v2: three stacked faction sections, each a centered table
        // (7 columns don't fit three-abreast).
        DrawFactionSection(FactionId::MRA);
        ImGui::Spacing();
        DrawFactionSection(FactionId::AUC);
        ImGui::Spacing();
        DrawFactionSection(FactionId::HLX);

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
            std::snprintf(line, sizeof(line), "Rank %u   XP %u   Flux %u", m_ctx->progression->RankOf(me),
                          m_ctx->progression->XPOf(me), m_ctx->progression->FluxOf(me));
            ImGui::SetCursorPosX((w - ImGui::CalcTextSize(line).x) * 0.5f);
            ImGui::TextUnformatted(line);
        }
    }

    void TFScoreboard::DrawFactionSection(FactionId faction)
    {
        float col[4];
        FactionColor(faction, col);
        const ImVec4 fcol(col[0], col[1], col[2], 1.0f);

        uint32_t regions = 0;
        if (m_ctx->regions)
            regions = m_ctx->regions->RegionsHeld(faction);

        // Centered ~72%-width section (header text + table share the margin).
        const float winW = ImGui::GetWindowWidth();
        const float sectionW = winW * 0.72f;
        const float marginX = (winW - sectionW) * 0.5f;

        ImGui::SetCursorPosX(marginX);
        ImGui::TextColored(fcol, "%s  [%s]", FactionName(faction), FactionTag(faction));
        ImGui::SameLine();
        ImGui::TextDisabled("-- regions held: %u", regions);

        // Collect + sort this faction's rows by score, then kills.
        std::vector<std::pair<PlayerId, const Row*>> list;
        list.reserve(m_rows.size());
        for (const auto& [id, row] : m_rows)
            if (row.faction == faction)
                list.emplace_back(id, &row);
        std::sort(list.begin(), list.end(),
                  [this](const auto& a, const auto& b)
                  {
                      const uint32_t sa = ScoreOf(a.first, *a.second);
                      const uint32_t sb = ScoreOf(b.first, *b.second);
                      if (sa != sb)
                          return sa > sb;
                      if (a.second->kills != b.second->kills)
                          return a.second->kills > b.second->kills;
                      return a.first < b.first;
                  });

        if (list.empty())
        {
            ImGui::SetCursorPosX(marginX);
            ImGui::TextDisabled("(no players)");
            return;
        }

        GraphicsEngine* gfx = m_ctx->engine ? m_ctx->engine->GetGraphics() : nullptr;
        const bool canDrawIcons = gfx && gfx->GetDevice() && gfx->GetContext();

        char tableId[32];
        std::snprintf(tableId, sizeof(tableId), "##tfsb_%u", static_cast<uint32_t>(faction));
        const ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
        ImGui::SetCursorPosX(marginX);
        if (!ImGui::BeginTable(tableId, 7, tflags, ImVec2(sectionW, 0.0f)))
            return;

        ImGui::TableSetupColumn("PLAYER", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("OUTFIT", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("CLASS", ImGuiTableColumnFlags_WidthFixed, 44.0f);
        ImGui::TableSetupColumn("SCORE", ImGuiTableColumnFlags_WidthStretch, 1.5f);
        ImGui::TableSetupColumn("K", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("D", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("CAP", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();

        for (const auto& [id, row] : list)
        {
            const bool isLocal = id == m_ctx->localPlayer;
            ImGui::TableNextRow();
            if (isLocal)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                       ImGui::ColorConvertFloat4ToU32(ImVec4(0.9f, 0.8f, 0.3f, 0.22f)));

            char name[16];
            PlayerLabel(id, m_ctx->localPlayer, name);
            const ImVec4 rowCol = isLocal ? ImVec4(1.0f, 0.95f, 0.6f, 1.0f) : ImVec4(0.92f, 0.92f, 0.92f, 1.0f);

            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(rowCol, "%s", name);

            ImGui::TableSetColumnIndex(1);
            const char* tag = m_ctx->outfits ? m_ctx->outfits->GetOutfitTag(id) : "";
            if (tag && tag[0] != '\0')
                ImGui::TextColored(rowCol, "[%s]", tag);
            else
                ImGui::TextDisabled("-");

            ImGui::TableSetColumnIndex(2);
            bool drewIcon = false;
            if (canDrawIcons && row->cls != ClassId::COUNT)
            {
                if (const char* key = ClassIconKey(row->cls))
                {
                    char iconPath[80];
                    std::snprintf(iconPath, sizeof(iconPath), "Assets/Textures/MMOFPS/ui/64/class_%s.png", key);
                    if (ID3D11ShaderResourceView* srv = gfx->GetOrLoadTextureSRV(iconPath))
                    {
                        ImGui::Image(static_cast<void*>(srv), ImVec2(18.0f, 18.0f));
                        drewIcon = true;
                    }
                }
            }
            if (!drewIcon)
                ImGui::TextDisabled("%s", ClassShortTag(row->cls)); // headless / unknown class

            ImGui::TableSetColumnIndex(3);
            ImGui::TextColored(rowCol, "%u", ScoreOf(id, *row));
            ImGui::TableSetColumnIndex(4);
            ImGui::TextColored(rowCol, "%u", row->kills);
            ImGui::TableSetColumnIndex(5);
            ImGui::TextColored(rowCol, "%u", row->deaths);
            ImGui::TableSetColumnIndex(6);
            if (row->hasAuth)
                ImGui::TextColored(rowCol, "%u", row->captures);
            else
                ImGui::TextDisabled("-"); // capture data needs the medal system
        }

        ImGui::EndTable();
    }

    void TFScoreboard::RenderDebugUI()
    {
        if (!ImGui::CollapsingHeader("TF Scoreboard"))
            return;
        ImGui::Text("open : %s", m_open ? "yes (TAB)" : "no");
        ImGui::Text("rows : %zu", m_rows.size());
        ImGui::Text("medal system : %s", m_medals ? "wired" : "absent (W2 fallback)");
    }

#else // !SPARK_HAS_IMGUI — headless / no ImGui: tallies only

    void TFScoreboard::RenderUI() {}
    void TFScoreboard::DrawHeader() {}
    void TFScoreboard::DrawFactionSection(FactionId) {}
    void TFScoreboard::RenderDebugUI() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront

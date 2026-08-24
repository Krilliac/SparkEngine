/**
 * @file TFOutfitSystem.cpp
 * @brief Outfits (clans/guilds) core: lifecycle, name/tag validation,
 *        cross-system queries and the server player<->character binding.
 *        Split parts (repo file-size rules): server op handlers in
 *        TFOutfitSystemServer.cpp, W12 score + leaderboard in
 *        TFOutfitSystemScore.cpp, wire send + client mirror in
 *        TFOutfitSystemWire.cpp, console commands in
 *        TFOutfitSystemConsole.cpp; shared helpers in
 *        TFOutfitSystemInternal.h. See the header for the full design note.
 */
#include "Game/TFOutfitSystem.h"

#include "Net/TFServerSim.h"
#include "Persistence/TFDatabase.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <cctype>

namespace Terrafront
{

    namespace
    {

        constexpr float kSweepPeriodSec = 2.0f; // departed-player fallback sweep cadence
        constexpr float kResultShowSec = 6.0f;  // UI status-line lifetime (panel reads resultAge)

    } // namespace

    const char* OutfitResultText(TFOutfitResult r)
    {
        switch (r)
        {
        case TFOutfitResult::Ok:
            return "ok";
        case TFOutfitResult::ServerError:
            return "server error";
        case TFOutfitResult::BadRequest:
            return "bad request";
        case TFOutfitResult::NotInOutfit:
            return "not in an outfit";
        case TFOutfitResult::AlreadyInOutfit:
            return "already in an outfit";
        case TFOutfitResult::TargetInOutfit:
            return "target is already in an outfit";
        case TFOutfitResult::NameInvalid:
            return "invalid outfit name (3-24 chars, letters/digits/single spaces)";
        case TFOutfitResult::TagInvalid:
            return "invalid tag (2-5 letters/digits)";
        case TFOutfitResult::NameTaken:
            return "outfit name already taken";
        case TFOutfitResult::TagTaken:
            return "outfit tag already taken";
        case TFOutfitResult::NoSuchPlayer:
            return "no such player online";
        case TFOutfitResult::NoSuchMember:
            return "no such outfit member";
        case TFOutfitResult::NoInvite:
            return "no pending invite";
        case TFOutfitResult::NotPermitted:
            return "not permitted for your rank";
        case TFOutfitResult::OutfitFull:
            return "outfit is full";
        default:
            return "unknown";
        }
    }

    // ---------------------------------------------------------------------------
    // Validation
    // ---------------------------------------------------------------------------

    bool TFOutfitSystem::ValidateOutfitName(const std::string& name)
    {
        if (name.size() < kTFOutfitNameMin || name.size() > kTFOutfitNameMax)
            return false;
        if (name.front() == ' ' || name.back() == ' ')
            return false;
        bool prevSpace = false;
        for (char c : name)
        {
            const unsigned char uc = static_cast<unsigned char>(c);
            if (c == ' ')
            {
                if (prevSpace)
                    return false; // no double spaces
                prevSpace = true;
                continue;
            }
            prevSpace = false;
            if (!std::isalnum(uc))
                return false;
        }
        return true;
    }

    bool TFOutfitSystem::ValidateOutfitTag(const std::string& tag)
    {
        if (tag.size() < kTFOutfitTagMin || tag.size() > kTFOutfitTagMax)
            return false;
        for (char c : tag)
            if (!std::isalnum(static_cast<unsigned char>(c)))
                return false;
        return true;
    }

    // ---------------------------------------------------------------------------
    // Lifecycle
    // ---------------------------------------------------------------------------

    TFOutfitSystem::TFOutfitSystem() = default;

    TFOutfitSystem::~TFOutfitSystem()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFOutfitSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;
        m_initialized = true;

        // Server binding fallback: if the preferred TFServerSim::HandleEnterWorld
        // hook (wave wiringNotes) is not applied yet, the first spawn of a player
        // still binds player<->character here, so tags/rosters flow either way.
        events.Subscribe<EvPlayerSpawned>([this](const EvPlayerSpawned& ev) { OnPlayerSpawned(ev); });

        // W12 competition score — aggregation off EXISTING event surfaces only
        // (TFMedalSystem precedent); handlers self-gate on authority.
        events.Subscribe<EvPlayerKilled>([this](const EvPlayerKilled& ev) { OnPlayerKilledScore(ev); });
        events.Subscribe<EvXPAwarded>([this](const EvXPAwarded& ev) { OnXPAwardedScore(ev); });

        RegisterConsoleCommands();

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFOutfitSystem initialized");
        return true;
    }

    void TFOutfitSystem::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx)
            return;

        if (m_ctx->IsAuthority())
        {
            m_store.Tick(deltaTime); // debounced persistence flush

            m_sweepAccum += deltaTime;
            if (m_sweepAccum >= kSweepPeriodSec)
            {
                m_sweepAccum = 0.0f;
                SweepDepartedPlayers();
                RolloverIfNeeded(); // tick-crossing-the-boundary weekly rollover path
            }
        }

#ifdef ENABLE_NETWORKING
        const bool clientUp = ClientNetActive();
        if (clientUp && !m_clientHandlers)
            EnsureClientHandlers();
        else if (!clientUp && m_clientHandlers)
        {
            ReleaseClientHandlers();
            // Connection dropped: the mirror + tag map describe a session that no
            // longer exists.
            m_mirror = Mirror{};
            m_lb = LeaderboardMirror{};
            m_tags.clear();
        }
#endif

        if (m_mirror.hasResult)
        {
            m_mirror.resultAge += deltaTime;
            if (m_mirror.resultAge > kResultShowSec)
                m_mirror.hasResult = false;
        }
    }

    void TFOutfitSystem::FixedUpdate(float) {}

    bool TFOutfitSystem::Checkpoint()
    {
        return !m_initialized || m_store.Checkpoint();
    }

    bool TFOutfitSystem::Shutdown()
    {
        if (!m_initialized)
            return true;

#ifdef ENABLE_NETWORKING
        if (m_clientHandlers)
            ReleaseClientHandlers();
#endif

        if (m_cmdsRegistered)
        {
            auto& console = Spark::SimpleConsole::GetInstance();
            for (const char* cmd :
                 {"tf_outfit_create", "tf_outfit_invite", "tf_outfit_accept", "tf_outfit_decline", "tf_outfit_leave",
                  "tf_outfit_kick", "tf_outfit_rank", "tf_outfit_disband", "tf_outfit_status", "tf_outfit_leaderboard"})
                console.UnregisterCommand(cmd);
            m_cmdsRegistered = false;
        }

        if (!m_store.Close())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "[TF] outfit shutdown checkpoint failed; in-memory state retained for retry");
            return false;
        }
        m_boundChars.clear();
        m_playerOfChar.clear();
        m_invites.clear();
        m_tags.clear();
        m_mirror = Mirror{};
        m_lb = LeaderboardMirror{};
        m_lastWeekKey = 0; // a re-Initialize must re-run the load-time rollover sweep
        m_initialized = false;
        return true;
    }

    // ---------------------------------------------------------------------------
    // Cross-system API
    // ---------------------------------------------------------------------------

    const char* TFOutfitSystem::GetOutfitTag(PlayerId player) const
    {
        if (!m_initialized || player == kInvalidPlayer)
            return "";

        // Authority roles resolve live from the registry (always fresh, and the
        // dedicated server has no broadcast mirror of its own).
        if (m_ctx && m_ctx->IsAuthority() && m_store.IsOpen())
        {
            if (const BoundChar* bc = BoundCharOf(player))
                if (const TFOutfitRecord* rec = m_store.FindByCharacter(bc->charId))
                    return rec->tag.c_str();
            return "";
        }

        const auto it = m_tags.find(player);
        return it != m_tags.end() ? it->second.c_str() : "";
    }

    uint32_t TFOutfitSystem::OutfitIdOf(PlayerId player) const
    {
        // Mirrors GetOutfitTag's authority path exactly (registry is server
        // truth); there is no client-side fallback — outfit chat routing is a
        // server decision (W8 ui-polish, see the header note).
        if (!m_initialized || player == kInvalidPlayer || !m_ctx || !m_ctx->IsAuthority() || !m_store.IsOpen())
            return 0;
        if (const BoundChar* bc = BoundCharOf(player))
            if (const TFOutfitRecord* rec = m_store.FindByCharacter(bc->charId))
                return rec->id;
        return 0;
    }

    TFOutfitRank TFOutfitSystem::LocalRank() const
    {
        for (const MirrorMember& m : m_mirror.members)
            if (m.charId == m_mirror.yourCharId)
                return m.rank;
        return TFOutfitRank::Member;
    }

    // ---------------------------------------------------------------------------
    // Server: player <-> character binding
    // ---------------------------------------------------------------------------

    void TFOutfitSystem::ServerOnCharacterEntered(PlayerId player, uint64_t charId, const std::string& charName)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || player == kInvalidPlayer || charId == 0)
            return;

        auto it = m_boundChars.find(player);
        if (it != m_boundChars.end() && it->second.charId == charId)
        {
            if (!charName.empty())
                it->second.name = charName;
            return; // idempotent re-entry (EvPlayerSpawned fallback after the hook already ran)
        }

        if (it != m_boundChars.end())
            m_playerOfChar.erase(it->second.charId); // player switched characters

        BoundChar bc;
        bc.charId = charId;
        bc.name = charName;
        m_boundChars[player] = bc;
        m_playerOfChar[charId] = player;

        if (!EnsureStoreOpen())
            return; // no persistence: tags/rosters stay empty, ops answer ServerError

        m_store.UpdateMemberName(charId, charName); // keep roster name copies fresh

        // Late joiner: give it the current player->tag table, tell everyone about
        // it, and hand it its own outfit roster if it has one.
        SendTagTableTo(player);
        const TFOutfitRecord* rec = m_store.FindByCharacter(charId);
        BroadcastTag(player, rec ? rec->tag : std::string{});
        if (rec)
            SendRosterTo(player, rec);
    }

    void TFOutfitSystem::ServerOnPlayerLeft(PlayerId player)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority())
            return;
        auto it = m_boundChars.find(player);
        if (it == m_boundChars.end())
            return;
        m_playerOfChar.erase(it->second.charId);
        m_boundChars.erase(it);
        // Clear the departed id's tag on every remaining client so a recycled
        // PlayerId never renders the previous occupant's tag before rebinding.
        BroadcastTag(player, std::string{});
    }

    void TFOutfitSystem::OnPlayerSpawned(const EvPlayerSpawned& ev)
    {
        if (!m_ctx || !m_ctx->IsAuthority() || !m_ctx->serverSim)
            return;
        const uint64_t charId = m_ctx->serverSim->ActiveCharacterOf(ev.player);
        if (charId == 0)
            return; // bots / pre-onboarding sessions have no character

        const auto it = m_boundChars.find(ev.player);
        if (it != m_boundChars.end() && it->second.charId == charId)
            return; // already bound (preferred enter-world hook, or an earlier spawn)

        std::string name;
        if (m_ctx->db && m_ctx->db->IsOpen())
        {
            TFCharacterRecord rec;
            if (m_ctx->db->FindCharacter(charId, rec))
                name = rec.name;
        }
        ServerOnCharacterEntered(ev.player, charId, name);
    }

    void TFOutfitSystem::SweepDepartedPlayers()
    {
        if (!m_ctx->serverSim)
            return;
        std::vector<PlayerId> gone;
        for (const auto& [player, bc] : m_boundChars)
            if (!m_ctx->serverSim->IsEnteredWorld(player))
                gone.push_back(player);
        for (PlayerId p : gone)
            ServerOnPlayerLeft(p);
    }

    // ---------------------------------------------------------------------------
    // Debug UI
    // ---------------------------------------------------------------------------

    void TFOutfitSystem::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!ImGui::CollapsingHeader("TF Outfits"))
            return;
        ImGui::Text("store        : %s", m_store.IsOpen() ? "open" : (m_storeOpenFailed ? "OPEN FAILED" : "closed"));
        ImGui::Text("outfits      : %zu", m_store.OutfitCount());
        ImGui::Text("bound players: %zu   invites: %zu", m_boundChars.size(), m_invites.size());
        ImGui::Text("tag map      : %zu   bad packets: %u", m_tags.size(), m_badPackets);
        ImGui::Text("score events : %u   week key: %u   lb rows: %zu", m_scoreEvents, m_lastWeekKey, m_lb.rows.size());
        if (m_mirror.outfitId != 0)
            ImGui::Text("mirror       : '%s' [%s] %u members (you: %s)", m_mirror.name.c_str(), m_mirror.tag.c_str(),
                        m_mirror.totalMembers, OutfitRankName(LocalRank()));
        else
            ImGui::Text("mirror       : no outfit");
        if (m_mirror.hasInvite)
            ImGui::Text("invite       : '%s' [%s] from %s", m_mirror.inviteName.c_str(), m_mirror.inviteTag.c_str(),
                        m_mirror.inviteFrom.c_str());
#endif
    }

} // namespace Terrafront

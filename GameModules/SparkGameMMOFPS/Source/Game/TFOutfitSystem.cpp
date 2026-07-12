/**
 * @file TFOutfitSystem.cpp
 * @brief Outfits (clans/guilds): server-authoritative registry over
 *        TFOutfitStore + wire ops + client mirror/tag map. See the header for
 *        the full design note.
 */
#include "Game/TFOutfitSystem.h"

#include "Game/TFPlayerSystem.h"      // FactionOf — the kill-credit team filter
#include "Game/TFProgressionSystem.h" // kXPReasonCapture* (score hooks)
#include "Net/TFClientNet.h"
#include "Net/TFServerSim.h"
#include "Persistence/TFDatabase.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"
#include "World/TFAlertSystem.h" // kXPReasonAlert (alert-win score hook)

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>

namespace Terrafront
{

    namespace
    {

        constexpr float kSweepPeriodSec = 2.0f; // departed-player fallback sweep cadence
        constexpr float kResultShowSec = 6.0f;  // UI status-line lifetime (panel reads resultAge)

        int64_t NowMs()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        }

        bool EqualsNoCase(const std::string& a, const std::string& b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            }
            return true;
        }

        void CopyField(char* dst, size_t dstSize, const std::string& src)
        {
            std::strncpy(dst, src.c_str(), dstSize - 1);
            dst[dstSize - 1] = '\0';
        }

        std::string FieldToString(const char* field, size_t fieldSize)
        {
            return std::string(field, strnlen(field, fieldSize));
        }

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

    void TFOutfitSystem::Shutdown()
    {
        if (!m_initialized)
            return;

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

        m_store.Close(); // flushes if dirty
        m_boundChars.clear();
        m_playerOfChar.clear();
        m_invites.clear();
        m_tags.clear();
        m_mirror = Mirror{};
        m_lb = LeaderboardMirror{};
        m_lastWeekKey = 0; // a re-Initialize must re-run the load-time rollover sweep
        m_initialized = false;
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
    // Server: op dispatch
    // ---------------------------------------------------------------------------

    bool TFOutfitSystem::EnsureStoreOpen()
    {
        if (!m_ctx || !m_ctx->IsAuthority())
            return false;
        if (m_store.IsOpen())
            return true;
        if (m_storeOpenFailed)
            return false; // quarantined/corrupt file: do not retry (and re-quarantine) every op
        // Same working-dir-relative resolution as TFServerSim::
        // EnsureAuthorityDatabaseOpen's "Saves/terrafront.db" — outfits.json
        // lands next to the account database.
        if (m_store.Open("Saves/outfits.json"))
        {
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] authority opened outfit store (%zu outfits)",
                           m_store.OutfitCount());
            RolloverIfNeeded(); // server-load weekly rollover path (shared with the Update tick)
            return true;
        }
        SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] authority failed to open Saves/outfits.json");
        m_storeOpenFailed = true;
        return false;
    }

    const TFOutfitSystem::BoundChar* TFOutfitSystem::BoundCharOf(PlayerId player) const
    {
        const auto it = m_boundChars.find(player);
        return it != m_boundChars.end() ? &it->second : nullptr;
    }

    PlayerId TFOutfitSystem::OnlinePlayerOfChar(uint64_t charId) const
    {
        const auto it = m_playerOfChar.find(charId);
        return it != m_playerOfChar.end() ? it->second : kInvalidPlayer;
    }

    PlayerId TFOutfitSystem::FindOnlinePlayerByCharName(const std::string& name) const
    {
        for (const auto& [player, bc] : m_boundChars)
            if (EqualsNoCase(bc.name, name))
                return player;
        return kInvalidPlayer;
    }

    void TFOutfitSystem::ServerHandleOutfitMsgRaw(PlayerId sender, const void* data, size_t size)
    {
        if (size != sizeof(TF_OutfitRequest) || sender == kInvalidPlayer || !data)
        {
            ++m_badPackets;
            return;
        }
        TF_OutfitRequest req;
        std::memcpy(&req, data, sizeof(req));
        req.name[sizeof(req.name) - 1] = '\0';
        req.tag[sizeof(req.tag) - 1] = '\0';
        ServerHandleOutfitMsg(sender, req);
    }

    void TFOutfitSystem::ServerHandleOutfitMsg(PlayerId sender, const TF_OutfitRequest& req)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority())
            return;

        const TFOutfitOp op = static_cast<TFOutfitOp>(req.op);
        uint32_t outfitId = 0;
        TFOutfitResult result = TFOutfitResult::ServerError;

        if (!EnsureStoreOpen())
        {
            SendReply(sender, op, TFOutfitResult::ServerError, 0);
            return;
        }

        const std::string name = FieldToString(req.name, sizeof(req.name));
        const std::string tag = FieldToString(req.tag, sizeof(req.tag));

        switch (op)
        {
        case TFOutfitOp::Create:
            result = ServerCreate(sender, name, tag, outfitId);
            break;
        case TFOutfitOp::Invite:
            result = ServerInvite(sender, name, outfitId);
            break;
        case TFOutfitOp::Accept:
            result = ServerAccept(sender, outfitId);
            break;
        case TFOutfitOp::Decline:
            result = ServerDecline(sender);
            break;
        case TFOutfitOp::Leave:
            result = ServerLeave(sender, outfitId);
            break;
        case TFOutfitOp::Kick:
            result = ServerKick(sender, req.targetCharId, outfitId);
            break;
        case TFOutfitOp::SetRank:
            result = ServerSetRank(sender, req.targetCharId, static_cast<TFOutfitRank>(req.rank), outfitId);
            break;
        case TFOutfitOp::Disband:
            result = ServerDisband(sender, outfitId);
            break;
        case TFOutfitOp::Leaderboard:
            // The snapshot IS the reply — no TF_OutfitReply (it would flash the
            // panel's op-status line on every silent 60 s refresh).
            ServerSendLeaderboard(sender);
            return;
        default:
            ++m_badPackets;
            result = TFOutfitResult::BadRequest;
            break;
        }

        SendReply(sender, op, result, outfitId);
        if (result != TFOutfitResult::Ok)
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] outfit op %u from player %u rejected: %s",
                           static_cast<unsigned>(op), sender, OutfitResultText(result));
    }

    TFOutfitResult TFOutfitSystem::ServerCreate(PlayerId sender, const std::string& name, const std::string& tag,
                                                uint32_t& outOutfitId)
    {
        const BoundChar* bc = BoundCharOf(sender);
        if (!bc)
            return TFOutfitResult::ServerError;
        if (m_store.FindByCharacter(bc->charId))
            return TFOutfitResult::AlreadyInOutfit;
        if (!ValidateOutfitName(name))
            return TFOutfitResult::NameInvalid;
        if (!ValidateOutfitTag(tag))
            return TFOutfitResult::TagInvalid;
        if (m_store.FindByName(name))
            return TFOutfitResult::NameTaken;
        if (m_store.FindByTag(tag))
            return TFOutfitResult::TagTaken;

        const TFOutfitRecord* rec = m_store.Create(name, tag, bc->charId, bc->name, NowMs());
        if (!rec)
            return TFOutfitResult::ServerError;

        outOutfitId = rec->id;
        m_invites.erase(bc->charId); // creating your own outfit voids a pending invite

        SendRosterTo(sender, rec);
        BroadcastTag(sender, rec->tag);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] outfit '%s' [%s] created by character %llu", rec->name.c_str(),
                       rec->tag.c_str(), static_cast<unsigned long long>(bc->charId));
        return TFOutfitResult::Ok;
    }

    TFOutfitResult TFOutfitSystem::ServerInvite(PlayerId sender, const std::string& targetName, uint32_t& outOutfitId)
    {
        const BoundChar* bc = BoundCharOf(sender);
        if (!bc)
            return TFOutfitResult::ServerError;
        const TFOutfitRecord* rec = m_store.FindByCharacter(bc->charId);
        if (!rec)
            return TFOutfitResult::NotInOutfit;
        outOutfitId = rec->id;

        const TFOutfitMemberRecord* me = rec->FindMember(bc->charId);
        if (!me || me->rank == TFOutfitRank::Member)
            return TFOutfitResult::NotPermitted; // Officer+ may invite
        if (rec->members.size() >= kTFMaxOutfitMembers)
            return TFOutfitResult::OutfitFull;
        if (targetName.empty())
            return TFOutfitResult::BadRequest;

        const PlayerId targetPlayer = FindOnlinePlayerByCharName(targetName);
        if (targetPlayer == kInvalidPlayer || targetPlayer == sender)
            return TFOutfitResult::NoSuchPlayer;
        const BoundChar* targetBc = BoundCharOf(targetPlayer);
        if (!targetBc)
            return TFOutfitResult::NoSuchPlayer;
        if (m_store.FindByCharacter(targetBc->charId))
            return TFOutfitResult::TargetInOutfit;

        m_invites[targetBc->charId] = rec->id; // latest invite wins (one pending per character)

        TF_OutfitInvite inv{};
        inv.outfitId = rec->id;
        CopyField(inv.name, sizeof(inv.name), rec->name);
        CopyField(inv.tag, sizeof(inv.tag), rec->tag);
        CopyField(inv.inviter, sizeof(inv.inviter), bc->name);
        SendWireTo(targetPlayer, kTFMsgOutfitInvite, &inv, sizeof(inv));
        return TFOutfitResult::Ok;
    }

    TFOutfitResult TFOutfitSystem::ServerAccept(PlayerId sender, uint32_t& outOutfitId)
    {
        const BoundChar* bc = BoundCharOf(sender);
        if (!bc)
            return TFOutfitResult::ServerError;
        if (m_store.FindByCharacter(bc->charId))
        {
            m_invites.erase(bc->charId);
            return TFOutfitResult::AlreadyInOutfit;
        }
        const auto invIt = m_invites.find(bc->charId);
        if (invIt == m_invites.end())
            return TFOutfitResult::NoInvite;
        const uint32_t outfitId = invIt->second;

        const TFOutfitRecord* rec = m_store.FindById(outfitId);
        if (!rec)
        {
            m_invites.erase(invIt);
            return TFOutfitResult::NoInvite; // outfit disbanded since the invite
        }
        if (rec->members.size() >= kTFMaxOutfitMembers)
            return TFOutfitResult::OutfitFull;

        if (!m_store.AddMember(outfitId, bc->charId, bc->name, TFOutfitRank::Member, NowMs()))
            return TFOutfitResult::ServerError;
        m_invites.erase(bc->charId);
        outOutfitId = outfitId;

        rec = m_store.FindById(outfitId); // re-fetch: mutation may have re-seated storage
        if (rec)
            BroadcastRoster(*rec);
        BroadcastTag(sender, rec ? rec->tag : std::string{});
        return TFOutfitResult::Ok;
    }

    TFOutfitResult TFOutfitSystem::ServerDecline(PlayerId sender)
    {
        const BoundChar* bc = BoundCharOf(sender);
        if (!bc)
            return TFOutfitResult::ServerError;
        if (m_invites.erase(bc->charId) == 0)
            return TFOutfitResult::NoInvite;
        return TFOutfitResult::Ok;
    }

    TFOutfitResult TFOutfitSystem::ServerLeave(PlayerId sender, uint32_t& outOutfitId)
    {
        const BoundChar* bc = BoundCharOf(sender);
        if (!bc)
            return TFOutfitResult::ServerError;
        const TFOutfitRecord* rec = m_store.FindByCharacter(bc->charId);
        if (!rec)
            return TFOutfitResult::NotInOutfit;
        const uint32_t outfitId = rec->id;
        outOutfitId = outfitId;
        const TFOutfitMemberRecord* me = rec->FindMember(bc->charId);
        const bool wasLeader = me && me->rank == TFOutfitRank::Leader;

        if (!m_store.RemoveMember(outfitId, bc->charId))
            return TFOutfitResult::ServerError;
        ServerRemoveMembership(outfitId, bc->charId, sender);

        if (wasLeader)
            ServerAfterLeaderChange(outfitId); // may disband-on-empty
        if (const TFOutfitRecord* remaining = m_store.FindById(outfitId))
            BroadcastRoster(*remaining);
        return TFOutfitResult::Ok;
    }

    TFOutfitResult TFOutfitSystem::ServerKick(PlayerId sender, uint64_t targetCharId, uint32_t& outOutfitId)
    {
        const BoundChar* bc = BoundCharOf(sender);
        if (!bc)
            return TFOutfitResult::ServerError;
        const TFOutfitRecord* rec = m_store.FindByCharacter(bc->charId);
        if (!rec)
            return TFOutfitResult::NotInOutfit;
        outOutfitId = rec->id;

        if (targetCharId == 0 || targetCharId == bc->charId)
            return TFOutfitResult::BadRequest; // leaving is not a kick
        const TFOutfitMemberRecord* me = rec->FindMember(bc->charId);
        const TFOutfitMemberRecord* target = rec->FindMember(targetCharId);
        if (!me)
            return TFOutfitResult::ServerError;
        if (!target)
            return TFOutfitResult::NoSuchMember;

        // Leader kicks anyone below Leader; Officer kicks Members only.
        const bool permitted = (me->rank == TFOutfitRank::Leader && target->rank != TFOutfitRank::Leader) ||
                               (me->rank == TFOutfitRank::Officer && target->rank == TFOutfitRank::Member);
        if (!permitted)
            return TFOutfitResult::NotPermitted;

        const uint32_t outfitId = rec->id;
        if (!m_store.RemoveMember(outfitId, targetCharId))
            return TFOutfitResult::ServerError;
        ServerRemoveMembership(outfitId, targetCharId, OnlinePlayerOfChar(targetCharId));

        if (const TFOutfitRecord* remaining = m_store.FindById(outfitId))
            BroadcastRoster(*remaining);
        return TFOutfitResult::Ok;
    }

    TFOutfitResult TFOutfitSystem::ServerSetRank(PlayerId sender, uint64_t targetCharId, TFOutfitRank rank,
                                                 uint32_t& outOutfitId)
    {
        const BoundChar* bc = BoundCharOf(sender);
        if (!bc)
            return TFOutfitResult::ServerError;
        const TFOutfitRecord* rec = m_store.FindByCharacter(bc->charId);
        if (!rec)
            return TFOutfitResult::NotInOutfit;
        outOutfitId = rec->id;

        const TFOutfitMemberRecord* me = rec->FindMember(bc->charId);
        if (!me || me->rank != TFOutfitRank::Leader)
            return TFOutfitResult::NotPermitted; // rank changes are Leader-only
        const TFOutfitMemberRecord* target = rec->FindMember(targetCharId);
        if (!target)
            return TFOutfitResult::NoSuchMember;
        if (targetCharId == bc->charId)
            return TFOutfitResult::BadRequest; // no self-rank ops (transfer names a target)
        if (rank > TFOutfitRank::Leader)
            return TFOutfitResult::BadRequest;

        const uint32_t outfitId = rec->id;
        if (rank == TFOutfitRank::Leader)
        {
            // Leadership transfer: exactly one Leader — old leader steps to Officer.
            if (!m_store.SetMemberRank(outfitId, bc->charId, TFOutfitRank::Officer) ||
                !m_store.SetMemberRank(outfitId, targetCharId, TFOutfitRank::Leader))
                return TFOutfitResult::ServerError;
        }
        else
        {
            if (!m_store.SetMemberRank(outfitId, targetCharId, rank))
                return TFOutfitResult::ServerError;
        }

        if (const TFOutfitRecord* updated = m_store.FindById(outfitId))
            BroadcastRoster(*updated);
        return TFOutfitResult::Ok;
    }

    TFOutfitResult TFOutfitSystem::ServerDisband(PlayerId sender, uint32_t& outOutfitId)
    {
        const BoundChar* bc = BoundCharOf(sender);
        if (!bc)
            return TFOutfitResult::ServerError;
        const TFOutfitRecord* rec = m_store.FindByCharacter(bc->charId);
        if (!rec)
            return TFOutfitResult::NotInOutfit;
        const TFOutfitMemberRecord* me = rec->FindMember(bc->charId);
        if (!me || me->rank != TFOutfitRank::Leader)
            return TFOutfitResult::NotPermitted;

        const uint32_t outfitId = rec->id;
        outOutfitId = outfitId;

        // Clear every online member's mirror + tag BEFORE the record vanishes.
        for (const TFOutfitMemberRecord& m : rec->members)
        {
            const PlayerId p = OnlinePlayerOfChar(m.charId);
            if (p == kInvalidPlayer)
                continue;
            SendRosterTo(p, nullptr);
            BroadcastTag(p, std::string{});
        }
        // Void pending invites into the dead outfit.
        for (auto it = m_invites.begin(); it != m_invites.end();)
        {
            if (it->second == outfitId)
                it = m_invites.erase(it);
            else
                ++it;
        }

        if (!m_store.Disband(outfitId))
            return TFOutfitResult::ServerError;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] outfit %u disbanded by character %llu", outfitId,
                       static_cast<unsigned long long>(bc->charId));
        return TFOutfitResult::Ok;
    }

    void TFOutfitSystem::ServerRemoveMembership(uint32_t /*outfitId*/, uint64_t /*charId*/, PlayerId onlinePlayer)
    {
        if (onlinePlayer == kInvalidPlayer)
            return;
        SendRosterTo(onlinePlayer, nullptr);       // clears the mirror
        BroadcastTag(onlinePlayer, std::string{}); // clears the nameplate tag everywhere
    }

    void TFOutfitSystem::ServerAfterLeaderChange(uint32_t outfitId)
    {
        const TFOutfitRecord* rec = m_store.FindById(outfitId);
        if (!rec)
            return;
        if (rec->members.empty())
        {
            m_store.Disband(outfitId);
            for (auto it = m_invites.begin(); it != m_invites.end();)
            {
                if (it->second == outfitId)
                    it = m_invites.erase(it);
                else
                    ++it;
            }
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] outfit %u disbanded (last member left)", outfitId);
            return;
        }
        if (rec->Leader())
            return; // still has a leader

        // Auto-promote: senior Officer (earliest joinedAtMs), else senior Member.
        const TFOutfitMemberRecord* heir = nullptr;
        for (const TFOutfitMemberRecord& m : rec->members)
        {
            if (!heir)
            {
                heir = &m;
                continue;
            }
            const bool mBetter = (m.rank > heir->rank) || (m.rank == heir->rank && m.joinedAtMs < heir->joinedAtMs);
            if (mBetter)
                heir = &m;
        }
        if (heir)
        {
            m_store.SetMemberRank(outfitId, heir->charId, TFOutfitRank::Leader);
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] outfit %u leadership auto-passed to character %llu",
                           outfitId, static_cast<unsigned long long>(heir->charId));
        }
    }

    // ---------------------------------------------------------------------------
    // Server: competition score aggregation + leaderboard (W12)
    // ---------------------------------------------------------------------------

    void TFOutfitSystem::RolloverIfNeeded()
    {
        if (!m_store.IsOpen())
            return;
        const uint32_t wk = TFOutfitISOWeekKey(NowMs());
        if (wk == m_lastWeekKey)
            return; // still inside the week RolloverWeek last stamped
        const size_t stamped = m_store.RolloverWeek(wk);
        if (stamped > 0)
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] outfit weekly scores rolled to ISO week %u (%zu outfits)",
                           wk, stamped);
        m_lastWeekKey = wk;
    }

    void TFOutfitSystem::ServerAddOutfitScore(PlayerId player, uint32_t points)
    {
        if (player == kInvalidPlayer || points == 0)
            return;
        const BoundChar* bc = BoundCharOf(player);
        if (!bc)
            return; // bots / pre-onboarding sessions never score
        if (!EnsureStoreOpen())
            return;
        const TFOutfitRecord* rec = m_store.FindByCharacter(bc->charId);
        if (!rec)
            return; // unaffiliated player
        if (m_store.AddScore(rec->id, points, TFOutfitISOWeekKey(NowMs())))
            ++m_scoreEvents;
    }

    void TFOutfitSystem::OnPlayerKilledScore(const EvPlayerKilled& ev)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority())
            return;
        // Same credit filter as TFMedalSystem/TFProgressionSystem: no score for
        // environment deaths, suicides, or team kills.
        if (ev.killer == kInvalidPlayer || ev.killer == ev.victim)
            return;
        if (m_ctx->players)
        {
            const FactionId killerF = m_ctx->players->FactionOf(ev.killer);
            const FactionId victimF = m_ctx->players->FactionOf(ev.victim);
            if (killerF != FactionId::None && killerF == victimF)
                return;
        }
        ServerAddOutfitScore(ev.killer, kTFOutfitScorePerKill);
    }

    void TFOutfitSystem::OnXPAwardedScore(const EvXPAwarded& ev)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority())
            return;
        // Capture participation (the canonical reason codes 4/5/6 —
        // TFMedalSystem/TFAlertSystem precedent) and alert wins (13 — the
        // per-winning-participant payout TFAlertSystem routes through
        // ServerAwardXP). Everything else is ignored.
        if (ev.reason == kXPReasonCaptureFacility || ev.reason == kXPReasonCaptureFort ||
            ev.reason == kXPReasonCaptureOutpost)
            ServerAddOutfitScore(ev.player, kTFOutfitScorePerCapture);
        else if (ev.reason == kXPReasonAlert)
            ServerAddOutfitScore(ev.player, kTFOutfitScorePerAlertWin);
    }

    void TFOutfitSystem::ServerSendLeaderboard(PlayerId requester)
    {
        RolloverIfNeeded(); // the weekly column is always current-week truth

        // Rank every outfit: weekly desc, all-time desc, then name (stable,
        // deterministic order for equal scores).
        std::vector<const TFOutfitRecord*> ranked;
        ranked.reserve(m_store.OutfitCount());
        for (const TFOutfitRecord& o : m_store.All())
            ranked.push_back(&o);
        std::sort(ranked.begin(), ranked.end(),
                  [](const TFOutfitRecord* a, const TFOutfitRecord* b)
                  {
                      if (a->weeklyScore != b->weeklyScore)
                          return a->weeklyScore > b->weeklyScore;
                      if (a->allTimeScore != b->allTimeScore)
                          return a->allTimeScore > b->allTimeScore;
                      return a->name < b->name;
                  });

        uint32_t yourOutfitId = 0;
        if (const BoundChar* bc = BoundCharOf(requester))
            if (const TFOutfitRecord* mine = m_store.FindByCharacter(bc->charId))
                yourOutfitId = mine->id;

        TF_OutfitLeaderboard lb{};
        lb.totalOutfits = static_cast<uint16_t>(std::min<size_t>(ranked.size(), 0xFFFF));
        lb.weekKey = TFOutfitISOWeekKey(NowMs());
        lb.yourIndex = 0xFF;

        auto fillRow = [](TF_OutfitLbRow& row, const TFOutfitRecord& rec, size_t rank)
        {
            row.outfitId = rec.id;
            row.rank = static_cast<uint32_t>(rank);
            row.weekly = rec.weeklyScore;
            row.allTime = rec.allTimeScore;
            CopyField(row.name, sizeof(row.name), rec.name);
            CopyField(row.tag, sizeof(row.tag), rec.tag);
        };

        const size_t topN = std::min<size_t>(ranked.size(), kTFOutfitLbTopN);
        for (size_t i = 0; i < topN; ++i)
        {
            fillRow(lb.rows[lb.count], *ranked[i], i + 1);
            if (ranked[i]->id == yourOutfitId)
                lb.yourIndex = lb.count;
            ++lb.count;
        }
        // Requester's outfit outside the top N: append its row at its TRUE rank
        // so the panel can highlight it either way.
        if (yourOutfitId != 0 && lb.yourIndex == 0xFF)
        {
            for (size_t i = topN; i < ranked.size(); ++i)
            {
                if (ranked[i]->id != yourOutfitId)
                    continue;
                fillRow(lb.rows[lb.count], *ranked[i], i + 1);
                lb.yourIndex = lb.count;
                ++lb.count;
                break;
            }
        }

        SendWireTo(requester, kTFMsgOutfitLeaderboard, &lb, sizeof(lb));
    }

    // ---------------------------------------------------------------------------
    // Server: wire send
    // ---------------------------------------------------------------------------

    void TFOutfitSystem::SendReply(PlayerId player, TFOutfitOp op, TFOutfitResult result, uint32_t outfitId)
    {
        TF_OutfitReply rep{};
        rep.op = static_cast<uint8_t>(op);
        rep.result = static_cast<uint8_t>(result);
        rep.outfitId = outfitId;
        SendWireTo(player, kTFMsgOutfitReply, &rep, sizeof(rep));
    }

    void TFOutfitSystem::SendRosterTo(PlayerId player, const TFOutfitRecord* outfit)
    {
        if (player == kInvalidPlayer)
            return;

        if (!outfit)
        {
            TF_OutfitRoster cleared{};
            cleared.outfitId = 0;
            SendWireTo(player, kTFMsgOutfitRoster, &cleared, sizeof(cleared));
            return;
        }

        const BoundChar* bc = BoundCharOf(player);
        const uint64_t yourCharId = bc ? bc->charId : 0;
        const size_t total = outfit->members.size();
        const size_t chunks = std::max<size_t>(1, (total + kTFOutfitRosterChunk - 1) / kTFOutfitRosterChunk);

        for (size_t chunk = 0; chunk < chunks; ++chunk)
        {
            TF_OutfitRoster r{};
            r.outfitId = outfit->id;
            r.yourCharId = yourCharId;
            CopyField(r.name, sizeof(r.name), outfit->name);
            CopyField(r.tag, sizeof(r.tag), outfit->tag);
            r.totalMembers = static_cast<uint16_t>(std::min<size_t>(total, 0xFFFF));
            r.chunkIndex = static_cast<uint8_t>(chunk);

            const size_t base = chunk * kTFOutfitRosterChunk;
            const size_t n = std::min<size_t>(kTFOutfitRosterChunk, total - std::min(total, base));
            r.count = static_cast<uint8_t>(n);
            for (size_t i = 0; i < n; ++i)
            {
                const TFOutfitMemberRecord& m = outfit->members[base + i];
                TF_OutfitMemberBrief& b = r.members[i];
                b.charId = m.charId;
                CopyField(b.name, sizeof(b.name), m.name);
                b.rank = static_cast<uint8_t>(m.rank);
                b.online = OnlinePlayerOfChar(m.charId) != kInvalidPlayer ? 1 : 0;
            }
            SendWireTo(player, kTFMsgOutfitRoster, &r, sizeof(r));
        }
    }

    void TFOutfitSystem::BroadcastRoster(const TFOutfitRecord& outfit)
    {
        for (const TFOutfitMemberRecord& m : outfit.members)
        {
            const PlayerId p = OnlinePlayerOfChar(m.charId);
            if (p != kInvalidPlayer)
                SendRosterTo(p, &outfit);
        }
    }

    void TFOutfitSystem::BroadcastTag(PlayerId player, const std::string& tag)
    {
        TF_OutfitTagUpdate upd{};
        upd.player = player;
        CopyField(upd.tag, sizeof(upd.tag), tag);
        for (const auto& [p, bc] : m_boundChars)
            SendWireTo(p, kTFMsgOutfitTagUpdate, &upd, sizeof(upd));
    }

    void TFOutfitSystem::SendTagTableTo(PlayerId player)
    {
        for (const auto& [p, bc] : m_boundChars)
        {
            const TFOutfitRecord* rec = m_store.FindByCharacter(bc.charId);
            if (!rec)
                continue;
            TF_OutfitTagUpdate upd{};
            upd.player = p;
            CopyField(upd.tag, sizeof(upd.tag), rec->tag);
            SendWireTo(player, kTFMsgOutfitTagUpdate, &upd, sizeof(upd));
        }
    }

    void TFOutfitSystem::SendWireTo(PlayerId player, uint16_t msgId, const void* payload, size_t size)
    {
        if (player == kInvalidPlayer)
            return;
        // Listen host / standalone: the local player is not a network client —
        // feed the mirror directly (TFSquadSystem::SendEchoTo pattern).
        if (m_ctx->HasLocalPlayer() && player == m_ctx->localPlayer && m_ctx->role != NetRole::Client)
        {
            ClientHandleWire(msgId, payload, size);
            return;
        }
#ifdef ENABLE_NETWORKING
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized())
            return;
        Spark::Net::NetworkMessage msg;
        msg.type = static_cast<Spark::Net::MessageType>(msgId);
        msg.channel = Spark::Net::ChannelType::Reliable;
        msg.payload.resize(size);
        std::memcpy(msg.payload.data(), payload, size);
        nm.SendToClient(player, msg);
#endif
    }

    // ---------------------------------------------------------------------------
    // Client: requests
    // ---------------------------------------------------------------------------

    void TFOutfitSystem::SendRequest(const TF_OutfitRequest& req)
    {
        if (!m_initialized || !m_ctx || m_ctx->localPlayer == kInvalidPlayer)
            return;

        if (m_ctx->IsAuthority())
        {
            // Mirror the RouteClientMessage enter-world gate for the direct
            // authority path (TFSquadSystem::SendOp defense-in-depth pattern).
#ifdef ENABLE_NETWORKING
            if (!m_ctx->serverSim || !m_ctx->serverSim->IsEnteredWorld(m_ctx->localPlayer))
                return;
#endif
            ServerHandleOutfitMsg(m_ctx->localPlayer, req);
        }
        else if (m_ctx->clientNet)
        {
            m_ctx->clientNet->SendMsg(static_cast<TFMsg>(kTFMsgOutfitRequest), &req, sizeof(req));
        }
    }

    void TFOutfitSystem::ClientRequestCreate(const std::string& name, const std::string& tag)
    {
        TF_OutfitRequest req{};
        req.op = static_cast<uint8_t>(TFOutfitOp::Create);
        CopyField(req.name, sizeof(req.name), name);
        CopyField(req.tag, sizeof(req.tag), tag);
        SendRequest(req);
    }

    void TFOutfitSystem::ClientRequestInvite(const std::string& characterName)
    {
        TF_OutfitRequest req{};
        req.op = static_cast<uint8_t>(TFOutfitOp::Invite);
        CopyField(req.name, sizeof(req.name), characterName);
        SendRequest(req);
    }

    void TFOutfitSystem::ClientRequestAccept()
    {
        TF_OutfitRequest req{};
        req.op = static_cast<uint8_t>(TFOutfitOp::Accept);
        SendRequest(req);
    }

    void TFOutfitSystem::ClientRequestDecline()
    {
        TF_OutfitRequest req{};
        req.op = static_cast<uint8_t>(TFOutfitOp::Decline);
        SendRequest(req);
        // Optimistic local clear: the reply only confirms.
        m_mirror.hasInvite = false;
    }

    void TFOutfitSystem::ClientRequestLeave()
    {
        TF_OutfitRequest req{};
        req.op = static_cast<uint8_t>(TFOutfitOp::Leave);
        SendRequest(req);
    }

    void TFOutfitSystem::ClientRequestKick(uint64_t targetCharId)
    {
        TF_OutfitRequest req{};
        req.op = static_cast<uint8_t>(TFOutfitOp::Kick);
        req.targetCharId = targetCharId;
        SendRequest(req);
    }

    void TFOutfitSystem::ClientRequestSetRank(uint64_t targetCharId, TFOutfitRank rank)
    {
        TF_OutfitRequest req{};
        req.op = static_cast<uint8_t>(TFOutfitOp::SetRank);
        req.rank = static_cast<uint8_t>(rank);
        req.targetCharId = targetCharId;
        SendRequest(req);
    }

    void TFOutfitSystem::ClientRequestDisband()
    {
        TF_OutfitRequest req{};
        req.op = static_cast<uint8_t>(TFOutfitOp::Disband);
        SendRequest(req);
    }

    void TFOutfitSystem::ClientRequestLeaderboard()
    {
        TF_OutfitRequest req{};
        req.op = static_cast<uint8_t>(TFOutfitOp::Leaderboard);
        SendRequest(req);
    }

    // ---------------------------------------------------------------------------
    // Client: mirror
    // ---------------------------------------------------------------------------

    void TFOutfitSystem::ClientHandleWire(uint16_t msgId, const void* data, size_t size)
    {
        if (!data)
            return;
        switch (msgId)
        {
        case kTFMsgOutfitReply:
        {
            if (size != sizeof(TF_OutfitReply))
                return;
            TF_OutfitReply rep;
            std::memcpy(&rep, data, sizeof(rep));
            ClientHandleReply(rep);
            break;
        }
        case kTFMsgOutfitRoster:
        {
            if (size != sizeof(TF_OutfitRoster))
                return;
            TF_OutfitRoster roster;
            std::memcpy(&roster, data, sizeof(roster));
            roster.name[sizeof(roster.name) - 1] = '\0';
            roster.tag[sizeof(roster.tag) - 1] = '\0';
            ClientHandleRoster(roster);
            break;
        }
        case kTFMsgOutfitTagUpdate:
        {
            if (size != sizeof(TF_OutfitTagUpdate))
                return;
            TF_OutfitTagUpdate upd;
            std::memcpy(&upd, data, sizeof(upd));
            upd.tag[sizeof(upd.tag) - 1] = '\0';
            ClientHandleTagUpdate(upd);
            break;
        }
        case kTFMsgOutfitInvite:
        {
            if (size != sizeof(TF_OutfitInvite))
                return;
            TF_OutfitInvite inv;
            std::memcpy(&inv, data, sizeof(inv));
            inv.name[sizeof(inv.name) - 1] = '\0';
            inv.tag[sizeof(inv.tag) - 1] = '\0';
            inv.inviter[sizeof(inv.inviter) - 1] = '\0';
            ClientHandleInvite(inv);
            break;
        }
        case kTFMsgOutfitLeaderboard:
        {
            if (size != sizeof(TF_OutfitLeaderboard))
                return;
            TF_OutfitLeaderboard lb;
            std::memcpy(&lb, data, sizeof(lb));
            for (TF_OutfitLbRow& row : lb.rows)
            {
                row.name[sizeof(row.name) - 1] = '\0';
                row.tag[sizeof(row.tag) - 1] = '\0';
            }
            ClientHandleLeaderboard(lb);
            break;
        }
        default:
            break;
        }
    }

    void TFOutfitSystem::ClientHandleReply(const TF_OutfitReply& rep)
    {
        NoteResult(static_cast<TFOutfitOp>(rep.op), static_cast<TFOutfitResult>(rep.result));
    }

    void TFOutfitSystem::ClientHandleRoster(const TF_OutfitRoster& roster)
    {
        if (roster.outfitId == 0)
        {
            // Left / kicked / disbanded: clear outfit fields, keep the tag map.
            m_mirror.outfitId = 0;
            m_mirror.yourCharId = 0;
            m_mirror.name.clear();
            m_mirror.tag.clear();
            m_mirror.totalMembers = 0;
            m_mirror.members.clear();
            return;
        }

        if (roster.chunkIndex == 0 || roster.outfitId != m_mirror.outfitId)
        {
            m_mirror.members.clear();
            m_mirror.outfitId = roster.outfitId;
            m_mirror.yourCharId = roster.yourCharId;
            m_mirror.name = FieldToString(roster.name, sizeof(roster.name));
            m_mirror.tag = FieldToString(roster.tag, sizeof(roster.tag));
            m_mirror.totalMembers = roster.totalMembers;
            m_mirror.hasInvite = false; // joining/being synced voids any stale invite UI
        }

        const uint8_t n = std::min<uint8_t>(roster.count, kTFOutfitRosterChunk);
        for (uint8_t i = 0; i < n; ++i)
        {
            const TF_OutfitMemberBrief& b = roster.members[i];
            MirrorMember m;
            m.charId = b.charId;
            m.name = FieldToString(b.name, sizeof(b.name));
            m.rank = static_cast<TFOutfitRank>(std::min<uint8_t>(b.rank, static_cast<uint8_t>(TFOutfitRank::Leader)));
            m.online = b.online != 0;
            m_mirror.members.push_back(std::move(m));
        }
    }

    void TFOutfitSystem::ClientHandleTagUpdate(const TF_OutfitTagUpdate& upd)
    {
        const std::string tag = FieldToString(upd.tag, sizeof(upd.tag));
        if (tag.empty())
            m_tags.erase(upd.player);
        else
            m_tags[upd.player] = tag;
    }

    void TFOutfitSystem::ClientHandleInvite(const TF_OutfitInvite& inv)
    {
        m_mirror.hasInvite = true;
        m_mirror.inviteOutfitId = inv.outfitId;
        m_mirror.inviteName = FieldToString(inv.name, sizeof(inv.name));
        m_mirror.inviteTag = FieldToString(inv.tag, sizeof(inv.tag));
        m_mirror.inviteFrom = FieldToString(inv.inviter, sizeof(inv.inviter));
    }

    void TFOutfitSystem::ClientHandleLeaderboard(const TF_OutfitLeaderboard& lb)
    {
        m_lb.rows.clear();
        m_lb.valid = true;
        m_lb.totalOutfits = lb.totalOutfits;
        m_lb.weekKey = lb.weekKey;
        const uint8_t n = std::min<uint8_t>(lb.count, kTFOutfitLbMaxRows);
        m_lb.yourIndex = lb.yourIndex < n ? static_cast<int>(lb.yourIndex) : -1;
        m_lb.rows.reserve(n);
        for (uint8_t i = 0; i < n; ++i)
        {
            const TF_OutfitLbRow& r = lb.rows[i];
            LbRow row;
            row.outfitId = r.outfitId;
            row.rank = r.rank;
            row.weekly = r.weekly;
            row.allTime = r.allTime;
            row.name = FieldToString(r.name, sizeof(r.name));
            row.tag = FieldToString(r.tag, sizeof(r.tag));
            m_lb.rows.push_back(std::move(row));
        }
    }

    void TFOutfitSystem::NoteResult(TFOutfitOp op, TFOutfitResult result)
    {
        m_mirror.hasResult = true;
        m_mirror.lastOp = op;
        m_mirror.lastResult = result;
        m_mirror.resultAge = 0.0f;
        if (result == TFOutfitResult::Ok && (op == TFOutfitOp::Accept || op == TFOutfitOp::Decline))
            m_mirror.hasInvite = false;
    }

    // ---------------------------------------------------------------------------
    // Client handlers (pure clients only; TFSquadSystem EnsureClientHandlers
    // pattern — the S->C ids in the reserved block are exclusively ours, so no
    // handler-slot contention with TFClientNet)
    // ---------------------------------------------------------------------------

#ifdef ENABLE_NETWORKING

    bool TFOutfitSystem::ClientNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return m_ctx->role == NetRole::Client && nm.IsInitialized() &&
               nm.GetRole() == Spark::Net::NetworkRole::Client &&
               nm.GetConnectionState() == Spark::Net::ConnectionState::Connected;
    }

    void TFOutfitSystem::EnsureClientHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();

        for (uint16_t id : {kTFMsgOutfitReply, kTFMsgOutfitRoster, kTFMsgOutfitTagUpdate, kTFMsgOutfitInvite,
                            kTFMsgOutfitLeaderboard})
        {
            nm.RegisterHandler(static_cast<MessageType>(id), [this, id](const NetworkMessage& m)
                               { ClientHandleWire(id, m.payload.data(), m.payload.size()); });
        }
        m_clientHandlers = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] outfit mirror handlers registered");
    }

    void TFOutfitSystem::ReleaseClientHandlers()
    {
        // No per-type removal in NetworkManager; overwrite with no-ops so no
        // dangling `this` survives module shutdown (TFServerSim pattern).
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        for (uint16_t id : {kTFMsgOutfitReply, kTFMsgOutfitRoster, kTFMsgOutfitTagUpdate, kTFMsgOutfitInvite,
                            kTFMsgOutfitLeaderboard})
            nm.RegisterHandler(static_cast<Spark::Net::MessageType>(id), [](const Spark::Net::NetworkMessage&) {});
        m_clientHandlers = false;
    }

#endif // ENABLE_NETWORKING

    // ---------------------------------------------------------------------------
    // Console commands (registered from Initialize — TFRegionSystem pattern; the
    // contended Console/TFCommands.cpp is never touched)
    // ---------------------------------------------------------------------------

    void TFOutfitSystem::RegisterConsoleCommands()
    {
        if (m_cmdsRegistered)
            return;
        auto& console = Spark::SimpleConsole::GetInstance();

        auto joinArgs = [](const std::vector<std::string>& args, size_t first, size_t lastExclusive) -> std::string
        {
            std::string out;
            for (size_t i = first; i < lastExclusive && i < args.size(); ++i)
            {
                if (!out.empty())
                    out += ' ';
                out += args[i];
            }
            return out;
        };

        auto memberByName = [this](const std::string& name, uint64_t& outCharId) -> bool
        {
            for (const MirrorMember& m : m_mirror.members)
            {
                if (EqualsNoCase(m.name, name))
                {
                    outCharId = m.charId;
                    return true;
                }
            }
            return false;
        };

        console.RegisterCommand(
            "tf_outfit_create",
            [this, joinArgs](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 2)
                    return "usage: tf_outfit_create <name...> <tag>";
                const std::string tag = args.back();
                const std::string name = joinArgs(args, 0, args.size() - 1);
                if (!ValidateOutfitName(name))
                    return std::string("[TF] ") + OutfitResultText(TFOutfitResult::NameInvalid);
                if (!ValidateOutfitTag(tag))
                    return std::string("[TF] ") + OutfitResultText(TFOutfitResult::TagInvalid);
                ClientRequestCreate(name, tag);
                return "[TF] outfit create requested: '" + name + "' [" + tag + "]";
            },
            "Create an outfit: tf_outfit_create <name...> <tag>", "TERRAFRONT", "tf_outfit_create <name...> <tag>");

        console.RegisterCommand(
            "tf_outfit_invite",
            [this, joinArgs](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "usage: tf_outfit_invite <character name...>";
                const std::string target = joinArgs(args, 0, args.size());
                ClientRequestInvite(target);
                return "[TF] outfit invite requested for '" + target + "'";
            },
            "Invite an online character to your outfit (Officer+)", "TERRAFRONT",
            "tf_outfit_invite <character name...>");

        console.RegisterCommand(
            "tf_outfit_accept",
            [this](const std::vector<std::string>&) -> std::string
            {
                if (!m_mirror.hasInvite)
                    return "[TF] no pending outfit invite";
                ClientRequestAccept();
                return "[TF] accepting invite to '" + m_mirror.inviteName + "'";
            },
            "Accept the pending outfit invite", "TERRAFRONT", "tf_outfit_accept");

        console.RegisterCommand(
            "tf_outfit_decline",
            [this](const std::vector<std::string>&) -> std::string
            {
                ClientRequestDecline();
                return "[TF] outfit invite declined";
            },
            "Decline the pending outfit invite", "TERRAFRONT", "tf_outfit_decline");

        console.RegisterCommand(
            "tf_outfit_leave",
            [this](const std::vector<std::string>&) -> std::string
            {
                ClientRequestLeave();
                return "[TF] outfit leave requested";
            },
            "Leave your outfit (leadership auto-passes; empty outfits disband)", "TERRAFRONT", "tf_outfit_leave");

        console.RegisterCommand(
            "tf_outfit_kick",
            [this, joinArgs, memberByName](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                    return "usage: tf_outfit_kick <member name...>";
                const std::string target = joinArgs(args, 0, args.size());
                uint64_t charId = 0;
                if (!memberByName(target, charId))
                    return "[TF] '" + target + "' is not on your roster (open the outfit panel to sync)";
                ClientRequestKick(charId);
                return "[TF] kick requested for '" + target + "'";
            },
            "Kick an outfit member by name (Leader: anyone; Officer: members)", "TERRAFRONT",
            "tf_outfit_kick <member name...>");

        console.RegisterCommand(
            "tf_outfit_rank",
            [this, joinArgs, memberByName](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() < 2)
                    return "usage: tf_outfit_rank <member name...> <member|officer|leader>";
                const std::string rankStr = args.back();
                const std::string target = joinArgs(args, 0, args.size() - 1);
                TFOutfitRank rank;
                if (EqualsNoCase(rankStr, "member"))
                    rank = TFOutfitRank::Member;
                else if (EqualsNoCase(rankStr, "officer"))
                    rank = TFOutfitRank::Officer;
                else if (EqualsNoCase(rankStr, "leader"))
                    rank = TFOutfitRank::Leader;
                else
                    return "usage: tf_outfit_rank <member name...> <member|officer|leader>";
                uint64_t charId = 0;
                if (!memberByName(target, charId))
                    return "[TF] '" + target + "' is not on your roster";
                ClientRequestSetRank(charId, rank);
                return "[TF] rank change requested: '" + target + "' -> " + OutfitRankName(rank);
            },
            "Set an outfit member's rank (Leader only; 'leader' transfers leadership)", "TERRAFRONT",
            "tf_outfit_rank <member name...> <member|officer|leader>");

        console.RegisterCommand(
            "tf_outfit_disband",
            [this](const std::vector<std::string>&) -> std::string
            {
                ClientRequestDisband();
                return "[TF] outfit disband requested";
            },
            "Disband your outfit (Leader only)", "TERRAFRONT", "tf_outfit_disband");

        console.RegisterCommand(
            "tf_outfit_status",
            [this](const std::vector<std::string>&) -> std::string
            {
                std::string out;
                if (m_mirror.outfitId == 0)
                    out = "[TF] not in an outfit";
                else
                {
                    out = "[TF] outfit '" + m_mirror.name + "' [" + m_mirror.tag +
                          "] members=" + std::to_string(m_mirror.totalMembers) +
                          " yourRank=" + OutfitRankName(LocalRank());
                    for (const MirrorMember& m : m_mirror.members)
                        out += "\n  " + m.name + " (" + OutfitRankName(m.rank) + (m.online ? ", online)" : ")");
                }
                if (m_mirror.hasInvite)
                    out += "\n[TF] pending invite: '" + m_mirror.inviteName + "' [" + m_mirror.inviteTag + "] from " +
                           m_mirror.inviteFrom + " (tf_outfit_accept / tf_outfit_decline)";
                if (m_mirror.hasResult)
                    out += std::string("\n[TF] last op result: ") + OutfitResultText(m_mirror.lastResult);
                return out;
            },
            "Show your outfit roster, pending invite and last op result", "TERRAFRONT", "tf_outfit_status");

        console.RegisterCommand(
            "tf_outfit_leaderboard",
            [this](const std::vector<std::string>&) -> std::string
            {
                ClientRequestLeaderboard(); // refresh in flight; print what we have
                if (!m_lb.valid)
                    return "[TF] outfit leaderboard requested (re-run to see the snapshot)";
                std::string out = "[TF] outfit leaderboard (ISO week " + std::to_string(m_lb.weekKey) + ", " +
                                  std::to_string(m_lb.totalOutfits) + " outfits):";
                for (size_t i = 0; i < m_lb.rows.size(); ++i)
                {
                    const LbRow& r = m_lb.rows[i];
                    out += "\n  #" + std::to_string(r.rank) + " [" + r.tag + "] " + r.name +
                           "  weekly=" + std::to_string(r.weekly) + "  all-time=" + std::to_string(r.allTime);
                    if (static_cast<int>(i) == m_lb.yourIndex)
                        out += "  <- yours";
                }
                if (m_lb.rows.empty())
                    out += "\n  (no outfits yet)";
                return out;
            },
            "Fetch + print the outfit score leaderboard (kill +1, capture +10, alert win +100)", "TERRAFRONT",
            "tf_outfit_leaderboard");

        m_cmdsRegistered = true;
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

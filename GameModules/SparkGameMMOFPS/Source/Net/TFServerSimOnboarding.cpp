/**
 * @file TFServerSimOnboarding.cpp
 * @brief TFServerSim client-message routing choke point (the enter-world gate)
 *        and the W5 onboarding handlers: login / register / character CRUD /
 *        enter-world (same class, split per repo file-size rules — see
 *        TFServerSim.cpp).
 */
#include "Net/TFServerSim.h"
#include "Net/TFOnboardingSessionRules.h"

#include "Account/TFAccountSystem.h"   // W5 onboarding (Task 4)
#include "Account/TFCharacterSystem.h" // W5 onboarding (Task 4)
#include "Persistence/TFDatabase.h"
#include "Persistence/TFPlayerMeta.h" // TFLoadout
#include "Persistence/TFSavePaths.h"
#include "Net/TFNetProtocol.h"
#include "Data/TFDataTables.h"
#include "Game/TFProgressionSystem.h" // W6 progression: loadout persistence + unlock purchases
#include "Game/TFOutfitSystem.h"      // Outfits lane: OutfitRequest routing + session hooks
#include "Game/TFAbilitySystem.h"     // class-abilities lane (W9): AbilityRequest routing
#include "Game/TFGrenadeSystem.h"     // grenades lane (W10): GrenadeThrow routing
#include "Game/TFPingSystem.h"        // ping-system lane (W11): PingPlace routing
#include "Game/TFSquadSystem.h"
#include "Utils/LogMacros.h"
#include "Utils/ScopeGuard.h"
#include "Utils/SecureMemory.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <algorithm>
#include <cstring>
#include <string>

namespace Terrafront
{

#ifdef ENABLE_NETWORKING

    // ---------------------------------------------------------------------------
    // W5 onboarding (Task 4): login / register / char CRUD / enter-world.
    // ---------------------------------------------------------------------------

    void TFServerSim::RouteClientMessage(PlayerId sender, TFMsg id, const void* data, size_t size)
    {
        // W5 T6 (T4-review #1 security fix): CRITICAL security gate. Before this
        // fix, the enter-world gate only withheld TF_WorldWelcome — the gameplay
        // handlers themselves never verified the sender had actually logged in
        // and entered the world, so a modified client could send SpawnRequest/
        // ClientInput/FireEvent/FactionSelect directly and play as an
        // unauthenticated "ghost". Every client-originated gameplay message now
        // requires `sender` to already be in m_enteredWorld (set exactly once, by
        // a successful HandleEnterWorld below) — this is the SAME dispatcher both
        // the socket path (RegisterNetHandlers) and the listen-host/standalone
        // loopback path (TFClientNet::RouteLoopback) call through, so local play
        // is gated identically to networked play: the local host player
        // (kTFLocalHostPlayer) must complete login -> character select/create ->
        // enter-world via TFLoginFlow exactly like a networked client before it
        // can move, spawn, fire, or switch factions. Bot-driven spawns/inputs
        // (TFBotSystem) never call through here — they call
        // TFPlayerSystem::ServerHandleSpawnRequest / EnqueueInput /
        // TFWeaponSystem::ServerHandleFire directly — so bots are unaffected.
        // Onboarding ids (login/char CRUD/enter-world itself) are never gated
        // here: they are how a session GETS into m_enteredWorld in the first
        // place.
        switch (id)
        {
        case TFMsg::ClientInput:
        case TFMsg::SpawnRequest:
        case TFMsg::FireEvent:
        case TFMsg::FactionSelect:
        case TFMsg::VehicleEnter:
        case TFMsg::VehicleExit:
        case TFMsg::AegisDeploy:
        case TFMsg::SquadMsg:
        case TFMsg::ChatMsg:
        case TFMsg::LoadoutChange:
        case TFMsg::LoadoutExtChange: // loadout-depth wave: gated like the other gameplay ids
        case TFMsg::UnlockRequest:
        case TFMsg::RedeployRequest:     // W7 ui-map-keys: MUST be gated
        case TFMsg::OutfitRequest:       // Outfits lane: gated like the other gameplay ids
        case TFMsg::AbilityRequest:      // class-abilities lane (W9): gated
        case TFMsg::GrenadeThrow:        // grenades lane (W10): gated
        case TFMsg::PingPlace:           // ping-system lane (W11): gated
        case TFMsg::ContinentHopRequest: // multimap server-authoritative hop (W13): gated
            if (!m_enteredWorld.contains(sender))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Game,
                               "[TF] gameplay message 0x%04X from non-entered-world client %u rejected",
                               static_cast<unsigned>(id), sender);
                return;
            }
            break;
        default:
            break;
        }

        switch (id)
        {
        case TFMsg::LoginRequest:
            HandleLogin(sender, data, size);
            break;
        case TFMsg::RegisterRequest:
            HandleRegister(sender, data, size);
            break;
        case TFMsg::CharListRequest:
            HandleCharList(sender, data, size);
            break;
        case TFMsg::CharCreateReq:
            HandleCharCreate(sender, data, size);
            break;
        case TFMsg::CharDeleteReq:
            HandleCharDelete(sender, data, size);
            break;
        case TFMsg::EnterWorldReq:
            HandleEnterWorld(sender, data, size);
            break;
        case TFMsg::ClientInput:
            HandleClientInput(sender, data, size);
            break;
        case TFMsg::SpawnRequest:
            HandleSpawnRequest(sender, data, size);
            break;
        case TFMsg::FireEvent:
            HandleFireEvent(sender, data, size);
            break;
        case TFMsg::FactionSelect:
            HandleFactionSelect(sender, data, size);
            break;
        // W7 ui-map-keys: server-validated map redeploy.
        case TFMsg::RedeployRequest:
            HandleRedeployRequest(sender, data, size);
            break;
        // Outfits lane: TFOutfitSystem size-validates, applies rank policy and replies.
        case TFMsg::OutfitRequest:
            if (m_ctx->outfits)
                m_ctx->outfits->ServerHandleOutfitMsgRaw(sender, data, size);
            break;
        // class-abilities lane (W9): TFAbilitySystem size-validates and replies.
        case TFMsg::AbilityRequest:
            if (m_ctx->abilities)
                m_ctx->abilities->ServerHandleAbilityMsgRaw(sender, data, size);
            break;
        // grenades lane (W10): TFGrenadeSystem size-validates and simulates.
        case TFMsg::GrenadeThrow:
            if (m_ctx->grenades)
                m_ctx->grenades->ServerHandleThrowMsgRaw(sender, data, size);
            break;
        // ping-system lane (W11): TFPingSystem size-validates and rebroadcasts.
        case TFMsg::PingPlace:
            if (m_ctx->pings)
                m_ctx->pings->ServerHandlePingMsgRaw(sender, data, size);
            break;
        // final-review #3: vehicle/squad verbs, now gated the same as the
        // other gameplay ids above.
        case TFMsg::VehicleEnter:
            HandleVehicleSeatOp(sender, data, size, true);
            break;
        case TFMsg::VehicleExit:
            HandleVehicleSeatOp(sender, data, size, false);
            break;
        case TFMsg::AegisDeploy:
            HandleAegisDeploy(sender, data, size);
            break;
        case TFMsg::SquadMsg:
            if (m_ctx->squads)
                m_ctx->squads->ServerHandleSquadMsgRaw(sender, data, size);
            break;
        case TFMsg::ChatMsg:
            HandleChatMsg(sender, data, size);
            break;
        // W6 progression: persist the player's saved loadout preference
        // (validated inside ServerSetLoadout; false == rejected, no change).
        case TFMsg::LoadoutChange:
        {
            if (size < sizeof(TF_LoadoutChange) || !m_ctx->progression || !m_ctx->data || !m_ctx->data->IsLoaded())
                break;
            TF_LoadoutChange lc{};
            std::memcpy(&lc, data, sizeof(lc));
            auto keyOf = [&](uint16_t wid) -> std::string
            {
                if (wid == kInvalidWeapon)
                    return {};
                const WeaponDef* def = m_ctx->data->GetWeapon(static_cast<WeaponId>(wid));
                return def ? def->key : std::string{};
            };
            TFLoadout lo;
            lo.primary = keyOf(lc.primary);
            lo.secondary = keyOf(lc.secondary);
            lo.tool = keyOf(lc.tool);
            m_ctx->progression->ServerSetLoadout(sender, lo);
            break;
        }
        // loadout-depth wave: grenade + suit picks (size-validated inside).
        case TFMsg::LoadoutExtChange:
            if (m_ctx->progression)
                m_ctx->progression->ServerHandleLoadoutExtMsgRaw(sender, data, size);
            break;
        // W6 progression: unlock-tree purchase (Persistence/TFUnlockTree.h).
        case TFMsg::UnlockRequest:
        {
            if (size < sizeof(TF_UnlockRequest) || !m_ctx->progression)
                break;
            TF_UnlockRequest req{};
            std::memcpy(&req, data, sizeof(req));
            req.unlockKey[sizeof(req.unlockKey) - 1] = '\0';
            const TFUnlockResult r = m_ctx->progression->ServerTryUnlock(sender, req.unlockKey);
            TF_UnlockReply rep{};
            rep.result = static_cast<uint8_t>(r);
            std::memcpy(rep.unlockKey, req.unlockKey, sizeof(rep.unlockKey));
            SendToPlayer(sender, static_cast<uint16_t>(TFMsg::UnlockReply), &rep, sizeof(rep), true);
            break;
        }
        // W13 multimap server-authoritative continent-hop (docs/TERRAFRONT_
        // MULTIMAP.md §2.2).
        case TFMsg::ContinentHopRequest:
            HandleContinentHopRequest(sender, data, size);
            break;
        default:
            break;
        }
    }

    void TFServerSim::HandleLogin(PlayerId sender, const void* data, size_t size)
    {
        if (size != sizeof(TF_AuthRequest) || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }

        TF_AuthReply rep{};
        const bool authenticated = m_ctx->account && m_ctx->account->AccountForClient(sender) != 0;
        if (!CanBeginAuthentication(authenticated, m_enteredWorld.contains(sender)))
        {
            rep.err = static_cast<uint8_t>(TFAuthErr::SessionActive);
            SendToPlayer(sender, static_cast<uint16_t>(TFMsg::LoginReply), &rep, sizeof(rep), true);
            return;
        }

        TF_AuthRequest req;
        std::memcpy(&req, data, sizeof(req));
        const auto clearRequest = Spark::MakeScopeExit([&] { Spark::SecureErase(&req, sizeof(req)); });
        const std::string user(req.user, strnlen(req.user, sizeof(req.user)));
        std::string pass(req.pass, strnlen(req.pass, sizeof(req.pass)));
        const auto clearPassword = Spark::MakeScopeExit([&] { Spark::SecureClear(pass); });

        if (!m_ctx->account || !EnsureAuthorityDatabaseOpen())
        {
            rep.err = static_cast<uint8_t>(TFAuthErr::ServerError);
        }
        else
        {
            const TFAuthResult r = m_ctx->account->Login(user, pass);
            rep.ok = r.ok ? 1 : 0;
            rep.err = static_cast<uint8_t>(r.err);
            rep.accountId = r.accountId;
            if (r.ok)
                m_ctx->account->BindSession(sender, r.accountId);
        }
        SendToPlayer(sender, static_cast<uint16_t>(TFMsg::LoginReply), &rep, sizeof(rep), true);
    }

    void TFServerSim::HandleRegister(PlayerId sender, const void* data, size_t size)
    {
        if (size != sizeof(TF_AuthRequest) || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }

        TF_AuthReply rep{};
        const bool authenticated = m_ctx->account && m_ctx->account->AccountForClient(sender) != 0;
        if (!CanBeginAuthentication(authenticated, m_enteredWorld.contains(sender)))
        {
            rep.err = static_cast<uint8_t>(TFAuthErr::SessionActive);
            SendToPlayer(sender, static_cast<uint16_t>(TFMsg::RegisterReply), &rep, sizeof(rep), true);
            return;
        }

        TF_AuthRequest req;
        std::memcpy(&req, data, sizeof(req));
        const auto clearRequest = Spark::MakeScopeExit([&] { Spark::SecureErase(&req, sizeof(req)); });
        const std::string user(req.user, strnlen(req.user, sizeof(req.user)));
        std::string pass(req.pass, strnlen(req.pass, sizeof(req.pass)));
        const auto clearPassword = Spark::MakeScopeExit([&] { Spark::SecureClear(pass); });

        if (!m_ctx->account || !EnsureAuthorityDatabaseOpen())
        {
            rep.err = static_cast<uint8_t>(TFAuthErr::ServerError);
        }
        else
        {
            const TFAuthResult r = m_ctx->account->Register(user, pass);
            rep.ok = r.ok ? 1 : 0;
            rep.err = static_cast<uint8_t>(r.err);
            rep.accountId = r.accountId;
            // Registration does NOT auto-login; the client sends LoginRequest next
            // (mirrors MMOAccountSystem's register-then-login flow).
        }
        SendToPlayer(sender, static_cast<uint16_t>(TFMsg::RegisterReply), &rep, sizeof(rep), true);
    }

    bool TFServerSim::EnsureAuthorityDatabaseOpen()
    {
        if (!m_ctx || !m_ctx->IsAuthority() || !m_ctx->db)
            return false;
        if (m_ctx->db->IsOpen())
            return true;
        const std::filesystem::path path = SavePaths::File("terrafront.db");
        if (!path.empty() && m_ctx->db->Open(path))
        {
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] authority opened account database at %s",
                           SavePaths::Utf8ForLog(path).c_str());
            return true;
        }
        SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] authority failed to open account database at %s",
                        path.empty() ? "<invalid save path>" : SavePaths::Utf8ForLog(path).c_str());
        return false;
    }

    void TFServerSim::HandleCharList(PlayerId sender, const void* data, size_t size)
    {
        (void)data;
        if (size != 0 || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }
        TF_CharListReply rep{};
        if (m_ctx->account && m_ctx->characters)
        {
            const uint64_t acctId = m_ctx->account->AccountForClient(sender);
            if (acctId != 0)
            {
                const std::vector<TFCharacterRecord> list = m_ctx->characters->List(acctId);
                const uint8_t n = static_cast<uint8_t>(std::min<size_t>(list.size(), 5));
                rep.count = n;
                for (uint8_t i = 0; i < n; ++i)
                {
                    TF_CharBrief& b = rep.chars[i];
                    b.id = list[i].id;
                    std::strncpy(b.name, list[i].name.c_str(), sizeof(b.name) - 1);
                    b.name[sizeof(b.name) - 1] = '\0';
                    b.faction = static_cast<uint8_t>(list[i].faction);
                    b.rank = list[i].rank;
                }
            }
        }
        SendToPlayer(sender, static_cast<uint16_t>(TFMsg::CharListReply), &rep, sizeof(rep), true);
    }

    void TFServerSim::HandleCharCreate(PlayerId sender, const void* data, size_t size)
    {
        if (size != sizeof(TF_CharCreateRequest) || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }
        TF_CharCreateRequest req;
        std::memcpy(&req, data, sizeof(req));
        const std::string name(req.name, strnlen(req.name, sizeof(req.name)));

        TF_CharOpReply rep{};
        if (!CanMutateCharacterProfile(m_enteredWorld.contains(sender)))
        {
            rep.err = static_cast<uint8_t>(TFCharErr::SessionActive);
            SendToPlayer(sender, static_cast<uint16_t>(TFMsg::CharCreateReply), &rep, sizeof(rep), true);
            return;
        }
        if (!m_ctx->account || !m_ctx->characters)
        {
            rep.err = static_cast<uint8_t>(TFCharErr::ServerError);
        }
        else
        {
            const uint64_t acctId = m_ctx->account->AccountForClient(sender);
            if (acctId == 0)
            {
                rep.err = static_cast<uint8_t>(TFCharErr::NotLoggedIn);
            }
            else
            {
                const TFCharCreateResult r =
                    m_ctx->characters->Create(acctId, name, static_cast<FactionId>(req.faction));
                rep.ok = r.ok ? 1 : 0;
                rep.err = static_cast<uint8_t>(r.err);
                rep.charId = r.charId;
            }
        }
        SendToPlayer(sender, static_cast<uint16_t>(TFMsg::CharCreateReply), &rep, sizeof(rep), true);
    }

    void TFServerSim::HandleCharDelete(PlayerId sender, const void* data, size_t size)
    {
        if (size != sizeof(TF_CharDeleteRequest) || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }
        TF_CharDeleteRequest req;
        std::memcpy(&req, data, sizeof(req));

        TF_CharOpReply rep{};
        rep.charId = req.charId;
        if (!CanMutateCharacterProfile(m_enteredWorld.contains(sender)))
        {
            rep.err = static_cast<uint8_t>(TFCharErr::SessionActive);
            SendToPlayer(sender, static_cast<uint16_t>(TFMsg::CharDeleteReply), &rep, sizeof(rep), true);
            return;
        }
        if (!m_ctx->account || !m_ctx->characters)
        {
            rep.err = static_cast<uint8_t>(TFCharErr::ServerError);
        }
        else
        {
            const uint64_t acctId = m_ctx->account->AccountForClient(sender);
            if (acctId == 0)
            {
                rep.err = static_cast<uint8_t>(TFCharErr::NotLoggedIn);
            }
            else
            {
                const TFCharErr err = m_ctx->characters->Delete(acctId, req.charId);
                rep.ok = (err == TFCharErr::Ok) ? 1 : 0;
                rep.err = static_cast<uint8_t>(err);
            }
        }
        SendToPlayer(sender, static_cast<uint16_t>(TFMsg::CharDeleteReply), &rep, sizeof(rep), true);
    }

    void TFServerSim::HandleEnterWorld(PlayerId sender, const void* data, size_t size)
    {
        if (size != sizeof(TF_EnterWorldRequest) || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }
        TF_EnterWorldRequest req;
        std::memcpy(&req, data, sizeof(req));

        // Idempotency guard (mirrors HandleFactionSelect's alive-guard): a
        // duplicate EnterWorldReq, or one sent while a second character is being
        // entered mid-session, must NOT re-bind the session's faction out from
        // under an already-alive/already-entered pawn (DESIGN.md W5 "Error
        // handling": duplicate/already-in-world is idempotent, server ignores).
        if (m_move.contains(sender) || m_enteredWorld.contains(sender))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game,
                           "[TF] player %u sent duplicate/late EnterWorldReq while already in world — ignored", sender);
            return;
        }

        // Server-authoritative gate: the client cannot self-report auth/enter-world
        // state. Without a bound, logged-in session AND ownership-verified
        // character, TF_WorldWelcome is never sent — the client stays parked on
        // the login/char-select screen (fails silently; no reply message exists
        // for a rejected EnterWorldReq by design, mirroring how an unauthenticated
        // socket gets no gameplay traffic at all).
        if (!m_ctx->account || !m_ctx->characters)
            return; // T6 boot wiring not present yet — cannot authoritatively enter world

        const uint64_t acctId = m_ctx->account->AccountForClient(sender);
        if (acctId == 0)
            return; // not logged in

        TFCharacterRecord rec;
        if (!m_ctx->characters->EnterWorld(acctId, req.charId, rec))
            return; // unknown character or not owned by this account

        SetPlayerFaction(sender, rec.faction);
        m_enteredWorld.insert(sender);
        m_activeCharacter[sender] = rec.id; // W5 onboarding (Task 6): progression re-key target
        // final-review #1 (data loss): seed this session's runtime progression from
        // the durable character record BEFORE any spawn/save can run. Without this,
        // the runtime record starts at the OnPlayerSpawned default (rank 1/0 xp/0
        // flux) and the next SaveNow/disconnect-flush overwrites the durable record
        // back to that default -- silent data loss for a returning character.
        if (m_ctx->progression)
            m_ctx->progression->ServerLoadCharacter(sender, rec.xp, rec.rank, rec.flux);
        // Outfits lane: bind player->character so tag/roster delivery happens at
        // enter-world instead of waiting for the first-spawn fallback.
        if (m_ctx->outfits)
            m_ctx->outfits->ServerOnCharacterEntered(sender, rec.id, rec.name);
        SendWorldWelcome(sender);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] player %u entered world as character %llu (%s)", sender,
                       static_cast<unsigned long long>(rec.id), FactionName(rec.faction));
    }

#endif // ENABLE_NETWORKING

} // namespace Terrafront

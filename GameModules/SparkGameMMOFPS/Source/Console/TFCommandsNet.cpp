/**
 * @file TFCommandsNet.cpp
 * @brief TERRAFRONT console commands — chat + W5 onboarding surface and the
 *        onboarding loopback acceptance self-test (split part of
 *        TFCommands.cpp).
 *
 * Registered from TerrafrontModule::RegisterConsoleCommands() so the single
 * registration entry point (and its order) is preserved. Surface here:
 * tf_chat, and under ENABLE_NETWORKING: tf_register, tf_login,
 * tf_char_create, tf_char_list, tf_enter, tf_selftest_onboarding.
 */

#include "Core/SparkGameMMOFPS.h"

#include "Console/TFCommandsInternal.h"
#include "Account/TFAccountSystem.h"   // W5 onboarding (Task 7): TFAuthErr
#include "Account/TFCharacterSystem.h" // W5 onboarding (Task 7): TFCharErr
#include "Net/TFClientNet.h"
#include "Net/TFServerSim.h"
#include "Net/TFNetProtocol.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFProgressionSystem.h"

#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"
#include "Utils/ScopeGuard.h"
#include "Utils/SecureMemory.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

using namespace Terrafront;
using namespace Terrafront::CommandDetail;

namespace
{

    bool ParseChatChannel(const std::string& arg, ChatChannel& out)
    {
        const std::string s = Lower(arg);
        if (s == "region" || s == "re")
        {
            out = ChatChannel::Region;
            return true;
        }
        if (s == "faction" || s == "f")
        {
            out = ChatChannel::Faction;
            return true;
        }
        if (s == "squad" || s == "s")
        {
            out = ChatChannel::Squad;
            return true;
        }
        if (s == "yell" || s == "y")
        {
            out = ChatChannel::Yell;
            return true;
        }
        return false;
    }

#ifdef ENABLE_NETWORKING
    // ---------------------------------------------------------------------------
    // W5 onboarding (Task 7): loopback acceptance harness.
    //
    // Drives the exact onboarding console commands' underlying primitive
    // (m_ctx.clientNet->SendMsg) over the listen-host/standalone loopback and
    // asserts, with real PASS/FAIL checks (not just "it built"):
    //  (a) the full happy path: register -> login -> character create ->
    //      enter-world -> the player can now spawn (RouteClientMessage lets
    //      gameplay through once the sender is in m_enteredWorld);
    //  (b) THE SECURITY GATE (T6/T4-review #1): a client that has NOT entered
    //      world is blocked from gameplay -- SpawnRequest/FactionSelect before
    //      enter-world produce NO authoritative effect (no pawn, no bound
    //      faction). ClientInput/FireEvent share the exact same guard in
    //      RouteClientMessage's switch, so proving Spawn/FactionSelect are
    //      rejected proves the single choke point that gates all four.
    //
    // Every check is logged at INFO/WARN under the "[TF-ACCEPTANCE]" tag so a
    // detached run's exec/engine log is independently grep-able for the result,
    // per docs/superpowers/specs/2026-07-06-terrafront-onboarding-design.md's
    // "Loopback flow harness" testing note.
    std::string RunOnboardingAcceptanceSelfTest(TFGameContext& ctx)
    {
        std::ostringstream os;
        int passCount = 0, failCount = 0;

        auto check = [&](bool cond, const char* what)
        {
            if (cond)
            {
                ++passCount;
                os << "\n  PASS: " << what;
                SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF-ACCEPTANCE] PASS: %s", what);
            }
            else
            {
                ++failCount;
                os << "\n  FAIL: " << what;
                SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF-ACCEPTANCE] FAIL: %s", what);
            }
        };

        os << "[TF-ACCEPTANCE] W5 onboarding loopback acceptance (T7)";
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF-ACCEPTANCE] starting onboarding loopback acceptance self-test");

        if (!ctx.clientNet || !ctx.serverSim || !ctx.players || !ClientConnected(ctx))
        {
            os << "\n  ABORT: requires clientNet+serverSim+players and a connection (tf_host first)";
            SPARK_LOG_WARN(Spark::LogCategory::Game,
                           "[TF-ACCEPTANCE] ABORT: missing systems or not connected - run tf_host first");
            return os.str();
        }

        // ---- Part 1: THE SECURITY GATE -- gameplay BEFORE login/enter-world must be rejected.
        os << "\n-- gate check: gameplay BEFORE login/enter-world must be rejected --";
        {
            TF_SpawnRequest sr{};
            sr.classId = static_cast<uint8_t>(ClassId::Striker);
            sr.spawnKind = 0;
            ctx.clientNet->SendMsg(TFMsg::SpawnRequest, &sr, sizeof(sr));

            TF_ClientInput in{};
            in.seq = 1;
            ctx.clientNet->SendMsg(TFMsg::ClientInput, &in, sizeof(in));

            TF_FireEvent fe{};
            ctx.clientNet->SendMsg(TFMsg::FireEvent, &fe, sizeof(fe));

            TF_FactionSelect fsel{};
            fsel.faction = static_cast<uint8_t>(FactionId::MRA);
            ctx.clientNet->SendMsg(TFMsg::FactionSelect, &fsel, sizeof(fsel));

            const PlayerId me = ctx.clientNet->LocalPlayerId();
            check(!ctx.serverSim->IsPlayerAlive(me),
                  "SpawnRequest before enter-world spawned NO pawn (RouteClientMessage gate held)");
            check(ctx.serverSim->GetPlayerFaction(me) == FactionId::None,
                  "FactionSelect before enter-world bound NO faction (gate held)");
            check(!ctx.InWorld(), "client is not InWorld before login/enter-world");
        }

        // ---- Part 2: the happy path -- register -> login -> create -> enter -> play.
        os << "\n-- happy path: register -> login -> char-create -> enter-world -> spawn --";
        const uint64_t runId =
            static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) & 0xFFFFFFu;
        const std::string suffix = std::to_string(runId);
        const std::string user = "tf_accept_" + suffix;
        std::string pass = "tf_accept_pw1";
        const auto clearAcceptancePassword = Spark::MakeScopeExit([&] { Spark::SecureClear(pass); });
        const std::string charName = "Acceptance" + suffix;

        {
            TF_AuthRequest reg{};
            const auto clearRegister = Spark::MakeScopeExit([&] { Spark::SecureErase(&reg, sizeof(reg)); });
            std::strncpy(reg.user, user.c_str(), sizeof(reg.user) - 1);
            std::strncpy(reg.pass, pass.c_str(), sizeof(reg.pass) - 1);
            ctx.clientNet->SendMsg(TFMsg::RegisterRequest, &reg, sizeof(reg));
            const auto regErr = static_cast<TFAuthErr>(ctx.clientNet->LastAuthError());
            check(regErr == TFAuthErr::Ok, "unique per-run acceptance account registered");
        }
        {
            TF_AuthRequest login{};
            const auto clearLogin = Spark::MakeScopeExit([&] { Spark::SecureErase(&login, sizeof(login)); });
            std::strncpy(login.user, user.c_str(), sizeof(login.user) - 1);
            std::strncpy(login.pass, pass.c_str(), sizeof(login.pass) - 1);
            ctx.clientNet->SendMsg(TFMsg::LoginRequest, &login, sizeof(login));
            check(ctx.clientNet->IsLoggedIn(),
                  "login succeeded (LoginReply delivered + TFAccountSystem verified the hash)");
        }

        ctx.clientNet->SendMsg(TFMsg::CharListRequest, nullptr, 0);
        uint64_t charId = 0;
        for (const TF_CharBrief& c : ctx.clientNet->CharacterList())
        {
            if (charName == c.name)
            {
                charId = c.id;
                break;
            }
        }
        if (charId == 0)
        {
            TF_CharCreateRequest cc{};
            std::strncpy(cc.name, charName.c_str(), sizeof(cc.name) - 1);
            cc.faction = static_cast<uint8_t>(FactionId::MRA);
            ctx.clientNet->SendMsg(TFMsg::CharCreateReq, &cc, sizeof(cc));
            const auto ccErr = static_cast<TFCharErr>(ctx.clientNet->LastCharOpError());
            check(ccErr == TFCharErr::Ok, "character create accepted");
            charId = ctx.clientNet->LastCharOpId();
        }
        else
        {
            check(true, "character already exists from a prior self-test run (reused, not recreated)");
        }
        check(charId != 0, "have a valid character id to enter with");

        ctx.clientNet->SendMsg(TFMsg::CharListRequest, nullptr, 0);
        check(!ctx.clientNet->CharacterList().empty(), "char list reply shows at least one character");

        {
            TF_EnterWorldRequest ew{};
            ew.charId = charId;
            ctx.clientNet->SendMsg(TFMsg::EnterWorldReq, &ew, sizeof(ew));
            check(ctx.InWorld(), "enter-world succeeded (TF_WorldWelcome delivered, client is InWorld)");
        }

        {
            const PlayerId me = ctx.clientNet->LocalPlayerId();
            check(ctx.serverSim->GetPlayerFaction(me) == FactionId::MRA,
                  "player's authoritative faction is now the character's faction (MRA)");

            TF_SpawnRequest sr{};
            sr.classId = static_cast<uint8_t>(ClassId::Striker);
            sr.spawnKind = 0;
            ctx.clientNet->SendMsg(TFMsg::SpawnRequest, &sr, sizeof(sr));
            check(ctx.serverSim->IsPlayerAlive(me),
                  "SpawnRequest AFTER enter-world spawned a pawn (gate lifted -> gameplay allowed)");

            PawnInfo pawn{};
            const bool hasPawn = ctx.players && ctx.players->GetPawnByPlayer(me, pawn);
            check(hasPawn && pawn.alive, "the spawned pawn is alive and resolvable (can move/fire/spawn)");
        }

        // ---- Part 3: final-review #1/#2 regression -- progression must SURVIVE a
        // disconnect/reconnect for the same character, and must NOT leak into a
        // stale runtime record after the disconnect flush.
        os << "\n-- progression persistence: award -> disconnect -> re-login -> re-enter --";
        if (!ctx.progression)
        {
            check(false, "progression system available for the disconnect/reconnect regression test");
        }
        else
        {
            const PlayerId me = ctx.clientNet->LocalPlayerId();
            check(ctx.serverSim->ActiveCharacterOf(me) == charId,
                  "active character bound to this session matches the one entered above");

            // Award progression through the real server API (the same entry
            // points a kill/capture would use) so this is not a privileged
            // test-only code path.
            ctx.progression->ServerAwardXP(me, 500, kXPReasonCaptureOutpost);
            ctx.progression->ServerGrantFlux(me, 250);
            const uint32_t awardedXP = ctx.progression->XPOf(me);
            const uint16_t awardedRank = ctx.progression->RankOf(me);
            const uint32_t awardedFlux = ctx.progression->FluxOf(me);
            check(awardedXP > 0 && awardedFlux > 0, "progression awarded xp/flux > 0 before the simulated disconnect");

            ctx.progression->SaveNow(); // force the debounced session-scoped save too

            // Simulate a real disconnect via the same cleanup path a real socket
            // drop runs (final flush to the durable character record, THEN
            // ClearPlayer) — see TFServerSim::CleanupPlayerSession /
            // DebugSimulateDisconnect.
            ctx.serverSim->DebugSimulateDisconnect(me);
            check(ctx.progression->XPOf(me) == 0 && ctx.progression->FluxOf(me) == 0,
                  "runtime progression record cleared after simulated disconnect (final-review #2, no leak to the next "
                  "session)");

            // Re-login (a fresh account session) then re-enter the SAME character.
            TF_AuthRequest relogin{};
            const auto clearRelogin = Spark::MakeScopeExit([&] { Spark::SecureErase(&relogin, sizeof(relogin)); });
            std::strncpy(relogin.user, user.c_str(), sizeof(relogin.user) - 1);
            std::strncpy(relogin.pass, pass.c_str(), sizeof(relogin.pass) - 1);
            ctx.clientNet->SendMsg(TFMsg::LoginRequest, &relogin, sizeof(relogin));
            check(ctx.clientNet->IsLoggedIn(), "re-login succeeded after the simulated disconnect");

            TF_EnterWorldRequest rew{};
            rew.charId = charId;
            ctx.clientNet->SendMsg(TFMsg::EnterWorldReq, &rew, sizeof(rew));
            check(ctx.serverSim->ActiveCharacterOf(me) == charId, "re-entered the SAME character after reconnecting");

            // THE regression proof: without final-review #1 (ServerLoadCharacter
            // wired into HandleEnterWorld), these would read back as 0/1/0 --
            // the runtime default -- instead of the values persisted above.
            check(ctx.progression->XPOf(me) == awardedXP,
                  "xp PRESERVED across disconnect/reconnect (final-review #1 regression proof)");
            check(ctx.progression->RankOf(me) == awardedRank,
                  "rank PRESERVED across disconnect/reconnect (final-review #1 regression proof)");
            check(ctx.progression->FluxOf(me) == awardedFlux,
                  "flux PRESERVED across disconnect/reconnect (final-review #1 regression proof)");
        }

        os << "\n[TF-ACCEPTANCE] RESULT: " << passCount << " passed, " << failCount << " failed";
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF-ACCEPTANCE] RESULT: %d passed, %d failed", passCount, failCount);
        return os.str();
    }
#endif // ENABLE_NETWORKING

} // namespace

void TerrafrontModule::RegisterConsoleCommandsNet()
{
    auto& console = Spark::SimpleConsole::GetInstance();
    const char* cat = "TERRAFRONT";

    console.RegisterCommand(
        "tf_chat",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 2)
                return "[TF] usage: tf_chat <region|faction|squad|yell> <message>";
            ChatChannel channel;
            if (!ParseChatChannel(args[0], channel))
                return "[TF] tf_chat: bad channel '" + args[0] + "'";
            std::ostringstream message;
            for (size_t i = 1; i < args.size(); ++i)
            {
                if (i != 1)
                    message << ' ';
                message << args[i];
            }
            if (!m_ctx.clientNet || !m_ctx.clientNet->SendChat(channel, message.str()))
                return "[TF] chat rejected - enter the world and provide non-empty text";
            return "[TF] chat sent";
        },
        "Send a TERRAFRONT chat message", cat, "tf_chat <region|faction|squad|yell> <message>");

    // ------------------------------------------------------------------- W5
    // Onboarding (T7): drive login/character-select/create/enter-world from
    // the console for scripted/standalone use. Each command builds the wire
    // POD and sends it through the SAME path TFLoginFlow uses
    // (m_ctx.clientNet->SendMsg) -- for the listen-host/standalone loopback
    // player the reply is now delivered synchronously in-process
    // (TFServerSim::SendToPlayer's local-player short-circuit, T7), so these
    // commands can report the outcome inline instead of "sent, check later".
#ifdef ENABLE_NETWORKING
    console.RegisterSensitiveCommand(
        "tf_register",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 2)
                return "[TF] usage: tf_register <user> <pass>";
            if (args[0].size() >= sizeof(TF_AuthRequest{}.user) || args[1].size() >= sizeof(TF_AuthRequest{}.pass))
                return "[TF] tf_register: username/password too long";
            if (!ClientConnected(m_ctx))
                return "[TF] not connected - use tf_host or tf_connect first";
            TF_AuthRequest req{};
            const auto clearRequest = Spark::MakeScopeExit([&] { Spark::SecureErase(&req, sizeof(req)); });
            std::strncpy(req.user, args[0].c_str(), sizeof(req.user) - 1);
            std::strncpy(req.pass, args[1].c_str(), sizeof(req.pass) - 1);
            m_ctx.clientNet->SendMsg(TFMsg::RegisterRequest, &req, sizeof(req));
            if (!m_ctx.IsAuthority())
                return "[TF] registration request sent - awaiting server reply";
            const auto err = static_cast<TFAuthErr>(m_ctx.clientNet->LastAuthError());
            return err == TFAuthErr::Ok ? "[TF] register '" + args[0] + "': ok"
                                        : "[TF] register '" + args[0] + "': err=" + std::to_string((int)err);
        },
        "Register a new account (onboarding)", cat, "tf_register <user> <pass>");

    console.RegisterSensitiveCommand(
        "tf_login",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 2)
                return "[TF] usage: tf_login <user> <pass>";
            if (args[0].size() >= sizeof(TF_AuthRequest{}.user) || args[1].size() >= sizeof(TF_AuthRequest{}.pass))
                return "[TF] tf_login: username/password too long";
            if (!ClientConnected(m_ctx))
                return "[TF] not connected - use tf_host or tf_connect first";
            TF_AuthRequest req{};
            const auto clearRequest = Spark::MakeScopeExit([&] { Spark::SecureErase(&req, sizeof(req)); });
            std::strncpy(req.user, args[0].c_str(), sizeof(req.user) - 1);
            std::strncpy(req.pass, args[1].c_str(), sizeof(req.pass) - 1);
            m_ctx.clientNet->SendMsg(TFMsg::LoginRequest, &req, sizeof(req));
            if (!m_ctx.IsAuthority())
                return "[TF] login request sent - awaiting server reply";
            return m_ctx.clientNet->IsLoggedIn()
                       ? "[TF] login ok: account " + std::to_string(m_ctx.clientNet->AccountId())
                       : "[TF] login failed: err=" + std::to_string((int)m_ctx.clientNet->LastAuthError());
        },
        "Log in to an account (onboarding)", cat, "tf_login <user> <pass>");

    console.RegisterCommand(
        "tf_char_create",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 2)
                return "[TF] usage: tf_char_create <name> <mra|auc|hlx>";
            FactionId f;
            if (!ParseFaction(args[1], f))
                return "[TF] tf_char_create: bad faction '" + args[1] + "'";
            if (args[0].size() >= sizeof(TF_CharCreateRequest{}.name))
                return "[TF] tf_char_create: name too long";
            if (!ClientConnected(m_ctx))
                return "[TF] not connected - use tf_host or tf_connect first";
            TF_CharCreateRequest req{};
            std::strncpy(req.name, args[0].c_str(), sizeof(req.name) - 1);
            req.faction = static_cast<uint8_t>(f);
            m_ctx.clientNet->SendMsg(TFMsg::CharCreateReq, &req, sizeof(req));
            if (!m_ctx.IsAuthority())
                return "[TF] character creation request sent - awaiting server reply";
            const auto err = static_cast<TFCharErr>(m_ctx.clientNet->LastCharOpError());
            return err == TFCharErr::Ok ? "[TF] character '" + args[0] + "' created: id " +
                                              std::to_string(m_ctx.clientNet->LastCharOpId())
                                        : "[TF] char create failed: err=" + std::to_string((int)err);
        },
        "Create a character (onboarding)", cat, "tf_char_create <name> <mra|auc|hlx>");

    console.RegisterCommand(
        "tf_char_list",
        [this](const std::vector<std::string>&) -> std::string
        {
            if (!ClientConnected(m_ctx))
                return "[TF] not connected - use tf_host or tf_connect first";
            m_ctx.clientNet->SendMsg(TFMsg::CharListRequest, nullptr, 0);
            const auto& list = m_ctx.clientNet->CharacterList();
            if (list.empty())
                return "[TF] no characters (log in first, or none created yet)";
            std::ostringstream os;
            os << "[TF] characters (" << list.size() << "):";
            for (size_t i = 0; i < list.size(); ++i)
            {
                const TF_CharBrief& c = list[i];
                os << "\n  [" << i << "] " << c.name << "  " << FactionTag(static_cast<FactionId>(c.faction))
                   << "  rank " << c.rank << "  id " << c.id;
            }
            return os.str();
        },
        "List your characters (onboarding)", cat, "tf_char_list");

    console.RegisterCommand(
        "tf_enter",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "[TF] usage: tf_enter <charId|index>";
            if (!ClientConnected(m_ctx))
                return "[TF] not connected - use tf_host or tf_connect first";
            char* end = nullptr;
            const unsigned long long n = std::strtoull(args[0].c_str(), &end, 10);
            if (end == args[0].c_str())
                return "[TF] tf_enter: bad id/index '" + args[0] + "'";
            const auto& list = m_ctx.clientNet->CharacterList();
            const uint64_t charId = (n < list.size()) ? list[static_cast<size_t>(n)].id : static_cast<uint64_t>(n);
            TF_EnterWorldRequest req{};
            req.charId = charId;
            m_ctx.clientNet->SendMsg(TFMsg::EnterWorldReq, &req, sizeof(req));
            return m_ctx.InWorld() ? "[TF] entered world as character " + std::to_string(charId)
                                   : "[TF] enter-world request sent for character " + std::to_string(charId) +
                                         " - not yet in world (check login/ownership)";
        },
        "Enter the world as a character (onboarding)", cat, "tf_enter <charId|index>");

    console.RegisterCommand(
        "tf_selftest_onboarding",
        [this](const std::vector<std::string>&) -> std::string { return RunOnboardingAcceptanceSelfTest(m_ctx); },
        "W5 T7 acceptance: proves onboarding happy-path + the enter-world security gate (loopback)", cat,
        "tf_selftest_onboarding");
#endif // ENABLE_NETWORKING
}

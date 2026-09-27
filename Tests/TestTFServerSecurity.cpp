/**
 * @file TestTFServerSecurity.cpp
 * @brief TERRAFRONT (SparkGameMMOFPS) server trust-boundary regressions from the
 *        security review: client-controlled view angles, socket routing of every
 *        gated client id, session/character exclusivity, faction binding,
 *        server-supplied character names, stored-hash parameter bounds and logout.
 *
 * SparkTests does not compile TFServerSim; each fix is exercised through the
 * header-only rule or the standalone module .cpp that the server path calls.
 */
#include "TestFramework.h"
#include "Account/TFAccountSystem.h"
#include "Account/TFCrypto.h"
#include "Net/TFClientMsgRouting.h"
#include "Net/TFClientSessionEnd.h"
#include "Net/TFClientSessionState.h"
#include "Net/TFOnboardingSessionRules.h"
#include "Net/TFRepProtocol.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Terrafront;

namespace
{
    constexpr float kWrapPiBound = 3.14159265f;

    bool IsWrapped(float a)
    {
        return std::isfinite(a) && a >= -kWrapPiBound && a <= kWrapPiBound;
    }
} // namespace

// A huge finite client viewYaw used to spin QuantAim::WrapPi forever: at
// |a| >= 2^27, a - 2*pi rounds back to a, so the authoritative tick never
// returned (walking, seated and turret-seat input paths all call it). Without
// the constant-time wrap this test hangs until the CTest timeout.
TEST(TFSec_WrapPiIsConstantTimeForHugeFiniteAngles)
{
    const float huge[] = {1e30f,         -1e30f, FLT_MAX, -FLT_MAX,
                          134217728.0f, // 2^27: first magnitude where one 2*pi step is lost
                          -134217728.0f, 1e8f,   -1e8f};
    for (const float a : huge)
    {
        const float w = QuantAim::WrapPi(a);
        EXPECT_TRUE(IsWrapped(w));
    }

    // Non-finite input is total (0), never a loop or a propagated NaN.
    EXPECT_EQ(QuantAim::WrapPi(std::numeric_limits<float>::infinity()), 0.0f);
    EXPECT_EQ(QuantAim::WrapPi(-std::numeric_limits<float>::infinity()), 0.0f);
    EXPECT_EQ(QuantAim::WrapPi(std::numeric_limits<float>::quiet_NaN()), 0.0f);

    // Quantization of a hostile angle stays inside the int16 wire range.
    const QuantAim q = QuantAim::From(1e30f, -FLT_MAX);
    EXPECT_TRUE(q.yaw >= -31416 && q.yaw <= 31416);
    EXPECT_TRUE(q.pitch >= -31416 && q.pitch <= 31416);
}

TEST(TFSec_WrapPiKeepsInRangeAnglesAndWrapsSmallMultiples)
{
    // In-range input is returned bit-for-bit (client prediction parity).
    const float inRange[] = {0.0f, 1.0f, -1.0f, 3.0f, -3.0f, 3.14159265f, -3.14159265f};
    for (const float a : inRange)
        EXPECT_EQ(QuantAim::WrapPi(a), a);

    const float twoPi = 6.28318531f;
    EXPECT_NEAR(QuantAim::WrapPi(1.0f + twoPi), 1.0f, 1e-5f);
    EXPECT_NEAR(QuantAim::WrapPi(-1.0f - twoPi), -1.0f, 1e-5f);
    EXPECT_NEAR(QuantAim::WrapPi(0.5f + 3.0f * twoPi), 0.5f, 1e-4f);
    EXPECT_NEAR(QuantAim::WrapPi(4.0f), 4.0f - twoPi, 1e-5f);
    EXPECT_NEAR(QuantAim::WrapPi(-4.0f), -4.0f + twoPi, 1e-5f);
}

// TFMsg::LoadoutExtChange was enter-world gated and dispatched inside
// RouteClientMessage, but RegisterNetHandlers never registered it, so every
// socket client's grenade/suit save was dropped as an unknown message type.
// Registration, teardown and the gate now read one list; every gated id is
// therefore socket-routed.
TEST(TFSec_EveryGatedClientMsgIsSocketRouted)
{
    static_assert(IsEnteredWorldGatedMsg(TFMsg::LoadoutExtChange));
    static_assert(IsEnteredWorldGatedMsg(TFMsg::LoadoutChange));
    static_assert(IsEnteredWorldGatedMsg(TFMsg::ClientInput));
    static_assert(IsEnteredWorldGatedMsg(TFMsg::FactionSelect));
    EXPECT_TRUE(std::find(kTFEnteredWorldGatedMsgs.begin(), kTFEnteredWorldGatedMsgs.end(), TFMsg::LoadoutExtChange) !=
                kTFEnteredWorldGatedMsgs.end());

    // Onboarding and credential ids are how a session enters the world: never gated.
    for (const TFMsg id : kTFOnboardingMsgs)
        EXPECT_FALSE(IsEnteredWorldGatedMsg(id));
    for (const TFMsg id : kTFCredentialMsgs)
        EXPECT_FALSE(IsEnteredWorldGatedMsg(id));

    // No id is listed twice (a duplicate would silently hide a missing one).
    std::vector<TFMsg> all(kTFEnteredWorldGatedMsgs.begin(), kTFEnteredWorldGatedMsgs.end());
    all.insert(all.end(), kTFOnboardingMsgs.begin(), kTFOnboardingMsgs.end());
    all.insert(all.end(), kTFCredentialMsgs.begin(), kTFCredentialMsgs.end());
    std::sort(all.begin(), all.end());
    EXPECT_TRUE(std::adjacent_find(all.begin(), all.end()) == all.end());
    EXPECT_EQ(all.size(), size_t{24});
}

// The stored hash is database-controlled. VerifyPassword used to trust its
// embedded cost (any nonzero uint32 iteration count, any derived-key length),
// so a tampered or corrupt row such as "pbkdf2-sha256$4294967295$00$00" made
// every login attempt for that username run ~4.3e9 HMAC rounds before the
// password was even compared. The same bounds hold for the scram-sha256 rows
// NET-100 stores now and for the legacy pbkdf2-sha256 rows it migrates.
// Without the bounds this test hangs until the CTest timeout.
TEST(TFSec_StoredHashParametersAreBoundedBeforeDerivation)
{
    // A legacy row exactly as the pre-NET-100 HashPassword wrote it.
    const std::string saltHex = TFAccountSystem::GenerateSalt();
    ASSERT_EQ(saltHex.size(), size_t{32});
    const std::string dk =
        Crypto::ToHex(Crypto::Pbkdf2HmacSha256("correcthorse1", Crypto::FromHex(saltHex), 150000, 32));
    ASSERT_EQ(dk.size(), size_t{64});
    const auto legacyWithIters = [&](const std::string& iters)
    { return std::string("pbkdf2-sha256$") + iters + "$" + saltHex + "$" + dk; };
    EXPECT_TRUE(TFAccountSystem::VerifyPassword("correcthorse1", legacyWithIters("150000")));
    EXPECT_FALSE(TFAccountSystem::VerifyPassword("wrongpassword", legacyWithIters("150000")));

    // The current row: "scram-sha256$150000$<salt>$<storedKey>$<serverKey>".
    const std::string scram = TFAccountSystem::HashPassword("correcthorse1", saltHex);
    EXPECT_TRUE(TFAccountSystem::VerifyPassword("correcthorse1", scram));
    const std::string scramPrefix = "scram-sha256$150000$";
    ASSERT_TRUE(scram.rfind(scramPrefix, 0) == 0);
    const std::string scramTail = scram.substr(scramPrefix.size()); // salt$storedKey$serverKey
    const auto scramWithIters = [&](const std::string& iters)
    { return std::string("scram-sha256$") + iters + "$" + scramTail; };

    for (const auto& withIters : {std::function<std::string(const std::string&)>(legacyWithIters),
                                  std::function<std::string(const std::string&)>(scramWithIters)})
    {
        // Unbounded cost: rejected without deriving.
        EXPECT_FALSE(TFAccountSystem::VerifyPassword("correcthorse1", withIters("4294967295")));
        EXPECT_FALSE(TFAccountSystem::VerifyPassword("correcthorse1", withIters("600001")));
        EXPECT_FALSE(TFAccountSystem::VerifyPassword("correcthorse1", withIters("99999")));
        EXPECT_FALSE(TFAccountSystem::VerifyPassword("correcthorse1", withIters("0")));

        // The iteration field must be a fully consumed decimal number.
        EXPECT_FALSE(TFAccountSystem::VerifyPassword("correcthorse1", withIters("+150000")));
        EXPECT_FALSE(TFAccountSystem::VerifyPassword("correcthorse1", withIters(" 150000")));
        EXPECT_FALSE(TFAccountSystem::VerifyPassword("correcthorse1", withIters("150000abc")));
        EXPECT_FALSE(TFAccountSystem::VerifyPassword("correcthorse1", withIters("")));
        EXPECT_FALSE(TFAccountSystem::VerifyPassword("correcthorse1", withIters("4294967296150000")));
    }
    EXPECT_FALSE(TFAccountSystem::VerifyPassword("anything", "pbkdf2-sha256$4294967295$00$00"));
    EXPECT_FALSE(TFAccountSystem::VerifyPassword("anything", "scram-sha256$4294967295$00$00$00"));

    // A legacy salt and derived key must be exactly the sizes the old
    // HashPassword wrote (a longer derived key multiplies the PBKDF2 block count).
    const std::string longDk = std::string("pbkdf2-sha256$150000$") + saltHex + "$" + dk + dk + dk + dk;
    EXPECT_FALSE(TFAccountSystem::VerifyPassword("correcthorse1", longDk));
    const std::string shortSalt = std::string("pbkdf2-sha256$150000$") + saltHex.substr(0, 16) + "$" + dk;
    EXPECT_FALSE(TFAccountSystem::VerifyPassword("correcthorse1", shortSalt));
    const std::string badHexSalt = std::string("pbkdf2-sha256$150000$") + std::string(32, 'z') + "$" + dk;
    EXPECT_FALSE(TFAccountSystem::VerifyPassword("correcthorse1", badHexSalt));
}

// Two connections with the same credentials used to both bind the account and
// both enter the same character, each with its own runtime wallet that later
// persisted to the same row (a stale session could restore spent flux). The
// account now binds to one live connection until its session is cleared.
TEST(TFSec_AccountBindsToOneLiveConnection)
{
    TFAccountSystem accounts;
    constexpr uint32_t kFirst = 11;
    constexpr uint32_t kSecond = 22;
    constexpr uint64_t kAccount = 7;

    EXPECT_TRUE(accounts.BindSession(kFirst, kAccount));
    EXPECT_TRUE(accounts.BindSession(kFirst, kAccount)); // idempotent for the owner
    EXPECT_FALSE(accounts.BindSession(kSecond, kAccount));
    EXPECT_EQ(accounts.AccountForClient(kSecond), uint64_t{0});
    EXPECT_EQ(accounts.AccountForClient(kFirst), kAccount); // refusal leaves the owner intact
    EXPECT_FALSE(accounts.BindSession(kSecond, 0));

    // Clearing a non-owner is a no-op for the owner's binding.
    accounts.ClearSession(kSecond);
    EXPECT_EQ(accounts.AccountForClient(kFirst), kAccount);
    EXPECT_FALSE(accounts.BindSession(kSecond, kAccount));

    // Disconnect/logout cleanup releases the account for the next connection.
    accounts.ClearSession(kFirst);
    EXPECT_EQ(accounts.AccountForClient(kFirst), uint64_t{0});
    EXPECT_TRUE(accounts.BindSession(kSecond, kAccount));
    EXPECT_EQ(accounts.AccountForClient(kSecond), kAccount);

    // Rebinding a connection to another account releases its previous one.
    EXPECT_TRUE(accounts.BindSession(kSecond, kAccount + 1));
    EXPECT_TRUE(accounts.BindSession(kFirst, kAccount));

    // The refused login is not a logged-in client view.
    TFClientSessionState state;
    state.ApplyLoginReply(false, 0, TFAuthErr::AccountInUse);
    EXPECT_FALSE(state.loggedIn);
    EXPECT_EQ(state.accountId, uint64_t{0});
}

TEST(TFSec_CharacterIsResidentInOneSessionOnly)
{
    std::unordered_map<uint32_t, uint64_t> active{{1u, 100u}, {2u, 200u}};
    EXPECT_TRUE(IsCharacterResidentElsewhere(active, 3u, uint64_t{100}));
    EXPECT_FALSE(IsCharacterResidentElsewhere(active, 1u, uint64_t{100})); // its own session
    EXPECT_FALSE(IsCharacterResidentElsewhere(active, 3u, uint64_t{300}));
    active.erase(1u); // disconnect cleanup
    EXPECT_FALSE(IsCharacterResidentElsewhere(active, 3u, uint64_t{100}));
}

// HandleFactionSelect's only guard was "no live pawn", which holds before the
// first spawn and after every death, so an entered-world client could rebind
// its session faction away from its character's (reading/posting another
// faction's chat, spawning at its skyanchor). A character-bound session's
// faction now comes only from the character record.
TEST(TFSec_CharacterBoundFactionIsNotClientSelectable)
{
    EXPECT_FALSE(CanApplyFactionSelect(true, false)); // bound, pre-spawn or dead
    EXPECT_FALSE(CanApplyFactionSelect(true, true));  // bound, alive
    EXPECT_FALSE(CanApplyFactionSelect(false, true)); // legacy: no switch while alive
    EXPECT_TRUE(CanApplyFactionSelect(false, false)); // legacy unbound session, no pawn
}

// The client adopted a CharListReply after only a size check; a malicious
// server could fill every TF_CharBrief name (and the trailing bytes) with
// non-zero data, and the char-select label / tf_char_list then read the name
// as a C string past the end of the vector's heap buffer. Malformed replies
// are now rejected whole at receipt.
TEST(TFSec_CharListReplyRejectsUnterminatedNames)
{
    TF_CharListReply good{};
    good.count = 2;
    good.chars[0].id = 101;
    std::strncpy(good.chars[0].name, "Alpha", sizeof(good.chars[0].name) - 1);
    good.chars[1].id = 102;
    std::memset(good.chars[1].name, 'B', sizeof(good.chars[1].name) - 1); // 23 chars + NUL: the maximum
    good.chars[1].name[sizeof(good.chars[1].name) - 1] = '\0';

    TFClientSessionState state;
    EXPECT_TRUE(state.ApplyCharListReply(good));
    ASSERT_EQ(state.characters.size(), size_t{2});
    EXPECT_EQ(state.characters[0].id, uint64_t{101});
    EXPECT_EQ(std::strlen(state.characters[1].name), size_t{23});

    // Every byte non-zero (names and trailing fields): the over-read payload.
    TF_CharListReply hostile{};
    std::memset(&hostile, 0x41, sizeof(hostile));
    hostile.count = 5;
    EXPECT_FALSE(state.ApplyCharListReply(hostile));
    EXPECT_EQ(state.characters.size(), size_t{2}); // previous list kept, nothing adopted

    // Only the last listed name is unterminated.
    TF_CharListReply lastBad = good;
    lastBad.count = 3;
    std::memset(lastBad.chars[2].name, 'C', sizeof(lastBad.chars[2].name));
    EXPECT_FALSE(state.ApplyCharListReply(lastBad));

    // Unlisted slots are not inspected; a count past the array is malformed.
    TF_CharListReply unlistedGarbage = good;
    std::memset(unlistedGarbage.chars[4].name, 'D', sizeof(unlistedGarbage.chars[4].name));
    EXPECT_TRUE(state.ApplyCharListReply(unlistedGarbage));
    TF_CharListReply overCount = good;
    overCount.count = 6;
    EXPECT_FALSE(state.ApplyCharListReply(overCount));

    TF_CharListReply empty{};
    EXPECT_TRUE(state.ApplyCharListReply(empty));
    EXPECT_TRUE(state.characters.empty());
}

// The character-select Logout button only reset UI fields, so the authority
// kept the connection bound to the account (character ops still authorized as
// it; a new login refused with SessionActive). Logout now ends the server
// session: a remote client closes its transport (socket leave -> cleanup ->
// ClearSession); the in-process host player runs that cleanup directly
// (TFClientNet::Disconnect) and keeps hosting.
TEST(TFSec_LogoutEndsTheAuthoritativeSession)
{
    EXPECT_TRUE(LogoutStopsTransport(NetRole::Client));
    EXPECT_FALSE(LogoutStopsTransport(NetRole::ListenHost));
    EXPECT_FALSE(LogoutStopsTransport(NetRole::Standalone));

    // The server-side half of logout is ClearSession: it must release the
    // account so the same (or another) connection can sign in again.
    TFAccountSystem accounts;
    EXPECT_TRUE(accounts.BindSession(5, 42));
    accounts.ClearSession(5);
    EXPECT_EQ(accounts.AccountForClient(5), uint64_t{0});
    EXPECT_TRUE(accounts.BindSession(5, 42));

    // The client's reply-driven view is reset with the session.
    TFClientSessionState state;
    state.ApplyLoginReply(true, 42, TFAuthErr::Ok);
    EXPECT_TRUE(state.loggedIn);
    state.Reset();
    EXPECT_FALSE(state.loggedIn);
    EXPECT_EQ(state.accountId, uint64_t{0});

    // A remote client's session end drops it to Standalone and resets the flow.
    const TFClientSessionEndDecision end = PlanClientSessionEnd(NetRole::Client, false);
    EXPECT_TRUE(end.role == NetRole::Standalone);
    EXPECT_TRUE(end.resetLoginFlow);
}

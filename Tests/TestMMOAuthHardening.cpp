/**
 * @file TestMMOAuthHardening.cpp
 * @brief MMO authentication hardening: equal-work login, per-peer admission, and registration limits.
 *
 * Timing is asserted structurally (how many PBKDF2 verifications run, with which parameters),
 * never by wall-clock measurement. Admission budgets run on the gate's server tick time, which
 * the tests drive explicitly.
 */

#include "TestFramework.h"

#include "../GameModules/SparkGameMMO/Source/Account/MMOAccountSystem.h"
#include "../GameModules/SparkGameMMO/Source/Session/MMOAuthAdmission.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    /// Records each password verification Login asks for. The recorded hash fixes the PBKDF2
    /// work Spark::PasswordHash::Verify performs (scheme, iteration count, salt and key length).
    struct VerifierProbe
    {
        int calls = 0;
        std::string lastHash;
    };
    VerifierProbe g_verifierProbe;

    bool RecordingVerifier(std::string_view /*password*/, std::string_view encodedHash)
    {
        ++g_verifierProbe.calls;
        g_verifierProbe.lastHash = std::string(encodedHash);
        return false;
    }

    constexpr std::string_view kLockoutPassword = "correct-horse-battery";

    /// Accepts exactly kLockoutPassword, standing in for a PBKDF2 match without the derivation cost.
    bool LockoutVerifier(std::string_view password, std::string_view /*encodedHash*/)
    {
        ++g_verifierProbe.calls;
        return password == kLockoutPassword;
    }

    std::vector<std::string> SplitHash(const std::string& encoded)
    {
        std::vector<std::string> parts;
        size_t start = 0;
        while (true)
        {
            const size_t separator = encoded.find('$', start);
            parts.push_back(encoded.substr(start, separator == std::string::npos ? separator : separator - start));
            if (separator == std::string::npos)
            {
                return parts;
            }
            start = separator + 1;
        }
    }

    bool IsHex(const std::string& text)
    {
        return !text.empty() && text.find_first_not_of("0123456789abcdef") == std::string::npos;
    }

    /// True when @p candidate makes Verify run exactly the key derivation @p reference does.
    bool SameDerivationWork(const std::string& reference, const std::string& candidate)
    {
        const auto ref = SplitHash(reference);
        const auto got = SplitHash(candidate);
        return ref.size() == 4 && got.size() == 4 && got[0] == ref[0] && got[1] == ref[1] &&
               got[2].size() == ref[2].size() && got[3].size() == ref[3].size() && IsHex(got[2]) && IsHex(got[3]);
    }
} // namespace

// SEC follow-up: Login used to return before any key derivation when the username was unknown
// (or the account was locked, suspended or banned), so response time revealed which usernames
// exist. Every path must now run exactly one verification with the production PBKDF2 parameters.
TEST(MMOAuth_UnknownUserPerformsSamePbkdf2WorkAsKnownUser)
{
    MMO::MMOAccountSystem accounts;
    ASSERT_TRUE(accounts.Initialize(nullptr));
    const MMO::AuthResult registered = accounts.Register("timing_known", "correct-horse-battery");
    ASSERT_TRUE(registered.success);
    const auto account = accounts.GetAccount(registered.accountId);
    ASSERT_TRUE(account.has_value());
    const std::string realHash = account->passwordHash;
    ASSERT_EQ(SplitHash(realHash).size(), static_cast<size_t>(4));

    accounts.SetPasswordVerifier(&RecordingVerifier);

    g_verifierProbe = {};
    const MMO::AuthResult wrongPassword = accounts.Login("timing_known", "wrong-password");
    EXPECT_FALSE(wrongPassword.success);
    EXPECT_EQ(g_verifierProbe.calls, 1);
    EXPECT_TRUE(g_verifierProbe.lastHash == realHash);

    g_verifierProbe = {};
    const MMO::AuthResult unknown = accounts.Login("timing_nobody", "wrong-password");
    EXPECT_FALSE(unknown.success);
    EXPECT_EQ(unknown.errorMessage, wrongPassword.errorMessage);
    EXPECT_EQ(g_verifierProbe.calls, 1);
    EXPECT_TRUE(SameDerivationWork(realHash, g_verifierProbe.lastHash));
    EXPECT_TRUE(g_verifierProbe.lastHash != realHash);

    // A restricted account must not answer faster than an active one either.
    for (const auto status : {MMO::AccountStatus::Locked, MMO::AccountStatus::Suspended, MMO::AccountStatus::Banned})
    {
        ASSERT_TRUE(accounts.SetAccountStatus(registered.accountId, status, "test"));
        g_verifierProbe = {};
        const MMO::AuthResult restricted = accounts.Login("timing_known", "wrong-password");
        EXPECT_FALSE(restricted.success);
        EXPECT_EQ(g_verifierProbe.calls, 1);
        EXPECT_TRUE(g_verifierProbe.lastHash == realHash);
    }
    // Attempts against a restricted account are refused before they count as failures.
    const auto after = accounts.GetAccount(registered.accountId);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->failedLoginAttempts, 1);

    accounts.SetPasswordVerifier(nullptr);
    accounts.Shutdown();
}

// Per-account brute-force bound: the failed-login lockout must still stop guessing once every attempt
// runs the verifier first. After MAX_FAILED_LOGINS (5) wrong passwords the account locks, the correct
// password is refused while locked, and further guesses are refused without counting.
TEST(MMOAuth_FailedLoginLockoutStopsBruteForce)
{
    constexpr int kMaxFailedLogins = 5; // MMOAccountSystem::MAX_FAILED_LOGINS
    MMO::MMOAccountSystem accounts;
    ASSERT_TRUE(accounts.Initialize(nullptr));
    const MMO::AuthResult registered = accounts.Register("lockout_user", std::string(kLockoutPassword));
    ASSERT_TRUE(registered.success);
    accounts.SetPasswordVerifier(&LockoutVerifier);

    for (int attempt = 1; attempt <= kMaxFailedLogins; ++attempt)
    {
        const auto before = accounts.GetAccount(registered.accountId);
        ASSERT_TRUE(before.has_value());
        EXPECT_TRUE(before->status == MMO::AccountStatus::Active);
        EXPECT_FALSE(accounts.Login("lockout_user", "guess-" + std::to_string(attempt)).success);
    }
    const auto locked = accounts.GetAccount(registered.accountId);
    ASSERT_TRUE(locked.has_value());
    EXPECT_TRUE(locked->status == MMO::AccountStatus::Locked);
    EXPECT_EQ(locked->failedLoginAttempts, kMaxFailedLogins);

    // The right password does not get through a locked account, and more guesses do not count.
    g_verifierProbe = {};
    const MMO::AuthResult correctWhileLocked = accounts.Login("lockout_user", kLockoutPassword);
    EXPECT_FALSE(correctWhileLocked.success);
    EXPECT_TRUE(correctWhileLocked.sessionToken.empty());
    EXPECT_EQ(g_verifierProbe.calls, 1);
    EXPECT_FALSE(accounts.Login("lockout_user", "guess-after-lock").success);
    const auto stillLocked = accounts.GetAccount(registered.accountId);
    ASSERT_TRUE(stillLocked.has_value());
    EXPECT_TRUE(stillLocked->status == MMO::AccountStatus::Locked);
    EXPECT_EQ(stillLocked->failedLoginAttempts, kMaxFailedLogins);

    // Control: the same password is accepted once the lock is lifted, so the lock is what refused it.
    ASSERT_TRUE(accounts.SetAccountStatus(registered.accountId, MMO::AccountStatus::Active, "test"));
    EXPECT_TRUE(accounts.Login("lockout_user", kLockoutPassword).success);

    accounts.SetPasswordVerifier(nullptr);
    accounts.Shutdown();
}

// One peer's credential attempts must never hold a different peer at RateLimited.
TEST(MMOAuth_AdmissionBudgetIsPerPeer)
{
    using Budget = MMO::AuthAdmissionBudget;
    Budget budget;
    Budget::Peer alice;
    Budget::Peer bob;

    EXPECT_TRUE(budget.TryAdmit(alice, Budget::Operation::Login));
    EXPECT_FALSE(budget.TryAdmit(alice, Budget::Operation::Login)); // alice's own cooldown
    EXPECT_TRUE(budget.TryAdmit(bob, Budget::Operation::Login));    // bob is unaffected by alice

    // Half the cooldown is not enough; the full cooldown restores alice.
    budget.Advance(Budget::PeerCooldown * 0.5f);
    Budget::AdvancePeer(alice, Budget::PeerCooldown * 0.5f);
    EXPECT_FALSE(budget.TryAdmit(alice, Budget::Operation::Login));
    budget.Advance(Budget::PeerCooldown * 0.5f);
    Budget::AdvancePeer(alice, Budget::PeerCooldown * 0.5f);
    EXPECT_TRUE(budget.TryAdmit(alice, Budget::Operation::Login));
}

// Brute-force bound: however many peers try, total KDF admissions stay within the global bucket.
TEST(MMOAuth_AdmissionBudgetBoundsAggregateRate)
{
    using Budget = MMO::AuthAdmissionBudget;
    Budget budget;
    std::array<Budget::Peer, 64> peers{};

    size_t burst = 0;
    for (auto& peer : peers)
    {
        burst += budget.TryAdmit(peer, Budget::Operation::Login) ? 1u : 0u;
    }
    EXPECT_EQ(burst, static_cast<size_t>(Budget::GlobalBurst));

    // Ten seconds of every peer retrying every 0.1 s tick.
    constexpr float kTick = 0.1f;
    constexpr int kTicks = 100;
    size_t admitted = 0;
    std::array<size_t, 64> perPeer{};
    for (int tick = 0; tick < kTicks; ++tick)
    {
        budget.Advance(kTick);
        for (size_t index = 0; index < peers.size(); ++index)
        {
            Budget::AdvancePeer(peers[index], kTick);
            if (budget.TryAdmit(peers[index], Budget::Operation::Login))
            {
                ++admitted;
                ++perPeer[index];
            }
        }
    }
    const float seconds = kTick * static_cast<float>(kTicks);
    EXPECT_LE(admitted, static_cast<size_t>(std::lround(Budget::GlobalRate * seconds)));
    EXPECT_GE(admitted, static_cast<size_t>(Budget::GlobalRate * seconds - 1.5f));
    for (const size_t count : perPeer)
    {
        EXPECT_LE(count, static_cast<size_t>(std::lround(seconds / Budget::PeerCooldown)));
    }
}

// Registration: one account per connection, and a global window across reconnecting peers.
TEST(MMOAuth_RegistrationBudgetPerPeerAndWindow)
{
    using Budget = MMO::AuthAdmissionBudget;
    Budget budget;
    std::array<Budget::Peer, 10> peers{};

    // Two ticks of four admissions: eight registrations exhaust the window.
    for (size_t index = 0; index < 4; ++index)
    {
        EXPECT_TRUE(budget.TryAdmit(peers[index], Budget::Operation::Register));
    }
    budget.Advance(1.0f);
    for (size_t index = 4; index < 8; ++index)
    {
        EXPECT_TRUE(budget.TryAdmit(peers[index], Budget::Operation::Register));
    }
    budget.Advance(1.0f);
    EXPECT_FALSE(budget.TryAdmit(peers[8], Budget::Operation::Register));
    // A refused registration charges nothing: the same peer can still log in.
    EXPECT_TRUE(budget.TryAdmit(peers[8], Budget::Operation::Login));

    // The window refills at RegistrationsPerWindow per RegistrationWindow seconds.
    const float refillOne = Budget::RegistrationWindow / Budget::RegistrationsPerWindow;
    budget.Advance(refillOne);
    Budget::AdvancePeer(peers[9], refillOne);
    EXPECT_TRUE(budget.TryAdmit(peers[9], Budget::Operation::Register));

    // A peer that already registered cannot register again, even with its cooldown elapsed.
    budget.Advance(Budget::RegistrationWindow);
    Budget::AdvancePeer(peers[0], Budget::RegistrationWindow);
    EXPECT_FALSE(budget.TryAdmit(peers[0], Budget::Operation::Register));
    EXPECT_TRUE(budget.TryAdmit(peers[0], Budget::Operation::Login));
}

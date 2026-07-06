/**
 * @file TFAccountSystem.cpp
 * @brief TERRAFRONT account register/login core logic (W5 onboarding, Task 2).
 *
 * Kept minimal-dependency (TFDatabase.h + stdlib) so it links standalone
 * into SparkTests without pulling in TFGameContext or the engine module
 * scaffolding.
 */
#include "Account/TFAccountSystem.h"

#include <chrono>
#include <random>
#include <sstream>

namespace Terrafront {

// === Hashing utilities (ported verbatim from MMOAccountSystem.cpp:61-81) ===

std::string TFAccountSystem::GenerateSalt()
{
    static std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t val = dist(rng);
    std::ostringstream ss;
    ss << std::hex << val;
    return ss.str();
}

std::string TFAccountSystem::HashPassword(const std::string& password, const std::string& salt)
{
    // NOTE: std::hash is still not a vetted cryptographic primitive (no
    // preimage/collision guarantees, implementation-defined per STL vendor).
    // A real fix means migrating to Argon2id/PBKDF2-HMAC-SHA256 behind an
    // algorithm-tag-per-account scheme (see onboarding-auth complexPlan).
    // Until that lands, this iterated many-round construction — re-salting
    // and re-indexing every round before re-hashing — raises offline
    // brute-force cost by ~5 orders of magnitude over the previous 2-round
    // mix, with no new dependencies.
    constexpr int kRounds = 100000;
    std::string combined = salt + password + salt;
    size_t hash = std::hash<std::string>{}(combined);
    for (int round = 0; round < kRounds; ++round)
    {
        std::ostringstream mix;
        mix << salt << std::hex << hash << password << round;
        hash = std::hash<std::string>{}(mix.str());
    }
    std::ostringstream ss;
    ss << std::hex << hash;
    return ss.str();
}

// === Registration / login ===

TFAuthResult TFAccountSystem::Register(const std::string& username, const std::string& password)
{
    TFAuthResult result;
    if (!m_db)
    {
        result.err = TFAuthErr::ServerError;
        return result;
    }
    if (username.size() < 3)
    {
        result.err = TFAuthErr::UsernameTooShort;
        return result;
    }
    if (password.size() < 8)
    {
        result.err = TFAuthErr::PasswordTooShort;
        return result;
    }

    TFAccountRecord existing;
    if (m_db->FindAccountByUsername(username, existing))
    {
        result.err = TFAuthErr::UsernameTaken;
        return result;
    }

    std::string salt = GenerateSalt();
    std::string hash = HashPassword(password, salt);

    TFAccountRecord rec;
    if (!m_db->CreateAccount(username, salt, hash, rec))
    {
        result.err = TFAuthErr::UsernameTaken;
        return result;
    }

    result.ok = true;
    result.err = TFAuthErr::Ok;
    result.accountId = rec.id;
    return result;
}

TFAuthResult TFAccountSystem::Login(const std::string& username, const std::string& password)
{
    TFAuthResult result;
    if (!m_db)
    {
        result.err = TFAuthErr::ServerError;
        return result;
    }

    TFAccountRecord rec;
    if (!m_db->FindAccountByUsername(username, rec) || HashPassword(password, rec.salt) != rec.passwordHash)
    {
        result.err = TFAuthErr::BadCredentials;
        return result;
    }

    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    m_db->TouchLogin(rec.id, nowMs);

    result.ok = true;
    result.err = TFAuthErr::Ok;
    result.accountId = rec.id;
    return result;
}

// === Session map ===

void TFAccountSystem::BindSession(uint32_t clientId, uint64_t accountId)
{
    m_sessions[clientId] = accountId;
}

uint64_t TFAccountSystem::AccountForClient(uint32_t clientId) const
{
    auto it = m_sessions.find(clientId);
    return it != m_sessions.end() ? it->second : 0;
}

void TFAccountSystem::ClearSession(uint32_t clientId)
{
    m_sessions.erase(clientId);
}

} // namespace Terrafront

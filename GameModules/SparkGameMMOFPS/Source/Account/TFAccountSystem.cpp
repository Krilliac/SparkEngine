/**
 * @file TFAccountSystem.cpp
 * @brief TERRAFRONT account register/login core logic (W5 onboarding, Task 2).
 *
 * Kept minimal-dependency (TFDatabase.h + stdlib) so it links standalone
 * into SparkTests without pulling in TFGameContext or the engine module
 * scaffolding.
 */
#include "Account/TFAccountSystem.h"
#include "Account/TFCrypto.h"

#include <chrono>
#include <random>
#include <sstream>
#include <vector>

namespace Terrafront {

// === Hashing utilities ===
//
// CUTOVER (2026-07-06): replaced the demo-grade iterated-std::hash scheme
// (100k rounds of std::hash<std::string>, STL-vendor-dependent, no
// preimage/collision guarantees) with real PBKDF2-HMAC-SHA256 (RFC 8018),
// implemented self-contained in TFCrypto.{h,cpp} (SparkEngine's
// NetworkEncryption.h is an XOR toy cipher, not cryptographically suitable).
// This is a dev codebase with no real users: the cutover is a clean break --
// old std::hash-format hashes simply fail VerifyPassword (Login returns
// BadCredentials; affected dev/test accounts just re-register). This does
// NOT touch GameModules/SparkGameMMO/Source/Account/MMOAccountSystem.cpp,
// which is a separate module/scheme out of scope for this pass.
//
// Stored format: "pbkdf2-sha256$<iterations>$<saltHex>$<dkHex>" -- iterations
// and salt travel with the hash so future parameter bumps (e.g. raising
// kPbkdf2Iterations) don't invalidate hashes minted under the old count;
// VerifyPassword re-derives using whatever params are embedded in the
// stored string, not the current compile-time default.

namespace {
constexpr uint32_t kPbkdf2Iterations = 150000;   // >= 100000 floor with headroom
constexpr size_t   kSaltBytes = 16;              // 128-bit salt
constexpr size_t   kDkBytes   = 32;               // 256-bit derived key
constexpr const char* kScheme = "pbkdf2-sha256";

std::vector<uint8_t> RandomBytes(size_t n)
{
    static std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<unsigned int> dist(0, 255);
    std::vector<uint8_t> out(n);
    for (auto& b : out)
        b = static_cast<uint8_t>(dist(rng));
    return out;
}

// Splits on '$' with no regex dependency; e.g. "a$b$c" -> {"a","b","c"}.
std::vector<std::string> SplitScheme(const std::string& s)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (true)
    {
        size_t pos = s.find('$', start);
        parts.push_back(s.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
        if (pos == std::string::npos)
            break;
        start = pos + 1;
    }
    return parts;
}
} // namespace

std::string TFAccountSystem::GenerateSalt()
{
    return Crypto::ToHex(RandomBytes(kSaltBytes));
}

std::string TFAccountSystem::HashPassword(const std::string& password, const std::string& salt)
{
    std::vector<uint8_t> saltBytes = Crypto::FromHex(salt);
    if (saltBytes.empty() && !salt.empty())
    {
        // `salt` wasn't valid hex (e.g. a caller/test passed an arbitrary
        // string rather than a GenerateSalt() output) -- fall back to the
        // raw bytes of the string so HashPassword stays a total, deterministic
        // function of (password, salt) for any input, matching the previous
        // scheme's behavior.
        saltBytes.assign(salt.begin(), salt.end());
    }

    std::vector<uint8_t> dk = Crypto::Pbkdf2HmacSha256(password, saltBytes, kPbkdf2Iterations, kDkBytes);

    std::ostringstream ss;
    ss << kScheme << '$' << kPbkdf2Iterations << '$' << salt << '$' << Crypto::ToHex(dk);
    return ss.str();
}

bool TFAccountSystem::VerifyPassword(const std::string& password, const std::string& storedHash)
{
    std::vector<std::string> parts = SplitScheme(storedHash);
    if (parts.size() != 4 || parts[0] != kScheme)
        return false;   // legacy (old std::hash) or unrecognized format -> caller treats as bad credentials

    uint32_t iterations = 0;
    try
    {
        unsigned long parsed = std::stoul(parts[1]);
        iterations = static_cast<uint32_t>(parsed);
    }
    catch (...)
    {
        return false;
    }
    if (iterations == 0)
        return false;

    const std::string& saltHex = parts[2];
    const std::string& dkHex   = parts[3];

    std::vector<uint8_t> saltBytes  = Crypto::FromHex(saltHex);
    std::vector<uint8_t> expectedDk = Crypto::FromHex(dkHex);
    if (expectedDk.empty())
        return false;

    std::vector<uint8_t> actualDk = Crypto::Pbkdf2HmacSha256(password, saltBytes, iterations, expectedDk.size());
    return Crypto::ConstantTimeEquals(actualDk, expectedDk);
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
    if (!m_db->FindAccountByUsername(username, rec) || !VerifyPassword(password, rec.passwordHash))
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

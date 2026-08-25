/**
 * @file TFAccountSystem.h
 * @brief TERRAFRONT account register/login core logic (W5 onboarding, Task 2).
 *
 * Core logic only: operates directly on a `TFDatabase*` (no TFGameContext),
 * so it is unit-testable standalone. The client-id -> account-id session map
 * is plain in-memory bookkeeping consumed by the session layer (Task 4).
 */
#pragma once

#include "Persistence/TFDatabase.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace Terrafront
{

    enum class TFAuthErr : uint8_t
    {
        Ok = 0,
        BadCredentials,
        UsernameTaken,
        UsernameTooShort,
        PasswordTooShort,
        ServerError,
        NotLoggedIn,
        SessionActive
    };

    struct TFAuthResult
    {
        bool ok = false;
        TFAuthErr err = TFAuthErr::ServerError;
        uint64_t accountId = 0;
    };

    class TFAccountSystem
    {
      public:
        void SetDatabase(TFDatabase* db) { m_db = db; } // core logic uses the db directly (unit-testable)

        TFAuthResult Register(const std::string& username, const std::string& password); // min length 3
        TFAuthResult Login(const std::string& username, const std::string& password);

        // session layer (Task 4): bind a connection to an account
        void BindSession(uint32_t clientId, uint64_t accountId);
        uint64_t AccountForClient(uint32_t clientId) const; // 0 if not logged in
        void ClearSession(uint32_t clientId);

        static std::string GenerateSalt(); // >=16 random bytes, hex-encoded
        static std::string HashPassword(const std::string& password,
                                        const std::string& salt); // self-describing pbkdf2-sha256$iters$salt$dk string
        static bool VerifyPassword(const std::string& password,
                                   const std::string& storedHash); // constant-time; false on legacy/unknown format

      private:
        TFDatabase* m_db = nullptr;
        std::unordered_map<uint32_t, uint64_t> m_sessions; // clientId -> accountId
    };

} // namespace Terrafront

#include "TestFramework.h"
#include "Account/MMOAccountSystem.h"
#include "Utils/SecureMemory.h"

#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace
{
    using PasswordBuffer = Spark::SensitiveCharBuffer<64>;

    void Fill(PasswordBuffer& buffer, std::string_view value)
    {
        std::memcpy(buffer.data(), value.data(), value.size());
        buffer.data()[value.size()] = '\0';
    }

    void SimulateRejectedSubmit(PasswordBuffer& buffer)
    {
        const auto clearPassword = buffer.ClearOnScopeExit();
        return; // regression: early exits still erase the full backing array
    }
} // namespace

static_assert(!std::is_copy_constructible_v<PasswordBuffer>);
static_assert(!std::is_move_constructible_v<PasswordBuffer>);

TEST(MMOCredentials_SensitiveBufferClearsNormalAndEarlyExitPaths)
{
    PasswordBuffer password;
    Fill(password, "normal-path-secret");
    {
        const auto clearPassword = password.ClearOnScopeExit();
        EXPECT_EQ(password.View(), std::string_view("normal-path-secret"));
    }
    EXPECT_TRUE(password.IsCleared());

    Fill(password, "failure-path-secret");
    SimulateRejectedSubmit(password);
    EXPECT_TRUE(password.IsCleared());
}

TEST(MMOCredentials_AccountBorrowsPasswordAndPersistsOnlyHash)
{
    MMO::MMOAccountSystem accounts;
    EXPECT_TRUE(accounts.Initialize(nullptr));

    constexpr std::string_view plaintext = "unique-mmo-credential";
    PasswordBuffer password;
    Fill(password, plaintext);

    MMO::AuthResult registration;
    {
        const auto clearPassword = password.ClearOnScopeExit();
        registration = accounts.Register("credential_user", password.View());
    }
    EXPECT_TRUE(registration.success);
    EXPECT_TRUE(password.IsCleared());

    const auto account = accounts.GetAccount(registration.accountId);
    EXPECT_TRUE(account.has_value());
    if (account)
    {
        EXPECT_FALSE(account->passwordHash.empty());
        EXPECT_TRUE(account->passwordHash.find(plaintext) == std::string::npos);
        EXPECT_TRUE(account->username != plaintext);
        EXPECT_TRUE(account->email != plaintext);
        EXPECT_TRUE(account->salt != plaintext);
        EXPECT_TRUE(account->banReason != plaintext);
    }

    accounts.Shutdown();
}

TEST(MMOCredentials_SecureClearOverwritesOwnedStringAndVectorStorage)
{
    char rawSecret[] = "raw-buffer-secret";
    Spark::SecureErase(rawSecret, sizeof(rawSecret));
    for (const char value : rawSecret)
        EXPECT_EQ(value, '\0');

    std::string secret = "owned-string-secret";
    Spark::SecureClear(secret);
    EXPECT_TRUE(secret.empty());

    std::vector<std::string> copies{"first-secret", "second-secret"};
    Spark::SecureClear(copies);
    EXPECT_TRUE(copies.empty());

    std::vector<uint8_t> wireCopy{'w', 'i', 'r', 'e', '-', 's', 'e', 'c', 'r', 'e', 't'};
    Spark::SecureClear(wireCopy);
    EXPECT_TRUE(wireCopy.empty());
}

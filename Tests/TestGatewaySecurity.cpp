/** @file TestGatewaySecurity.cpp @brief HMAC admission and replay tests. */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#endif

#include "TestFramework.h"
#include "GatewaySecurity.h"
#include "Utils/SecureRandom.h"

#include <chrono>
#include <filesystem>
#include <vector>

using namespace Spark::Gateway;

namespace
{
    int64_t NowMilliseconds()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    AdmissionRequest Request()
    {
        AdmissionRequest request;
        request.clientId = 42;
        request.sessionId = "session-42";
        request.playerName = "Player";
        return request;
    }
} // namespace

TEST(GatewaySecurity_AcceptsOnceAndRejectsReplay)
{
    KeyFileAuthenticator authenticator(std::vector<uint8_t>(32, 0x5a));
    AdmissionRequest request = Request();
    request.credential = authenticator.CreateCredential(request, NowMilliseconds(), 123456);
    EXPECT_TRUE(authenticator.Authenticate(request).accepted);
    EXPECT_FALSE(authenticator.Authenticate(request).accepted);
}

TEST(GatewaySecurity_RejectsTamperingAndExpiredTimestamp)
{
    KeyFileAuthenticator authenticator(std::vector<uint8_t>(32, 0x6b), std::chrono::seconds(2));
    AdmissionRequest request = Request();
    request.credential = authenticator.CreateCredential(request, NowMilliseconds(), 222);
    request.playerName = "Tampered";
    EXPECT_FALSE(authenticator.Authenticate(request).accepted);

    request = Request();
    request.credential = authenticator.CreateCredential(request, NowMilliseconds() - 5000, 333);
    EXPECT_FALSE(authenticator.Authenticate(request).accepted);
}

TEST(GatewaySecurity_BindsAuthoritativeSpawnPosition)
{
    KeyFileAuthenticator authenticator(std::vector<uint8_t>(32, 0x7c));
    AdmissionRequest request = Request();
    request.spawnPosition = {10.0f, 20.0f, -30.0f};
    request.credential = authenticator.CreateCredential(request, NowMilliseconds(), 444);

    request.spawnPosition.x = 11.0f;
    EXPECT_FALSE(authenticator.Authenticate(request).accepted);
}

TEST(GatewaySecurity_RejectsWeakKey)
{
    KeyFileAuthenticator authenticator(std::vector<uint8_t>(31, 0x11));
    EXPECT_FALSE(authenticator.IsReady());
}

TEST(GatewaySecurity_AcceptsOwnerOnlyGeneratedKeyFile)
{
    const std::filesystem::path root = std::filesystem::temp_directory_path() / Spark::SecureRandom::HexToken(12);
    const std::filesystem::path keyFile = root / "gateway.key";
    const std::string token = Spark::SecureRandom::HexToken(32);
    std::string error;
    EXPECT_TRUE(Spark::SecureRandom::CreatePrivateFile(keyFile, token + "\n", &error));

    std::vector<uint8_t> key;
    EXPECT_TRUE(LoadPrivateGatewayKey(keyFile, key, error));
    EXPECT_EQ(key.size(), token.size() / 2);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

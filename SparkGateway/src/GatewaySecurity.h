/**
 * @file GatewaySecurity.h
 * @brief Local key-file authentication with replay-resistant HMAC credentials.
 */
#pragma once

#include "GatewayCoordinator.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Gateway
{
    inline constexpr uint16_t GatewayProtocolMajor = 1;
    inline constexpr uint16_t GatewayProtocolMinor = 0;
    inline constexpr size_t GatewayMaximumBodySize = 4096;
    inline constexpr size_t GatewayMaximumCredentialSize = 512;

    /** Loads a >=256-bit secret and rejects key files readable by other users. */
    [[nodiscard]] bool LoadPrivateGatewayKey(const std::filesystem::path& path, std::vector<uint8_t>& key,
                                             std::string& error);
    [[nodiscard]] std::vector<uint8_t> ComputeGatewayMac(std::span<const uint8_t> key, std::string_view data);
    [[nodiscard]] bool VerifyGatewayMac(std::span<const uint8_t> key, std::string_view data,
                                        std::span<const uint8_t> supplied);

    /** Production local authenticator. Credential: v1.<unix-ms>.<nonce>.<hmac-sha256-hex>. */
    class KeyFileAuthenticator final : public IGatewayAuthenticator
    {
      public:
        explicit KeyFileAuthenticator(const std::filesystem::path& keyFile,
                                      std::chrono::milliseconds replayWindow = std::chrono::seconds(60));
        explicit KeyFileAuthenticator(std::vector<uint8_t> key,
                                      std::chrono::milliseconds replayWindow = std::chrono::seconds(60));
        ~KeyFileAuthenticator() override;

        [[nodiscard]] AuthenticationResult Authenticate(const AdmissionRequest& request) override;
        [[nodiscard]] bool IsReady() const override { return m_key.size() >= 32; }
        [[nodiscard]] const std::string& Error() const { return m_error; }

        [[nodiscard]] std::string CreateCredential(const AdmissionRequest& request, int64_t unixMilliseconds,
                                                   uint64_t nonce) const;

      private:
        [[nodiscard]] std::string CanonicalPayload(const AdmissionRequest& request, int64_t unixMilliseconds,
                                                   uint64_t nonce) const;
        void PruneReplays(int64_t nowMilliseconds);

        std::vector<uint8_t> m_key;
        std::chrono::milliseconds m_replayWindow;
        std::string m_error;
        std::unordered_map<uint64_t, int64_t> m_seenNonces;
        std::mutex m_mutex;
    };
} // namespace Spark::Gateway

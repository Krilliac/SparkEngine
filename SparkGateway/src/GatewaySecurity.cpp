#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <aclapi.h>
#else
#include <sys/stat.h>
#endif

#include "GatewaySecurity.h"
#include "Utils/PasswordHash.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace Spark::Gateway
{
    namespace
    {
        std::string Hex(std::span<const uint8_t> bytes)
        {
            std::ostringstream stream;
            stream << std::hex << std::setfill('0');
            for (const uint8_t byte : bytes)
                stream << std::setw(2) << static_cast<unsigned int>(byte);
            return stream.str();
        }

        bool DecodeHex(std::string_view text, std::vector<uint8_t>& bytes)
        {
            if (text.size() % 2 != 0)
                return false;
            bytes.clear();
            bytes.reserve(text.size() / 2);
            for (size_t index = 0; index < text.size(); index += 2)
            {
                unsigned int value = 0;
                const auto [end, error] = std::from_chars(text.data() + index, text.data() + index + 2, value, 16);
                if (error != std::errc{} || end != text.data() + index + 2)
                    return false;
                bytes.push_back(static_cast<uint8_t>(value));
            }
            return true;
        }

        std::vector<uint8_t> HmacSha256(std::span<const uint8_t> key, std::string_view data)
        {
            const auto digest = Spark::PasswordHash::ComputeHmacSha256(
                key, std::span(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
            return {digest.begin(), digest.end()};
        }

        bool ConstantTimeEqual(std::span<const uint8_t> left, std::span<const uint8_t> right)
        {
            if (left.size() != right.size())
                return false;
            uint8_t difference = 0;
            for (size_t index = 0; index < left.size(); ++index)
                difference |= left[index] ^ right[index];
            return difference == 0;
        }

        void AppendUint32(std::string& output, uint32_t value)
        {
            for (int shift = 24; shift >= 0; shift -= 8)
                output.push_back(static_cast<char>((value >> shift) & 0xffu));
        }

        void AppendUint64(std::string& output, uint64_t value)
        {
            for (int shift = 56; shift >= 0; shift -= 8)
                output.push_back(static_cast<char>((value >> shift) & 0xffu));
        }

        void AppendString(std::string& output, std::string_view value)
        {
            AppendUint64(output, static_cast<uint64_t>(value.size()));
            output.append(value);
        }

        bool HasPrivatePermissions(const std::filesystem::path& path, std::string& error)
        {
#ifdef _WIN32
            PACL dacl = nullptr;
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            PSID owner = nullptr;
            const DWORD status = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT,
                                                       OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
                                                       nullptr, &dacl, nullptr, &descriptor);
            if (status != ERROR_SUCCESS || !owner || !dacl)
            {
                if (descriptor)
                    LocalFree(descriptor);
                error = "Cannot inspect gateway key ACL";
                return false;
            }
            ACL_SIZE_INFORMATION information{};
            bool privateAcl = GetAclInformation(dacl, &information, sizeof(information), AclSizeInformation) != FALSE;
            for (DWORD index = 0; privateAcl && index < information.AceCount; ++index)
            {
                void* rawAce = nullptr;
                if (!GetAce(dacl, index, &rawAce))
                {
                    privateAcl = false;
                    break;
                }
                const auto* header = static_cast<ACE_HEADER*>(rawAce);
                if (header->AceType == ACCESS_DENIED_ACE_TYPE)
                    continue;
                if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
                {
                    privateAcl = false;
                    break;
                }
                const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(rawAce);
                const PSID trustee = const_cast<DWORD*>(&ace->SidStart);
                if (!EqualSid(owner, trustee))
                    privateAcl = false;
            }
            LocalFree(descriptor);
            if (!privateAcl)
                error = "Gateway key ACL must grant read access only to its owner";
            return privateAcl;
#else
            struct stat info{};
            if (::stat(path.c_str(), &info) != 0 || (info.st_mode & 077) != 0)
            {
                error = "Gateway key permissions must be 0600 or stricter";
                return false;
            }
            return true;
#endif
        }
    } // namespace

    bool LoadPrivateGatewayKey(const std::filesystem::path& path, std::vector<uint8_t>& key, std::string& error)
    {
        key.clear();
        error.clear();
        if (path.empty() || !HasPrivatePermissions(path, error))
            return false;
        std::ifstream file(path, std::ios::binary);
        std::string contents((std::istreambuf_iterator<char>(file)), {});
        while (!contents.empty() && (contents.back() == '\r' || contents.back() == '\n'))
            contents.pop_back();
        if (!file && !file.eof())
        {
            error = "Cannot read gateway key file";
            return false;
        }
        if (contents.size() == 64 && DecodeHex(contents, key))
        {
        }
        else
            key.assign(contents.begin(), contents.end());
        if (key.size() < 32 || key.size() > GatewayMaximumBodySize)
        {
            key.clear();
            error = "Gateway key must contain between 32 and 4096 bytes";
            return false;
        }
        return true;
    }

    std::vector<uint8_t> ComputeGatewayMac(std::span<const uint8_t> key, std::string_view data)
    {
        return HmacSha256(key, data);
    }

    bool VerifyGatewayMac(std::span<const uint8_t> key, std::string_view data, std::span<const uint8_t> supplied)
    {
        return ConstantTimeEqual(HmacSha256(key, data), supplied);
    }

    KeyFileAuthenticator::KeyFileAuthenticator(const std::filesystem::path& keyFile,
                                               std::chrono::milliseconds replayWindow)
        : m_replayWindow(replayWindow)
    {
        (void)LoadPrivateGatewayKey(keyFile, m_key, m_error);
    }

    KeyFileAuthenticator::KeyFileAuthenticator(std::vector<uint8_t> key, std::chrono::milliseconds replayWindow)
        : m_key(std::move(key)), m_replayWindow(replayWindow)
    {
        if (m_key.size() < 32)
        {
            m_error = "Gateway key must contain at least 32 bytes";
            m_key.clear();
        }
    }

    AuthenticationResult KeyFileAuthenticator::Authenticate(const AdmissionRequest& request)
    {
        if (!IsReady())
            return {false, {}, m_error.empty() ? "Gateway key is unavailable" : m_error};
        const size_t first = request.credential.find('.');
        const size_t second = request.credential.find('.', first == std::string::npos ? first : first + 1);
        const size_t third = request.credential.find('.', second == std::string::npos ? second : second + 1);
        if (first == std::string::npos || second == std::string::npos || third == std::string::npos ||
            request.credential.substr(0, first) != "v1")
            return {false, {}, "Malformed gateway credential"};
        int64_t timestamp = 0;
        uint64_t nonce = 0;
        const auto timestampText = std::string_view(request.credential).substr(first + 1, second - first - 1);
        const auto nonceText = std::string_view(request.credential).substr(second + 1, third - second - 1);
        if (std::from_chars(timestampText.data(), timestampText.data() + timestampText.size(), timestamp).ec !=
                std::errc{} ||
            std::from_chars(nonceText.data(), nonceText.data() + nonceText.size(), nonce).ec != std::errc{})
            return {false, {}, "Malformed gateway credential"};
        std::vector<uint8_t> supplied;
        if (!DecodeHex(std::string_view(request.credential).substr(third + 1), supplied) || supplied.size() != 32)
            return {false, {}, "Malformed gateway credential"};
        const int64_t now =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        if (timestamp < now - m_replayWindow.count() || timestamp > now + m_replayWindow.count())
            return {false, {}, "Expired gateway credential"};
        const auto expected = HmacSha256(m_key, CanonicalPayload(request, timestamp, nonce));
        if (expected.empty() || !ConstantTimeEqual(expected, supplied))
            return {false, {}, "Invalid gateway credential"};
        std::lock_guard lock(m_mutex);
        PruneReplays(now);
        if (m_seenNonces.contains(nonce))
            return {false, {}, "Replayed gateway credential"};
        m_seenNonces.emplace(nonce, timestamp);
        return {true, request.sessionId, {}};
    }

    std::string KeyFileAuthenticator::CreateCredential(const AdmissionRequest& request, int64_t unixMilliseconds,
                                                       uint64_t nonce) const
    {
        const auto digest = HmacSha256(m_key, CanonicalPayload(request, unixMilliseconds, nonce));
        if (digest.empty())
            return {};
        return "v1." + std::to_string(unixMilliseconds) + "." + std::to_string(nonce) + "." + Hex(digest);
    }

    std::string KeyFileAuthenticator::CanonicalPayload(const AdmissionRequest& request, int64_t unixMilliseconds,
                                                       uint64_t nonce) const
    {
        std::string payload;
        payload.reserve(96 + request.sessionId.size() + request.playerName.size());
        AppendString(payload, "SparkGateway/1.0");
        AppendUint64(payload, std::bit_cast<uint64_t>(unixMilliseconds));
        AppendUint64(payload, nonce);
        AppendUint64(payload, static_cast<uint64_t>(request.clientId));
        AppendString(payload, request.sessionId);
        AppendString(payload, request.playerName);
        AppendUint32(payload, std::bit_cast<uint32_t>(request.spawnPosition.x));
        AppendUint32(payload, std::bit_cast<uint32_t>(request.spawnPosition.y));
        AppendUint32(payload, std::bit_cast<uint32_t>(request.spawnPosition.z));
        return payload;
    }

    void KeyFileAuthenticator::PruneReplays(int64_t nowMilliseconds)
    {
        std::erase_if(m_seenNonces,
                      [&](const auto& entry) { return entry.second < nowMilliseconds - m_replayWindow.count(); });
    }
} // namespace Spark::Gateway

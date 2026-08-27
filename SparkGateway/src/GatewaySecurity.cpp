#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <aclapi.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "GatewaySecurity.h"
#include "Utils/PasswordHash.h"
#include "Utils/ScopeGuard.h"
#include "Utils/SecureMemory.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>

namespace Spark::Gateway
{
    namespace
    {
        constexpr size_t GatewayMaximumRawKeySize = GatewayMaximumBodySize + 2;
        constexpr size_t GatewayReadBufferSize = GatewayMaximumRawKeySize + 1;

        void AppendHex(std::string& output, std::span<const uint8_t> bytes)
        {
            constexpr char HexDigits[] = "0123456789abcdef";
            output.reserve(output.size() + bytes.size() * 2);
            for (const uint8_t& byte : bytes)
            {
                output.push_back(HexDigits[byte >> 4]);
                output.push_back(HexDigits[byte & 0x0fu]);
            }
        }

        bool DecodeHex(std::string_view text, std::vector<uint8_t>& bytes)
        {
            Spark::SecureClear(bytes);
            if (text.size() % 2 != 0)
                return false;
            bytes.reserve(text.size() / 2);
            for (size_t index = 0; index < text.size(); index += 2)
            {
                unsigned int value = 0;
                const auto [end, error] = std::from_chars(text.data() + index, text.data() + index + 2, value, 16);
                if (error != std::errc{} || end != text.data() + index + 2)
                {
                    Spark::SecureClear(bytes);
                    return false;
                }
                bytes.push_back(static_cast<uint8_t>(value));
            }
            return true;
        }

        Spark::PasswordHash::Sha256Digest HmacSha256(std::span<const uint8_t> key, std::string_view data)
        {
            return Spark::PasswordHash::ComputeHmacSha256(
                key, std::span(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
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

#ifdef _WIN32
        bool OwnerMatchesCurrentProcessUser(PSID owner, std::string& error)
        {
            HANDLE token = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            {
                error = "Cannot inspect current process user";
                return false;
            }
            const auto closeToken = Spark::MakeScopeExit([token] { CloseHandle(token); });

            DWORD requiredSize = 0;
            if (GetTokenInformation(token, TokenUser, nullptr, 0, &requiredSize) != FALSE ||
                GetLastError() != ERROR_INSUFFICIENT_BUFFER || requiredSize == 0)
            {
                error = "Cannot inspect current process user";
                return false;
            }
            std::vector<uint8_t> tokenUserBuffer(requiredSize);
            if (!GetTokenInformation(token, TokenUser, tokenUserBuffer.data(), requiredSize, &requiredSize))
            {
                error = "Cannot inspect current process user";
                return false;
            }
            const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(tokenUserBuffer.data());
            if (!tokenUser->User.Sid || !IsValidSid(tokenUser->User.Sid))
            {
                error = "Cannot inspect current process user";
                return false;
            }
            if (!EqualSid(owner, tokenUser->User.Sid))
            {
                error = "Gateway key file must be owned by the current process user";
                return false;
            }
            return true;
        }

        bool HasPrivatePermissions(HANDLE file, std::string& error)
        {
            PACL dacl = nullptr;
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            PSID owner = nullptr;
            const DWORD status =
                GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
                                nullptr, &dacl, nullptr, &descriptor);
            const auto freeDescriptor = Spark::MakeScopeExit(
                [&]
                {
                    if (descriptor)
                        LocalFree(descriptor);
                });
            if (status != ERROR_SUCCESS || !owner || !IsValidSid(owner) || !dacl)
            {
                error = "Cannot inspect gateway key ACL";
                return false;
            }
            if (!OwnerMatchesCurrentProcessUser(owner, error))
                return false;
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
            if (!privateAcl)
                error = "Gateway key ACL must grant read access only to its owner";
            return privateAcl;
        }

        bool ReadValidatedPrivateFile(const std::filesystem::path& path, std::span<char> contents, size_t& contentsSize,
                                      std::string& error)
        {
            contentsSize = 0;
            const HANDLE file =
                CreateFileW(path.c_str(), GENERIC_READ | READ_CONTROL, 0, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                error = "Cannot open gateway key file";
                return false;
            }
            const auto closeFile = Spark::MakeScopeExit([file] { CloseHandle(file); });

            BY_HANDLE_FILE_INFORMATION information{};
            if (!GetFileInformationByHandle(file, &information) ||
                (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
                information.nNumberOfLinks != 1)
            {
                error = "Gateway key must be a regular, single-link file";
                return false;
            }
            ULARGE_INTEGER fileSize{};
            fileSize.HighPart = information.nFileSizeHigh;
            fileSize.LowPart = information.nFileSizeLow;
            if (fileSize.QuadPart > GatewayMaximumRawKeySize || !HasPrivatePermissions(file, error))
            {
                if (error.empty())
                    error = "Gateway key file is too large";
                return false;
            }

            for (;;)
            {
                DWORD bytesRead = 0;
                const DWORD available = static_cast<DWORD>(contents.size() - contentsSize);
                if (!ReadFile(file, contents.data() + contentsSize, available, &bytesRead, nullptr))
                {
                    error = "Cannot read gateway key file";
                    return false;
                }
                if (bytesRead == 0)
                    return true;
                contentsSize += bytesRead;
                if (contentsSize > GatewayMaximumRawKeySize)
                {
                    error = "Gateway key file is too large";
                    return false;
                }
            }
        }
#else
        bool HasPrivatePermissions(const struct stat& information, std::string& error)
        {
            if (information.st_uid != geteuid())
            {
                error = "Gateway key file must be owned by the current process user";
                return false;
            }
            if ((information.st_mode & 077) != 0)
            {
                error = "Gateway key permissions must be 0600 or stricter";
                return false;
            }
            return true;
        }

        bool ReadValidatedPrivateFile(const std::filesystem::path& path, std::span<char> contents, size_t& contentsSize,
                                      std::string& error)
        {
            contentsSize = 0;
            const int file = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
            if (file < 0)
            {
                error = "Cannot open gateway key file";
                return false;
            }
            const auto closeFile = Spark::MakeScopeExit([file] { ::close(file); });

            struct stat info{};
            if (::fstat(file, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1)
            {
                error = "Gateway key must be a regular, single-link file";
                return false;
            }
            if (info.st_size < 0 || static_cast<uint64_t>(info.st_size) > GatewayMaximumRawKeySize ||
                !HasPrivatePermissions(info, error))
            {
                if (error.empty())
                    error = "Gateway key file is too large";
                return false;
            }

            for (;;)
            {
                const ssize_t bytesRead = ::read(file, contents.data() + contentsSize, contents.size() - contentsSize);
                if (bytesRead == 0)
                    return true;
                if (bytesRead < 0)
                {
                    if (errno == EINTR)
                        continue;
                    error = "Cannot read gateway key file";
                    return false;
                }
                contentsSize += static_cast<size_t>(bytesRead);
                if (contentsSize > GatewayMaximumRawKeySize)
                {
                    error = "Gateway key file is too large";
                    return false;
                }
            }
        }
#endif
    } // namespace

    bool LoadPrivateGatewayKey(const std::filesystem::path& path, std::vector<uint8_t>& key, std::string& error)
    {
        Spark::SecureClear(key);
        error.clear();
        if (path.empty())
        {
            error = "Cannot open gateway key file";
            return false;
        }

        bool loaded = false;
        const auto clearKeyOnFailure = Spark::MakeScopeExit(
            [&]
            {
                if (!loaded)
                    Spark::SecureClear(key);
            });
        std::array<char, GatewayReadBufferSize> contents{};
        const auto clearContents = Spark::MakeScopeExit([&] { Spark::SecureErase(contents.data(), contents.size()); });
        size_t contentsSize = 0;
        if (!ReadValidatedPrivateFile(path, contents, contentsSize, error))
            return false;
        while (contentsSize > 0 && (contents[contentsSize - 1] == '\r' || contents[contentsSize - 1] == '\n'))
            --contentsSize;
        const std::string_view keyText(contents.data(), contentsSize);
        if (keyText.size() == 64 && DecodeHex(keyText, key))
        {
        }
        else
            key.assign(keyText.begin(), keyText.end());
        if (key.size() < 32 || key.size() > GatewayMaximumBodySize)
        {
            error = "Gateway key must contain between 32 and 4096 bytes";
            return false;
        }
        loaded = true;
        return true;
    }

    std::vector<uint8_t> ComputeGatewayMac(std::span<const uint8_t> key, std::string_view data)
    {
        auto digest = HmacSha256(key, data);
        const auto clearDigest = Spark::MakeScopeExit([&] { Spark::SecureErase(digest.data(), digest.size()); });
        return {digest.begin(), digest.end()};
    }

    bool VerifyGatewayMac(std::span<const uint8_t> key, std::string_view data, std::span<const uint8_t> supplied)
    {
        auto expected = HmacSha256(key, data);
        const auto clearExpected = Spark::MakeScopeExit([&] { Spark::SecureErase(expected.data(), expected.size()); });
        return ConstantTimeEqual(expected, supplied);
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
            Spark::SecureClear(m_key);
        }
    }

    KeyFileAuthenticator::~KeyFileAuthenticator()
    {
        Spark::SecureClear(m_key);
    }

    AuthenticationResult KeyFileAuthenticator::Authenticate(const AdmissionRequest& request)
    {
        if (!IsReady())
            return {false, {}, m_error.empty() ? "Gateway key is unavailable" : m_error};
        const std::string_view credential(request.credential);
        if (credential.size() > GatewayMaximumCredentialSize)
            return {false, {}, "Malformed gateway credential"};
        const size_t first = credential.find('.');
        const size_t second = credential.find('.', first == std::string::npos ? first : first + 1);
        const size_t third = credential.find('.', second == std::string::npos ? second : second + 1);
        if (first == std::string::npos || second == std::string::npos || third == std::string::npos ||
            credential.substr(0, first) != "v1")
            return {false, {}, "Malformed gateway credential"};
        int64_t timestamp = 0;
        uint64_t nonce = 0;
        const auto timestampText = credential.substr(first + 1, second - first - 1);
        const auto nonceText = credential.substr(second + 1, third - second - 1);
        const auto [timestampEnd, timestampError] =
            std::from_chars(timestampText.data(), timestampText.data() + timestampText.size(), timestamp);
        const auto [nonceEnd, nonceError] =
            std::from_chars(nonceText.data(), nonceText.data() + nonceText.size(), nonce);
        if (timestampError != std::errc{} || timestampEnd != timestampText.data() + timestampText.size() ||
            nonceError != std::errc{} || nonceEnd != nonceText.data() + nonceText.size())
            return {false, {}, "Malformed gateway credential"};
        std::vector<uint8_t> supplied;
        const auto clearSupplied = Spark::MakeScopeExit([&] { Spark::SecureClear(supplied); });
        if (!DecodeHex(credential.substr(third + 1), supplied) || supplied.size() != 32)
            return {false, {}, "Malformed gateway credential"};
        const int64_t now =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        if (timestamp < now - m_replayWindow.count() || timestamp > now + m_replayWindow.count())
            return {false, {}, "Expired gateway credential"};
        std::string payload = CanonicalPayload(request, timestamp, nonce);
        const auto clearPayload = Spark::MakeScopeExit([&] { Spark::SecureClear(payload); });
        auto expected = HmacSha256(m_key, payload);
        const auto clearExpected = Spark::MakeScopeExit([&] { Spark::SecureErase(expected.data(), expected.size()); });
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
        std::string payload = CanonicalPayload(request, unixMilliseconds, nonce);
        const auto clearPayload = Spark::MakeScopeExit([&] { Spark::SecureClear(payload); });
        auto digest = HmacSha256(m_key, payload);
        const auto clearDigest = Spark::MakeScopeExit([&] { Spark::SecureErase(digest.data(), digest.size()); });
        if (digest.empty())
            return {};

        std::string credential;
        const auto clearCredentialOnException = Spark::MakeScopeFail([&] { Spark::SecureClear(credential); });
        credential.reserve(3 + 20 + 1 + 20 + 1 + digest.size() * 2);
        credential.append("v1.");
        credential.append(std::to_string(unixMilliseconds));
        credential.push_back('.');
        credential.append(std::to_string(nonce));
        credential.push_back('.');
        AppendHex(credential, digest);
        return credential;
    }

    std::string KeyFileAuthenticator::CanonicalPayload(const AdmissionRequest& request, int64_t unixMilliseconds,
                                                       uint64_t nonce) const
    {
        std::string payload;
        const auto clearPayloadOnException = Spark::MakeScopeFail([&] { Spark::SecureClear(payload); });
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

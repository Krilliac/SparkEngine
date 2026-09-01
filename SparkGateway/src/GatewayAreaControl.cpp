#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "GatewayAreaControl.h"

#include "Utils/DaemonClient.h"
#include "Utils/DaemonFraming.h"
#include "Utils/ScopeGuard.h"
#include "Utils/SecureMemory.h"
#include "Utils/SecureRandom.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <thread>

namespace Spark::Gateway
{
    namespace
    {
        constexpr uint16_t ResponseFlag = 0x100;
        constexpr auto LocalIoTimeout = std::chrono::seconds(2);
        constexpr auto AtomicReplaceTimeout = std::chrono::milliseconds(1500);

        std::string NormalizeLocalEndpoint(std::string endpoint)
        {
#ifdef _WIN32
            return endpoint;
#else
            if (endpoint.empty() || endpoint.front() == '/')
                return endpoint;
            for (char& character : endpoint)
            {
                if (!std::isalnum(static_cast<unsigned char>(character)) && character != '-' && character != '_')
                    character = '-';
            }
            return "/tmp/" + endpoint + ".sock";
#endif
        }

        bool ReceiveExactUntil(Daemon::NativeSocket socket, void* buffer, size_t length, const std::atomic<bool>& stop,
                               std::chrono::steady_clock::time_point deadline)
        {
            auto* bytes = static_cast<uint8_t*>(buffer);
            size_t received = 0;
            while (received < length && !stop.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline)
            {
#ifdef _WIN32
                DWORD count = 0;
                const DWORD chunk = static_cast<DWORD>(
                    (std::min)(length - received, static_cast<size_t>(std::numeric_limits<DWORD>::max())));
                const BOOL succeeded = ::ReadFile(socket, bytes + received, chunk, &count, nullptr);
                if (succeeded && count > 0)
                {
                    received += static_cast<size_t>(count);
                    continue;
                }
                if (succeeded)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                const DWORD error = ::GetLastError();
                if (error != ERROR_NO_DATA && error != ERROR_PIPE_LISTENING && error != ERROR_PIPE_BUSY)
                    return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
#else
                pollfd descriptor{socket, POLLIN, 0};
                const int polled = ::poll(&descriptor, 1, 50);
                if (polled < 0)
                {
                    if (errno == EINTR)
                        continue;
                    return false;
                }
                if (polled == 0)
                    continue;
                const ssize_t count = ::recv(socket, bytes + received, length - received, 0);
                if (count > 0)
                {
                    received += static_cast<size_t>(count);
                    continue;
                }
                if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                    continue;
                return false;
#endif
            }
            return received == length;
        }

        bool SendExactUntil(Daemon::NativeSocket socket, const void* buffer, size_t length,
                            const std::atomic<bool>& stop, std::chrono::steady_clock::time_point deadline)
        {
            const auto* bytes = static_cast<const uint8_t*>(buffer);
            size_t sent = 0;
            while (sent < length && !stop.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline)
            {
#ifdef _WIN32
                DWORD count = 0;
                const DWORD chunk = static_cast<DWORD>(
                    (std::min)(length - sent, static_cast<size_t>(std::numeric_limits<DWORD>::max())));
                const BOOL succeeded = ::WriteFile(socket, bytes + sent, chunk, &count, nullptr);
                if (succeeded && count > 0)
                {
                    sent += static_cast<size_t>(count);
                    continue;
                }
                if (succeeded)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                const DWORD error = ::GetLastError();
                if (error != ERROR_NO_DATA && error != ERROR_PIPE_BUSY && error != ERROR_PIPE_LISTENING)
                    return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
#else
                pollfd descriptor{socket, POLLOUT, 0};
                const int polled = ::poll(&descriptor, 1, 50);
                if (polled < 0)
                {
                    if (errno == EINTR)
                        continue;
                    return false;
                }
                if (polled == 0)
                    continue;
#ifdef MSG_NOSIGNAL
                constexpr int sendFlags = MSG_NOSIGNAL;
#else
                constexpr int sendFlags = 0;
#endif
                const ssize_t count = ::send(socket, bytes + sent, length - sent, sendFlags);
                if (count > 0)
                {
                    sent += static_cast<size_t>(count);
                    continue;
                }
                if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                    continue;
                return false;
#endif
            }
            return sent == length;
        }

        bool ReceiveFrameUntil(Daemon::NativeSocket socket, Daemon::FrameHeader& header, std::vector<uint8_t>& payload,
                               const std::atomic<bool>& stop)
        {
            const auto deadline = std::chrono::steady_clock::now() + LocalIoTimeout;
            uint8_t encoded[Daemon::kFrameHeaderSize];
            if (!ReceiveExactUntil(socket, encoded, sizeof(encoded), stop, deadline))
                return false;
            header = Daemon::DecodeFrameHeader(encoded);
            if (header.payloadSize > Daemon::kMaxPayloadSize)
                return false;
            payload.resize(header.payloadSize);
            return payload.empty() || ReceiveExactUntil(socket, payload.data(), payload.size(), stop, deadline);
        }

        bool SendFrameUntil(Daemon::NativeSocket socket, Daemon::ServiceId service, uint16_t messageType,
                            const std::vector<uint8_t>& payload, const std::atomic<bool>& stop)
        {
            if (payload.size() > Daemon::kMaxPayloadSize)
                return false;
            Daemon::FrameHeader header{static_cast<uint32_t>(payload.size()), static_cast<uint16_t>(service),
                                       messageType};
            uint8_t encoded[Daemon::kFrameHeaderSize];
            Daemon::EncodeFrameHeader(header, encoded);
            const auto deadline = std::chrono::steady_clock::now() + LocalIoTimeout;
            return SendExactUntil(socket, encoded, sizeof(encoded), stop, deadline) &&
                   (payload.empty() || SendExactUntil(socket, payload.data(), payload.size(), stop, deadline));
        }

#ifdef _WIN32
        bool QueryTokenUser(HANDLE token, std::vector<uint8_t>& storage, TOKEN_USER*& user)
        {
            DWORD required = 0;
            (void)::GetTokenInformation(token, TokenUser, nullptr, 0, &required);
            if (required == 0 || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER)
                return false;
            storage.resize(required);
            if (!::GetTokenInformation(token, TokenUser, storage.data(), required, &required))
                return false;
            user = reinterpret_cast<TOKEN_USER*>(storage.data());
            return true;
        }

        bool SameUserPeer(HANDLE pipe)
        {
            HANDLE clientToken = nullptr;
            HANDLE serverToken = nullptr;
            if (!::ImpersonateNamedPipeClient(pipe))
                return false;
            const bool openedClient = ::OpenThreadToken(::GetCurrentThread(), TOKEN_QUERY, TRUE, &clientToken) != FALSE;
            const bool reverted = ::RevertToSelf() != FALSE;
            const bool openedServer = ::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &serverToken) != FALSE;
            if (!openedClient || !reverted || !openedServer)
            {
                if (clientToken)
                    ::CloseHandle(clientToken);
                if (serverToken)
                    ::CloseHandle(serverToken);
                return false;
            }
            std::vector<uint8_t> clientStorage;
            std::vector<uint8_t> serverStorage;
            TOKEN_USER* clientUser = nullptr;
            TOKEN_USER* serverUser = nullptr;
            const bool queried = QueryTokenUser(clientToken, clientStorage, clientUser) &&
                                 QueryTokenUser(serverToken, serverStorage, serverUser);
            ::CloseHandle(clientToken);
            ::CloseHandle(serverToken);
            return queried && ::EqualSid(clientUser->User.Sid, serverUser->User.Sid) != FALSE;
        }

#else
        bool SameUserPeer(int socket)
        {
#if defined(__linux__)
            ucred credentials{};
            socklen_t length = sizeof(credentials);
            return ::getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0 &&
                   credentials.uid == ::geteuid();
#elif defined(__APPLE__) || defined(__FreeBSD__)
            uid_t user = 0;
            gid_t group = 0;
            return ::getpeereid(socket, &user, &group) == 0 && user == ::geteuid();
#else
            return false;
#endif
        }

        void ConfigureAcceptedSocket(int socket)
        {
#if defined(__APPLE__) && defined(SO_NOSIGPIPE)
            const int enabled = 1;
            (void)::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#else
            (void)socket;
#endif
        }

        bool EndpointIsActive(const std::string& endpoint)
        {
            const int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (probe < 0)
                return true;
            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            std::memcpy(address.sun_path, endpoint.data(), endpoint.size());
            const bool connected = ::connect(probe, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
            const int connectError = errno;
            ::close(probe);
            return connected || (connectError != ECONNREFUSED && connectError != ENOENT);
        }

        int CreateLocalListener(const std::string& rawEndpoint)
        {
            const std::string endpoint = NormalizeLocalEndpoint(rawEndpoint);
            if (endpoint.empty() || endpoint.size() >= sizeof(sockaddr_un{}.sun_path))
            {
                errno = EINVAL;
                return -1;
            }
            struct stat existing{};
            if (::lstat(endpoint.c_str(), &existing) == 0)
            {
                if (!S_ISSOCK(existing.st_mode) || existing.st_uid != ::geteuid())
                {
                    errno = EACCES;
                    return -1;
                }
                if (EndpointIsActive(endpoint))
                {
                    errno = EADDRINUSE;
                    return -1;
                }
                if (::unlink(endpoint.c_str()) != 0)
                    return -1;
            }
            else if (errno != ENOENT)
            {
                return -1;
            }
            const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (listener < 0)
                return -1;
            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            std::memcpy(address.sun_path, endpoint.data(), endpoint.size());
            if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
                ::chmod(endpoint.c_str(), S_IRUSR | S_IWUSR) != 0 || ::listen(listener, 8) != 0)
            {
                const int listenerError = errno;
                ::close(listener);
                ::unlink(endpoint.c_str());
                errno = listenerError;
                return -1;
            }
            const int flags = ::fcntl(listener, F_GETFL, 0);
            if (flags >= 0)
                (void)::fcntl(listener, F_SETFL, flags | O_NONBLOCK);
            return listener;
        }

        void UnlinkOwnedEndpoint(const std::string& endpoint, int listener)
        {
            struct stat pathStatus{};
            struct stat listenerStatus{};
            if (::lstat(endpoint.c_str(), &pathStatus) == 0 && ::fstat(listener, &listenerStatus) == 0 &&
                pathStatus.st_dev == listenerStatus.st_dev && pathStatus.st_ino == listenerStatus.st_ino)
                (void)::unlink(endpoint.c_str());
        }
#endif

        bool AtomicWriteText(const std::filesystem::path& target, std::string_view text)
        {
            if (target.empty())
                return false;
            std::error_code error;
            if (!target.parent_path().empty())
            {
                std::filesystem::create_directories(target.parent_path(), error);
                if (error)
                    return false;
            }
            const auto targetStatus = std::filesystem::symlink_status(target, error);
            if (error && error != std::errc::no_such_file_or_directory)
                return false;
            if (!error && std::filesystem::is_symlink(targetStatus))
                return false;
#ifdef _WIN32
            const auto isTransientReplaceContention = [&](DWORD moveError)
            {
                if (moveError == ERROR_SHARING_VIOLATION || moveError == ERROR_LOCK_VIOLATION)
                    return true;
                if (moveError != ERROR_ACCESS_DENIED)
                    return false;
                HANDLE deleteProbe = ::CreateFileW(
                    target.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (deleteProbe != INVALID_HANDLE_VALUE)
                {
                    ::CloseHandle(deleteProbe);
                    return false;
                }
                const DWORD probeError = ::GetLastError();
                return probeError == ERROR_SHARING_VIOLATION || probeError == ERROR_LOCK_VIOLATION;
            };
            const DWORD targetAttributes = ::GetFileAttributesW(target.c_str());
            if (targetAttributes != INVALID_FILE_ATTRIBUTES && (targetAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                return false;
            std::filesystem::path temporary;
            HANDLE file = INVALID_HANDLE_VALUE;
            for (int attempt = 0; attempt < 8 && file == INVALID_HANDLE_VALUE; ++attempt)
            {
                const std::string token = SecureRandom::HexToken(12);
                if (token.empty())
                    return false;
                temporary = target.string() + ".tmp." + token;
                file = ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT,
                                     nullptr);
                if (file == INVALID_HANDLE_VALUE && ::GetLastError() != ERROR_FILE_EXISTS)
                    return false;
            }
            if (file == INVALID_HANDLE_VALUE)
                return false;
            size_t offset = 0;
            bool wrote = true;
            while (offset < text.size())
            {
                DWORD count = 0;
                const DWORD chunk = static_cast<DWORD>(
                    (std::min)(text.size() - offset, static_cast<size_t>(std::numeric_limits<DWORD>::max())));
                if (!::WriteFile(file, text.data() + offset, chunk, &count, nullptr) || count == 0)
                {
                    wrote = false;
                    break;
                }
                offset += static_cast<size_t>(count);
            }
            if (wrote && !::FlushFileBuffers(file))
                wrote = false;
            ::CloseHandle(file);
            if (!wrote)
            {
                (void)::DeleteFileW(temporary.c_str());
                return false;
            }
            const auto replaceDeadline = std::chrono::steady_clock::now() + AtomicReplaceTimeout;
            while (
                !::MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                const DWORD moveError = ::GetLastError();
                if (!isTransientReplaceContention(moveError) || std::chrono::steady_clock::now() >= replaceDeadline)
                {
                    (void)::DeleteFileW(temporary.c_str());
                    return false;
                }
                const DWORD currentAttributes = ::GetFileAttributesW(target.c_str());
                if (currentAttributes != INVALID_FILE_ATTRIBUTES)
                {
                    if ((currentAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                    {
                        (void)::DeleteFileW(temporary.c_str());
                        return false;
                    }
                }
                else
                {
                    const DWORD attributeError = ::GetLastError();
                    if (attributeError != ERROR_FILE_NOT_FOUND && attributeError != ERROR_PATH_NOT_FOUND)
                    {
                        (void)::DeleteFileW(temporary.c_str());
                        return false;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return true;
#else
            std::filesystem::path temporary;
            int file = -1;
            for (int attempt = 0; attempt < 8 && file < 0; ++attempt)
            {
                const std::string token = SecureRandom::HexToken(12);
                if (token.empty())
                    return false;
                temporary = target.string() + ".tmp." + token;
                file =
                    ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
                if (file < 0 && errno != EEXIST)
                    return false;
            }
            if (file < 0)
                return false;
            size_t offset = 0;
            bool wrote = true;
            while (offset < text.size())
            {
                const ssize_t count = ::write(file, text.data() + offset, text.size() - offset);
                if (count > 0)
                {
                    offset += static_cast<size_t>(count);
                    continue;
                }
                if (count < 0 && errno == EINTR)
                    continue;
                wrote = false;
                break;
            }
            wrote = wrote && ::fsync(file) == 0;
            if (::close(file) != 0)
                wrote = false;
            if (!wrote || ::rename(temporary.c_str(), target.c_str()) != 0)
            {
                (void)::unlink(temporary.c_str());
                return false;
            }
            if (!target.parent_path().empty())
            {
                const int directory = ::open(target.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                if (directory >= 0)
                {
                    (void)::fsync(directory);
                    (void)::close(directory);
                }
            }
            return true;
#endif
        }

        int64_t NowMilliseconds()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        }

        bool SeedNonce(std::atomic<uint64_t>& nonce)
        {
            uint64_t seed = 0;
            if (!SecureRandom::Fill(&seed, sizeof(seed)))
                return false;
            if (seed == 0)
                seed = 1;
            nonce.store(seed, std::memory_order_relaxed);
            return true;
        }

        std::string EndpointFor(const AreaEndpoint& endpoint)
        {
            return "spark-area-control-" + std::to_string(endpoint.area.interServerPort);
        }

        void AppendHex(std::string& output, std::span<const uint8_t> bytes)
        {
            static constexpr char Digits[] = "0123456789abcdef";
            const size_t begin = output.size();
            output.resize(begin + bytes.size() * 2);
            for (size_t index = 0; index < bytes.size(); ++index)
            {
                output[begin + index * 2] = Digits[bytes[index] >> 4];
                output[begin + index * 2 + 1] = Digits[bytes[index] & 0x0f];
            }
        }

        bool DecodeHex(std::string_view text, std::vector<uint8_t>& output)
        {
            Spark::SecureClear(output);
            if (text.size() != 64)
                return false;
            output.resize(32);
            for (size_t index = 0; index < output.size(); ++index)
            {
                unsigned int value = 0;
                const auto [end, error] =
                    std::from_chars(text.data() + index * 2, text.data() + index * 2 + 2, value, 16);
                if (error != std::errc{} || end != text.data() + index * 2 + 2)
                {
                    Spark::SecureClear(output);
                    return false;
                }
                output[index] = static_cast<uint8_t>(value);
            }
            return true;
        }

        std::string SignedBody(AreaControlPhase phase, const HandoffCommand& command, int64_t timestamp, uint64_t nonce)
        {
            return std::to_string(GatewayProtocolMajor) + "\n" + std::to_string(GatewayProtocolMinor) + "\n" +
                   std::to_string(timestamp) + "\n" + std::to_string(nonce) + "\n" +
                   std::to_string(static_cast<unsigned int>(phase)) + "\n" + std::to_string(command.epoch) + "\n" +
                   std::to_string(command.sourceArea) + "\n" + std::to_string(command.targetArea) + "\n" +
                   command.sessionId;
        }

        std::vector<uint8_t> EncodeRequest(std::span<const uint8_t> key, AreaControlPhase phase,
                                           const HandoffCommand& command, int64_t timestamp, uint64_t nonce)
        {
            std::string body = SignedBody(phase, command, timestamp, nonce);
            const auto clearBody = Spark::MakeScopeExit([&] { Spark::SecureClear(body); });
            auto mac = ComputeGatewayMac(key, body);
            const auto clearMac = Spark::MakeScopeExit([&] { Spark::SecureClear(mac); });
            body.push_back('\n');
            AppendHex(body, mac);
            if (body.size() > GatewayMaximumBodySize)
                return {};
            return {body.begin(), body.end()};
        }

        struct DecodedRequest
        {
            AreaControlPhase phase{};
            HandoffCommand command;
            int64_t timestamp = 0;
            uint64_t nonce = 0;
            std::string_view signedBody;
            std::vector<uint8_t> mac;

            ~DecodedRequest() { Spark::SecureClear(mac); }
        };

        bool ParseUnsigned(std::string_view text, uint64_t& value)
        {
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            return error == std::errc{} && end == text.data() + text.size();
        }

        bool DecodeRequest(const std::vector<uint8_t>& payload, DecodedRequest& request)
        {
            Spark::SecureClear(request.mac);
            request.signedBody = {};
            if (payload.empty() || payload.size() > GatewayMaximumBodySize)
                return false;
            const std::string_view text(reinterpret_cast<const char*>(payload.data()), payload.size());
            std::vector<std::string_view> fields;
            size_t begin = 0;
            while (begin <= text.size())
            {
                const size_t end = text.find('\n', begin);
                fields.emplace_back(text.data() + begin, (end == std::string::npos ? text.size() : end) - begin);
                if (end == std::string::npos)
                    break;
                begin = end + 1;
            }
            if (fields.size() != 10 || fields[0] != std::to_string(GatewayProtocolMajor))
                return false;
            uint64_t minor = 0, timestamp = 0, nonce = 0, phase = 0, epoch = 0, source = 0, target = 0;
            if (!ParseUnsigned(fields[1], minor) || minor > GatewayProtocolMinor ||
                !ParseUnsigned(fields[2], timestamp) || !ParseUnsigned(fields[3], nonce) ||
                !ParseUnsigned(fields[4], phase) || phase < 1 ||
                phase > static_cast<uint64_t>(AreaControlPhase::Probe) || !ParseUnsigned(fields[5], epoch) ||
                !ParseUnsigned(fields[6], source) || source > std::numeric_limits<Net::AreaID>::max() ||
                !ParseUnsigned(fields[7], target) || target > std::numeric_limits<Net::AreaID>::max() ||
                fields[8].empty() || fields[8].size() > 128 || !DecodeHex(fields[9], request.mac))
                return false;
            request.phase = static_cast<AreaControlPhase>(phase);
            request.command = {std::string(fields[8]), epoch, static_cast<Net::AreaID>(source),
                               static_cast<Net::AreaID>(target)};
            request.timestamp = static_cast<int64_t>(timestamp);
            request.nonce = nonce;
            const size_t macDelimiter = text.rfind('\n');
            request.signedBody = text.substr(0, macDelimiter);
            return true;
        }
    } // namespace

    LocalAreaControlPlane::LocalAreaControlPlane(const std::filesystem::path& keyFile)
    {
        (void)LoadPrivateGatewayKey(keyFile, m_key, m_error);
        if (!SeedNonce(m_nonce))
        {
            Spark::SecureClear(m_key);
            m_error = "Operating-system secure random source is unavailable";
        }
    }

    LocalAreaControlPlane::LocalAreaControlPlane(std::vector<uint8_t> key) : m_key(std::move(key))
    {
        if (m_key.size() < 32)
            Spark::SecureClear(m_key);
        if (!SeedNonce(m_nonce))
            Spark::SecureClear(m_key);
    }

    LocalAreaControlPlane::~LocalAreaControlPlane()
    {
        Spark::SecureClear(m_key);
    }

    bool LocalAreaControlPlane::IsReady() const
    {
        std::vector<Net::AreaID> ids;
        {
            std::lock_guard lock(m_mutex);
            if (m_key.size() < 32 || m_endpoints.empty())
                return false;
            ids.reserve(m_endpoints.size());
            for (const auto& [id, endpoint] : m_endpoints)
            {
                (void)endpoint;
                ids.push_back(id);
            }
        }
        return std::ranges::all_of(ids, [this](Net::AreaID id) { return IsEndpointReady(id); });
    }

    bool LocalAreaControlPlane::IsEndpointReady(Net::AreaID id) const
    {
        std::string endpoint;
        {
            std::lock_guard lock(m_mutex);
            const auto found = m_endpoints.find(id);
            if (m_key.size() < 32 || found == m_endpoints.end() || found->second.empty())
                return false;
            endpoint = found->second;
        }
        const HandoffCommand probe{"spark-readiness", 0, id, id};
        const auto result = SendToEndpoint(endpoint, AreaControlPhase::Probe, probe);
        return result == HandoffOperationResult::Applied || result == HandoffOperationResult::Duplicate;
    }

    HandoffOperationResult LocalAreaControlPlane::Prepare(const HandoffCommand& command)
    {
        return Send(AreaControlPhase::Prepare, command);
    }
    HandoffOperationResult LocalAreaControlPlane::Transfer(const HandoffCommand& command)
    {
        return Send(AreaControlPhase::Transfer, command);
    }
    HandoffOperationResult LocalAreaControlPlane::Commit(const HandoffCommand& command)
    {
        return Send(AreaControlPhase::Commit, command);
    }
    HandoffOperationResult LocalAreaControlPlane::Acknowledge(const HandoffCommand& command)
    {
        return Send(AreaControlPhase::Acknowledge, command);
    }
    HandoffOperationResult LocalAreaControlPlane::Abort(const HandoffCommand& command)
    {
        return Send(AreaControlPhase::Abort, command);
    }

    void LocalAreaControlPlane::RegisterEndpoint(Net::AreaID id, const AreaEndpoint& endpoint)
    {
        std::lock_guard lock(m_mutex);
        if (endpoint.host == "127.0.0.1")
            m_endpoints[id] = EndpointFor(endpoint);
        else
            m_endpoints[id].clear();
    }

    HandoffOperationResult LocalAreaControlPlane::Send(AreaControlPhase phase, const HandoffCommand& command)
    {
        std::vector<std::string> endpoints;
        {
            std::lock_guard lock(m_mutex);
            const auto source = m_endpoints.find(command.sourceArea);
            const auto target = m_endpoints.find(command.targetArea);
            if (source == m_endpoints.end() || target == m_endpoints.end())
                return HandoffOperationResult::Unavailable;
            endpoints.push_back(source->second);
            if (target->second != source->second)
                endpoints.push_back(target->second);
        }
        bool anyApplied = false;
        for (const std::string& endpoint : endpoints)
        {
            const auto result = SendToEndpoint(endpoint, phase, command);
            if (result == HandoffOperationResult::Rejected || result == HandoffOperationResult::Unavailable)
                return result;
            anyApplied |= result == HandoffOperationResult::Applied;
        }
        return anyApplied ? HandoffOperationResult::Applied : HandoffOperationResult::Duplicate;
    }

    HandoffOperationResult LocalAreaControlPlane::SendToEndpoint(std::string_view endpoint, AreaControlPhase phase,
                                                                 const HandoffCommand& command) const
    {
        const uint64_t nonce = m_nonce.fetch_add(1, std::memory_order_relaxed);
        auto payload = EncodeRequest(m_key, phase, command, NowMilliseconds(), nonce);
        const auto clearPayload = Spark::MakeScopeExit([&] { Spark::SecureClear(payload); });
        if (payload.empty())
            return HandoffOperationResult::Rejected;
        Daemon::DaemonClient client;
        if (!client.Connect(NormalizeLocalEndpoint(std::string(endpoint))))
            return HandoffOperationResult::Unavailable;
        auto response = client.Request(Daemon::ServiceId::Orchestration, static_cast<uint16_t>(phase), payload);
        if (!response || response->messageType != static_cast<uint16_t>(phase) + ResponseFlag ||
            response->payload.size() != 1 ||
            response->payload[0] > static_cast<uint8_t>(HandoffOperationResult::Unavailable))
            return HandoffOperationResult::Unavailable;
        return static_cast<HandoffOperationResult>(response->payload[0]);
    }

    LocalAreaControlService::LocalAreaControlService(std::string endpoint, std::filesystem::path keyFile,
                                                     std::filesystem::path epochStateFile)
        : m_endpoint(std::move(endpoint)), m_keyFile(std::move(keyFile)), m_epochStateFile(std::move(epochStateFile))
    {
    }

    LocalAreaControlService::LocalAreaControlService(std::string endpoint, std::vector<uint8_t> key,
                                                     std::filesystem::path epochStateFile)
        : m_endpoint(std::move(endpoint)), m_epochStateFile(std::move(epochStateFile)), m_key(std::move(key))
    {
        if (m_key.size() < 32)
            Spark::SecureClear(m_key);
    }

    LocalAreaControlService::~LocalAreaControlService()
    {
        Stop();
        Spark::SecureClear(m_key);
    }

    bool LocalAreaControlService::Start()
    {
        SetError({});
        if (m_thread.joinable())
        {
            SetError("Gateway area-control service is already running");
            return false;
        }
        if (m_key.empty())
        {
            std::string keyError;
            if (!LoadPrivateGatewayKey(m_keyFile, m_key, keyError))
            {
                SetError("Cannot load gateway area-control key '" + m_keyFile.string() + "': " + keyError);
                return false;
            }
        }
        if (m_key.size() < 32)
        {
            SetError("Gateway area-control key must contain at least 32 bytes");
            return false;
        }
        if (!LoadState())
        {
            SetError("Cannot load gateway area-control epoch state '" + m_epochStateFile.string() +
                     "': corrupt or unreadable");
            return false;
        }
        m_stop.store(false, std::memory_order_release);
        m_ready.store(false, std::memory_order_release);
        m_startupComplete.store(false, std::memory_order_release);
        try
        {
            m_thread = std::thread(&LocalAreaControlService::Run, this);
        }
        catch (const std::system_error& error)
        {
            SetError(std::string("Cannot create gateway area-control worker thread: ") + error.what());
            m_startupComplete.store(true, std::memory_order_release);
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!m_startupComplete.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (IsReady())
            return true;
        if (!m_startupComplete.load(std::memory_order_acquire))
            SetError("Gateway area-control listener startup timed out after 5 seconds");
        Stop();
        return false;
    }

    std::string LocalAreaControlService::GetLastError() const
    {
        std::lock_guard lock(m_errorMutex);
        return m_error;
    }

    void LocalAreaControlService::SetError(std::string error)
    {
        std::lock_guard lock(m_errorMutex);
        m_error = std::move(error);
    }

    void LocalAreaControlService::Stop()
    {
        m_stop.store(true, std::memory_order_release);
#ifdef _WIN32
        const std::wstring pipe = Daemon::NormalizePipeName(NormalizeLocalEndpoint(m_endpoint));
        HANDLE wake = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (wake != INVALID_HANDLE_VALUE)
            CloseHandle(wake);
#endif
        if (m_thread.joinable())
            m_thread.join();
        m_ready.store(false, std::memory_order_release);
    }

    void LocalAreaControlService::Run()
    {
#ifdef _WIN32
        const std::wstring pipeName = Daemon::NormalizePipeName(NormalizeLocalEndpoint(m_endpoint));
        if (pipeName.empty())
        {
            SetError("Gateway area-control endpoint is not valid UTF-8: '" + m_endpoint + "'");
            m_startupComplete.store(true, std::memory_order_release);
            return;
        }
        HANDLE pipe = CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                       1, 5120, 5120, 1000, nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
        {
            const DWORD error = ::GetLastError();
            const std::error_code systemError(static_cast<int>(error), std::system_category());
            SetError("Cannot create gateway area-control named pipe '" + m_endpoint + "': " + systemError.message() +
                     " (Windows error " + std::to_string(error) + ")");
            m_ready.store(false, std::memory_order_release);
            m_startupComplete.store(true, std::memory_order_release);
            return;
        }
        m_ready.store(true, std::memory_order_release);
        m_startupComplete.store(true, std::memory_order_release);
        while (!m_stop.load(std::memory_order_acquire))
        {
            const bool connectedCall = ConnectNamedPipe(pipe, nullptr) != 0;
            const DWORD connectError = connectedCall ? ERROR_SUCCESS : ::GetLastError();
            const bool connected = connectedCall || connectError == ERROR_PIPE_CONNECTED;
            if (!connected)
            {
                if (connectError == ERROR_NO_DATA)
                {
                    DisconnectNamedPipe(pipe);
                    continue;
                }
                if (connectError == ERROR_PIPE_LISTENING)
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (connected && !m_stop.load(std::memory_order_acquire))
            {
                bool responseSent = false;
                Daemon::FrameHeader header{};
                std::vector<uint8_t> payload;
                const auto clearPayload = Spark::MakeScopeExit([&] { Spark::SecureClear(payload); });
                HandoffOperationResult result = HandoffOperationResult::Rejected;
                const bool received = ReceiveFrameUntil(pipe, header, payload, m_stop);
                const bool trusted = received && SameUserPeer(pipe);
                if (trusted && payload.size() <= GatewayMaximumBodySize &&
                    header.serviceId == static_cast<uint16_t>(Daemon::ServiceId::Orchestration))
                {
                    DecodedRequest request;
                    const int64_t now = NowMilliseconds();
                    if (DecodeRequest(payload, request) &&
                        request.phase == static_cast<AreaControlPhase>(header.messageType) &&
                        request.timestamp >= now - 60000 && request.timestamp <= now + 60000 &&
                        VerifyGatewayMac(m_key, request.signedBody, request.mac))
                    {
                        std::lock_guard lock(m_mutex);
                        std::erase_if(m_seenNonces, [&](const auto& item) { return item.second < now - 60000; });
                        if (!m_seenNonces.contains(request.nonce))
                        {
                            m_seenNonces.emplace(request.nonce, request.timestamp);
                            result = Apply(request.command.sessionId, request.command.epoch, request.phase);
                        }
                    }
                }
                if (trusted)
                {
                    const std::vector<uint8_t> response{static_cast<uint8_t>(result)};
                    responseSent = SendFrameUntil(pipe, Daemon::ServiceId::Orchestration,
                                                  header.messageType + ResponseFlag, response, m_stop);
                }
                // Complete the documented reply lifecycle before recycling
                // the single pipe instance. PeekNamedPipe does not reliably
                // observe peer closure under Wine and can hold the listener
                // unavailable until the full I/O timeout expires.
                if (responseSent)
                    (void)::FlushFileBuffers(pipe);
            }
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);
#else
        const std::string endpoint = NormalizeLocalEndpoint(m_endpoint);
        const int listener = CreateLocalListener(endpoint);
        if (listener < 0)
        {
            const int error = errno;
            const std::error_code systemError(error, std::system_category());
            SetError("Cannot create gateway area-control local listener '" + endpoint + "': " + systemError.message() +
                     " (errno " + std::to_string(error) + ")");
            m_ready.store(false, std::memory_order_release);
            m_startupComplete.store(true, std::memory_order_release);
            return;
        }
        m_ready.store(true, std::memory_order_release);
        m_startupComplete.store(true, std::memory_order_release);
        while (!m_stop.load(std::memory_order_acquire))
        {
            pollfd descriptor{listener, POLLIN, 0};
            const int polled = ::poll(&descriptor, 1, 100);
            if (polled <= 0)
            {
                if (polled < 0 && errno != EINTR)
                    break;
                continue;
            }
            const int connection = ::accept(listener, nullptr, nullptr);
            if (connection < 0)
                continue;
            ConfigureAcceptedSocket(connection);
            if (!SameUserPeer(connection))
            {
                ::close(connection);
                continue;
            }
            Daemon::FrameHeader header{};
            std::vector<uint8_t> payload;
            const auto clearPayload = Spark::MakeScopeExit([&] { Spark::SecureClear(payload); });
            HandoffOperationResult result = HandoffOperationResult::Rejected;
            const bool received = ReceiveFrameUntil(connection, header, payload, m_stop);
            if (received && payload.size() <= GatewayMaximumBodySize &&
                header.serviceId == static_cast<uint16_t>(Daemon::ServiceId::Orchestration))
            {
                DecodedRequest request;
                const int64_t now = NowMilliseconds();
                if (DecodeRequest(payload, request) &&
                    request.phase == static_cast<AreaControlPhase>(header.messageType) &&
                    request.timestamp >= now - 60000 && request.timestamp <= now + 60000 &&
                    VerifyGatewayMac(m_key, request.signedBody, request.mac))
                {
                    std::lock_guard lock(m_mutex);
                    std::erase_if(m_seenNonces, [&](const auto& item) { return item.second < now - 60000; });
                    if (!m_seenNonces.contains(request.nonce))
                    {
                        m_seenNonces.emplace(request.nonce, request.timestamp);
                        result = Apply(request.command.sessionId, request.command.epoch, request.phase);
                    }
                }
            }
            if (received)
            {
                const std::vector<uint8_t> response{static_cast<uint8_t>(result)};
                (void)SendFrameUntil(connection, Daemon::ServiceId::Orchestration, header.messageType + ResponseFlag,
                                     response, m_stop);
            }
            ::close(connection);
        }
        UnlinkOwnedEndpoint(endpoint, listener);
        ::close(listener);
#endif
        m_ready.store(false, std::memory_order_release);
    }

    HandoffOperationResult LocalAreaControlService::Apply(std::string_view sessionId, uint64_t epoch,
                                                          AreaControlPhase phase)
    {
        if (phase == AreaControlPhase::Probe)
            return HandoffOperationResult::Applied;
        auto found = m_sessions.find(std::string(sessionId));
        if (found == m_sessions.end())
        {
            if (phase != AreaControlPhase::Prepare || epoch == 0)
                return HandoffOperationResult::Rejected;
            const std::string session(sessionId);
            m_sessions.emplace(session, SessionFence{epoch, phase});
            if (!SaveState())
            {
                m_sessions.erase(session);
                return HandoffOperationResult::Unavailable;
            }
            return HandoffOperationResult::Applied;
        }
        SessionFence& fence = found->second;
        if (epoch < fence.epoch)
            return HandoffOperationResult::Rejected;
        if (epoch == fence.epoch && phase == fence.phase)
            return HandoffOperationResult::Duplicate;
        if (epoch > fence.epoch)
        {
            if ((fence.phase != AreaControlPhase::Acknowledge && fence.phase != AreaControlPhase::Abort) ||
                phase != AreaControlPhase::Prepare)
                return HandoffOperationResult::Rejected;
            const SessionFence previous = fence;
            fence = {epoch, phase};
            if (!SaveState())
            {
                fence = previous;
                return HandoffOperationResult::Unavailable;
            }
            return HandoffOperationResult::Applied;
        }
        const bool sequential = (fence.phase == AreaControlPhase::Prepare && phase == AreaControlPhase::Transfer) ||
                                (fence.phase == AreaControlPhase::Transfer && phase == AreaControlPhase::Commit) ||
                                (fence.phase == AreaControlPhase::Commit && phase == AreaControlPhase::Acknowledge);
        const bool abort = phase == AreaControlPhase::Abort && fence.phase != AreaControlPhase::Acknowledge;
        if (!sequential && !abort)
            return HandoffOperationResult::Rejected;
        const SessionFence previous = fence;
        fence.phase = phase;
        if (!SaveState())
        {
            fence = previous;
            return HandoffOperationResult::Unavailable;
        }
        return HandoffOperationResult::Applied;
    }

    bool LocalAreaControlService::LoadState()
    {
        std::error_code error;
        if (!std::filesystem::exists(m_epochStateFile, error))
            return !error;
        std::ifstream input(m_epochStateFile);
        if (!input)
            return false;
        std::unordered_map<std::string, SessionFence> loaded;
        std::string session;
        uint64_t epoch = 0;
        unsigned int phase = 0;
        while (true)
        {
            input >> std::ws;
            if (input.peek() == std::char_traits<char>::eof())
                break;
            if (!(input >> std::quoted(session) >> epoch >> phase) || session.empty() || session.size() > 128 ||
                epoch == 0 || phase < 1 || phase > 5 || loaded.contains(session))
                return false;
            loaded.emplace(session, SessionFence{epoch, static_cast<AreaControlPhase>(phase)});
        }
        if (input.bad())
            return false;
        m_sessions.swap(loaded);
        return true;
    }

    bool LocalAreaControlService::SaveState() const
    {
        std::ostringstream output;
        for (const auto& [session, fence] : m_sessions)
            output << std::quoted(session) << ' ' << fence.epoch << ' ' << static_cast<unsigned int>(fence.phase)
                   << '\n';
        return AtomicWriteText(m_epochStateFile, output.str());
    }

    namespace
    {
        constexpr uint16_t IngressRequest = 0x40;
        constexpr uint16_t IngressResponse = 0x140;

        class ReadOnlyAdmissionBuffer final : public std::streambuf
        {
          public:
            explicit ReadOnlyAdmissionBuffer(std::span<const uint8_t> payload)
            {
                auto* first = reinterpret_cast<char*>(const_cast<uint8_t*>(payload.data()));
                setg(first, first, first + payload.size());
            }
        };

        bool AppendAdmissionText(std::vector<uint8_t>& payload, std::string_view text)
        {
            if (payload.size() > GatewayMaximumBodySize || text.size() > GatewayMaximumBodySize - payload.size())
                return false;
            if (text.empty())
                return true;
            const auto* first = reinterpret_cast<const uint8_t*>(text.data());
            payload.insert(payload.end(), first, first + text.size());
            return true;
        }

        template <typename Value> bool AppendAdmissionValue(std::vector<uint8_t>& payload, Value value)
        {
            char encoded[64]{};
            const auto [end, error] = std::to_chars(encoded, encoded + sizeof(encoded), value);
            return error == std::errc{} && AppendAdmissionText(payload, {encoded, static_cast<size_t>(end - encoded)});
        }

        bool AppendQuotedAdmissionText(std::vector<uint8_t>& payload, std::string_view text)
        {
            if (!AppendAdmissionText(payload, "\""))
                return false;
            for (const char& character : text)
            {
                if ((character == '\\' || character == '"') && !AppendAdmissionText(payload, "\\"))
                    return false;
                if (!AppendAdmissionText(payload, {&character, 1}))
                    return false;
            }
            return AppendAdmissionText(payload, "\"");
        }

        std::vector<uint8_t> EncodeAdmission(const AdmissionRequest& request)
        {
            if (request.credential.size() > GatewayMaximumCredentialSize)
                return {};
            std::vector<uint8_t> payload;
            payload.reserve(GatewayMaximumBodySize);
            bool complete = false;
            const auto clearOnFailure = Spark::MakeScopeExit(
                [&]
                {
                    if (!complete)
                        Spark::SecureClear(payload);
                });
            if (!AppendAdmissionValue(payload, GatewayProtocolMajor) || !AppendAdmissionText(payload, " ") ||
                !AppendAdmissionValue(payload, GatewayProtocolMinor) || !AppendAdmissionText(payload, " ") ||
                !AppendAdmissionValue(payload, request.clientId) || !AppendAdmissionText(payload, " ") ||
                !AppendAdmissionValue(payload, request.spawnPosition.x) || !AppendAdmissionText(payload, " ") ||
                !AppendAdmissionValue(payload, request.spawnPosition.y) || !AppendAdmissionText(payload, " ") ||
                !AppendAdmissionValue(payload, request.spawnPosition.z) || !AppendAdmissionText(payload, " ") ||
                !AppendQuotedAdmissionText(payload, request.sessionId) || !AppendAdmissionText(payload, " ") ||
                !AppendQuotedAdmissionText(payload, request.playerName) || !AppendAdmissionText(payload, " ") ||
                !AppendQuotedAdmissionText(payload, request.credential))
                return {};
            complete = true;
            return payload;
        }

        bool DecodeAdmission(const std::vector<uint8_t>& payload, AdmissionRequest& request)
        {
            Spark::SecureClear(request.credential);
            bool complete = false;
            const auto clearCredentialOnFailure = Spark::MakeScopeExit(
                [&]
                {
                    if (!complete)
                        Spark::SecureClear(request.credential);
                });
            request.credential.reserve(GatewayMaximumCredentialSize);
            if (payload.empty() || payload.size() > GatewayMaximumBodySize)
                return false;
            ReadOnlyAdmissionBuffer buffer(payload);
            std::istream input(&buffer);
            uint16_t major = 0, minor = 0;
            if (!(input >> major >> minor >> request.clientId >> request.spawnPosition.x >> request.spawnPosition.y >>
                  request.spawnPosition.z >> std::quoted(request.sessionId) >> std::quoted(request.playerName) >>
                  std::quoted(request.credential)) ||
                major != GatewayProtocolMajor || minor > GatewayProtocolMinor ||
                request.credential.size() > GatewayMaximumCredentialSize)
                return false;
            input >> std::ws;
            complete = input.eof();
            return complete;
        }

        std::vector<uint8_t> EncodeRoute(const RouteResult& result)
        {
            std::ostringstream output;
            output << (result.accepted ? 1 : 0) << ' ' << static_cast<unsigned int>(result.failure) << ' '
                   << result.port << ' ' << std::quoted(result.host) << ' ' << std::quoted(result.reason) << ' '
                   << result.session.authoritativeArea;
            const std::string text = output.str();
            return {text.begin(), text.end()};
        }

        bool DecodeRoute(const std::vector<uint8_t>& payload, RouteResult& result)
        {
            if (payload.empty() || payload.size() > GatewayMaximumBodySize)
                return false;
            std::istringstream input(std::string(payload.begin(), payload.end()));
            unsigned int accepted = 0, failure = 0;
            if (!(input >> accepted >> failure >> result.port >> std::quoted(result.host) >>
                  std::quoted(result.reason) >> result.session.authoritativeArea) ||
                accepted > 1 || failure > static_cast<unsigned int>(RouteFailure::NoAreaAvailable))
                return false;
            result.accepted = accepted != 0;
            result.failure = static_cast<RouteFailure>(failure);
            return true;
        }
    } // namespace

    RouteResult LocalGatewayIngressClient::Admit(const AdmissionRequest& request) const
    {
        RouteResult result;
        auto payload = EncodeAdmission(request);
        const auto clearPayload = Spark::MakeScopeExit([&] { Spark::SecureClear(payload); });
        if (payload.empty())
        {
            result.failure = RouteFailure::InvalidRequest;
            result.reason = "Admission exceeds local ingress frame bounds";
            return result;
        }
        Daemon::DaemonClient client;
        if (auto connected = client.Connect(NormalizeLocalEndpoint(m_endpoint)); !connected)
        {
            result.failure = RouteFailure::NotReady;
            result.reason = "Local gateway ingress is unavailable";
            return result;
        }
        auto response = client.Request(Daemon::ServiceId::Orchestration, IngressRequest, payload);
        if (!response || response->messageType != IngressResponse || !DecodeRoute(response->payload, result))
        {
            result.failure = RouteFailure::NotReady;
            result.reason = "Malformed local gateway ingress response";
        }
        return result;
    }

    LocalGatewayIngressService::LocalGatewayIngressService(std::string endpoint, GatewayCoordinator& coordinator)
        : m_endpoint(std::move(endpoint)), m_coordinator(&coordinator)
    {
    }

    LocalGatewayIngressService::~LocalGatewayIngressService()
    {
        Stop();
    }

    bool LocalGatewayIngressService::Start()
    {
        if (m_thread.joinable() || m_endpoint.empty() || !m_coordinator)
            return false;
        m_stop.store(false, std::memory_order_release);
        m_thread = std::thread(&LocalGatewayIngressService::Run, this);
        for (int attempt = 0; attempt < 100 && !IsReady(); ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (IsReady())
            return true;
        Stop();
        return false;
    }

    void LocalGatewayIngressService::Stop()
    {
        m_stop.store(true, std::memory_order_release);
#ifdef _WIN32
        const std::wstring pipe = Daemon::NormalizePipeName(NormalizeLocalEndpoint(m_endpoint));
        HANDLE wake = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (wake != INVALID_HANDLE_VALUE)
            CloseHandle(wake);
#endif
        if (m_thread.joinable())
            m_thread.join();
        m_ready.store(false, std::memory_order_release);
    }

    void LocalGatewayIngressService::Run()
    {
#ifdef _WIN32
        const std::wstring pipeName = Daemon::NormalizePipeName(NormalizeLocalEndpoint(m_endpoint));
        HANDLE pipe = CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                       1, 5120, 5120, 1000, nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
        {
            m_ready.store(false, std::memory_order_release);
            return;
        }
        m_ready.store(true, std::memory_order_release);
        while (!m_stop.load(std::memory_order_acquire))
        {
            const bool connectedCall = ConnectNamedPipe(pipe, nullptr) != 0;
            const DWORD connectError = connectedCall ? ERROR_SUCCESS : GetLastError();
            const bool connected = connectedCall || connectError == ERROR_PIPE_CONNECTED;
            if (!connected)
            {
                if (connectError == ERROR_PIPE_LISTENING || connectError == ERROR_NO_DATA)
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (connected && !m_stop.load(std::memory_order_acquire))
            {
                bool responseSent = false;
                Daemon::FrameHeader header{};
                std::vector<uint8_t> payload;
                const auto clearPayload = Spark::MakeScopeExit([&] { Spark::SecureClear(payload); });
                RouteResult route;
                const bool received = ReceiveFrameUntil(pipe, header, payload, m_stop);
                const bool trusted = received && SameUserPeer(pipe);
                if (trusted && header.serviceId == static_cast<uint16_t>(Daemon::ServiceId::Orchestration) &&
                    header.messageType == IngressRequest)
                {
                    AdmissionRequest request;
                    const auto clearCredential = Spark::MakeScopeExit([&] { Spark::SecureClear(request.credential); });
                    if (DecodeAdmission(payload, request))
                        route = m_coordinator->Admit(request);
                    else
                    {
                        route.failure = RouteFailure::InvalidRequest;
                        route.reason = "Malformed local ingress request";
                    }
                }
                else
                {
                    route.failure = RouteFailure::InvalidRequest;
                    route.reason = "Invalid local ingress frame";
                }
                if (trusted)
                    responseSent = SendFrameUntil(pipe, Daemon::ServiceId::Orchestration, IngressResponse,
                                                  EncodeRoute(route), m_stop);
                if (responseSent)
                    (void)::FlushFileBuffers(pipe);
            }
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);
#else
        const std::string endpoint = NormalizeLocalEndpoint(m_endpoint);
        const int listener = CreateLocalListener(endpoint);
        if (listener < 0)
        {
            m_ready.store(false, std::memory_order_release);
            return;
        }
        m_ready.store(true, std::memory_order_release);
        while (!m_stop.load(std::memory_order_acquire))
        {
            pollfd descriptor{listener, POLLIN, 0};
            const int polled = ::poll(&descriptor, 1, 100);
            if (polled <= 0)
            {
                if (polled < 0 && errno != EINTR)
                    break;
                continue;
            }
            const int connection = ::accept(listener, nullptr, nullptr);
            if (connection < 0)
                continue;
            ConfigureAcceptedSocket(connection);
            if (!SameUserPeer(connection))
            {
                ::close(connection);
                continue;
            }
            Daemon::FrameHeader header{};
            std::vector<uint8_t> payload;
            const auto clearPayload = Spark::MakeScopeExit([&] { Spark::SecureClear(payload); });
            RouteResult route;
            const bool received = ReceiveFrameUntil(connection, header, payload, m_stop);
            if (received && header.serviceId == static_cast<uint16_t>(Daemon::ServiceId::Orchestration) &&
                header.messageType == IngressRequest)
            {
                AdmissionRequest request;
                const auto clearCredential = Spark::MakeScopeExit([&] { Spark::SecureClear(request.credential); });
                if (DecodeAdmission(payload, request))
                    route = m_coordinator->Admit(request);
                else
                {
                    route.failure = RouteFailure::InvalidRequest;
                    route.reason = "Malformed local ingress request";
                }
            }
            else
            {
                route.failure = RouteFailure::InvalidRequest;
                route.reason = "Invalid local ingress frame";
            }
            if (received)
                (void)SendFrameUntil(connection, Daemon::ServiceId::Orchestration, IngressResponse, EncodeRoute(route),
                                     m_stop);
            ::close(connection);
        }
        UnlinkOwnedEndpoint(endpoint, listener);
        ::close(listener);
#endif
        m_ready.store(false, std::memory_order_release);
    }
} // namespace Spark::Gateway

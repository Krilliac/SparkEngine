/**
 * @file TestSecDaemonPipeIdentity.cpp
 * @brief Security regression tests for SparkDaemon endpoint identity (SEC4 daemon client).
 *
 * Named pipes are machine-global on Windows, so another local account could
 * create the daemon's pipe name first and serve the engine attacker-chosen
 * shader bytecode, or impersonate the engine's token. These tests pin:
 *   - path-style endpoints map to per-path, per-user pipe names instead of one
 *     shared basename (Windows);
 *   - DaemonClient connects at SecurityIdentification, so a server cannot act
 *     as the client (Windows);
 *   - the client's server-identity check accepts the current user and fails
 *     closed otherwise (Windows);
 *   - DaemonServer refuses an endpoint another process already holds instead
 *     of silently sharing it (all platforms);
 *   - a path-style endpoint still round-trips a real Control ping (all platforms).
 *
 * Thread affinity: every test runs on the test thread plus one server thread it joins.
 */

#include "TestFramework.h"

#include "Utils/DaemonClient.h"
#include "Utils/DaemonFraming.h"
#include "Utils/DaemonProtocol.h"

// SparkDaemon sources are pulled into SparkTests directly.
#include "ControlService.h"
#include "DaemonServer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace
{
    std::string UniqueEndpoint(const char* tag)
    {
#if defined(_WIN32)
        return std::string("spark-sec-pipe-") + tag + "-" + std::to_string(::GetCurrentProcessId());
#else
        return std::string("/tmp/spark-sec-pipe-") + tag + "-" + std::to_string(static_cast<long long>(::getpid())) +
               ".sock";
#endif
    }

    /// A filesystem-style endpoint (the shape of the engine's default `./.spark-daemon.sock`).
    std::string UniquePathEndpoint(const char* tag)
    {
#if defined(_WIN32)
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            ("spark-sec-pipe-" + std::string(tag) + "-" + std::to_string(::GetCurrentProcessId()) + ".sock");
        const std::u8string utf8 = path.u8string();
        return std::string(utf8.begin(), utf8.end());
#else
        return UniqueEndpoint(tag);
#endif
    }

    bool WaitForEndpoint(const std::string& endpoint, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
#if defined(_WIN32)
            const std::wstring pipeName = Spark::Daemon::NormalizePipeName(endpoint);
            if (!pipeName.empty() && ::WaitNamedPipeW(pipeName.c_str(), 20))
                return true;
#else
            struct stat status
            {
            };
            if (::stat(endpoint.c_str(), &status) == 0)
                return true;
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    /// Runs DaemonServer::Run on a background thread and always joins it.
    class ServerRun
    {
      public:
        explicit ServerRun(const std::string& endpoint)
        {
            m_server = std::make_unique<Spark::Daemon::DaemonServer>();
            m_server->AddService(std::make_unique<Spark::Daemon::ControlService>(m_server->GetShouldStopFlag()));
            m_thread = std::thread(
                [this, endpoint]
                {
                    auto result = m_server->Run(endpoint);
                    if (!result)
                        m_error = result.error();
                    m_succeeded.store(result.has_value(), std::memory_order_release);
                    m_exited.store(true, std::memory_order_release);
                });
        }

        ~ServerRun() { StopAndJoin(); }

        ServerRun(const ServerRun&) = delete;
        ServerRun& operator=(const ServerRun&) = delete;

        /// True when Run() returned on its own within @p timeout.
        bool ExitsWithin(std::chrono::milliseconds timeout) const
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (m_exited.load(std::memory_order_acquire))
                    return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return m_exited.load(std::memory_order_acquire);
        }

        void StopAndJoin()
        {
            m_server->Stop();
            if (m_thread.joinable())
                m_thread.join();
        }

        /// Valid only after StopAndJoin().
        [[nodiscard]] bool Succeeded() const { return m_succeeded.load(std::memory_order_acquire); }
        [[nodiscard]] const std::string& Error() const { return m_error; }

      private:
        std::unique_ptr<Spark::Daemon::DaemonServer> m_server;
        std::thread m_thread;
        std::atomic<bool> m_exited{false};
        std::atomic<bool> m_succeeded{false};
        std::string m_error;
    };
} // namespace

// ---------------------------------------------------------------------------
// All platforms
// ---------------------------------------------------------------------------

TEST(SecDaemonPipe_ServerRefusesEndpointHeldByAnotherProcess)
{
    const std::string endpoint = UniqueEndpoint("squat");

#if defined(_WIN32)
    // A squatter (here: this test) creates the name first with the default,
    // same-user-writable DACL. Before the fix the daemon added its own
    // instances to the squatter's pipe and ran; now it must refuse.
    const std::wstring pipeName = Spark::Daemon::NormalizePipeName(endpoint);
    ASSERT_FALSE(pipeName.empty());
    HANDLE squatter =
        ::CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                           PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);
    ASSERT_TRUE(squatter != INVALID_HANDLE_VALUE);
#else
    int squatter = ::socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_TRUE(squatter >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    ASSERT_TRUE(endpoint.size() < sizeof(address.sun_path));
    std::memcpy(address.sun_path, endpoint.data(), endpoint.size());
    ::unlink(endpoint.c_str());
    ASSERT_TRUE(::bind(squatter, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    ASSERT_TRUE(::listen(squatter, 4) == 0);
#endif

    ServerRun run(endpoint);
    const bool exitedOnItsOwn = run.ExitsWithin(std::chrono::milliseconds(3000));
    run.StopAndJoin();

#if defined(_WIN32)
    ::CloseHandle(squatter);
#else
    ::close(squatter);
    ::unlink(endpoint.c_str());
#endif

    EXPECT_TRUE(exitedOnItsOwn);
    EXPECT_FALSE(run.Succeeded());
    EXPECT_FALSE(run.Error().empty());
}

TEST(SecDaemonPipe_PathEndpointRoundTripsPing)
{
    const std::string endpoint = UniquePathEndpoint("roundtrip");
    ServerRun run(endpoint);
    EXPECT_TRUE(WaitForEndpoint(endpoint, std::chrono::milliseconds(2000)));

    Spark::Daemon::DaemonClient client;
    auto connected = client.Connect(endpoint);
    EXPECT_TRUE(connected.has_value());
    if (connected)
    {
        auto ping = client.Ping();
        EXPECT_TRUE(ping.has_value());
        if (!ping)
            std::cerr << "  ping error: " << ping.error() << '\n';
    }
    else
    {
        std::cerr << "  connect error: " << connected.error() << '\n';
    }
    client.Disconnect();
    run.StopAndJoin();
    EXPECT_TRUE(run.Succeeded());
#if !defined(_WIN32)
    ::unlink(endpoint.c_str());
#endif
}

// ---------------------------------------------------------------------------
// Windows only: named-pipe naming, SQOS and server identity
// ---------------------------------------------------------------------------

#if defined(_WIN32)

TEST(SecDaemonPipe_PathEndpointsGetPerPathPipeNames)
{
    using Spark::Daemon::NormalizePipeName;

    // Two build trees with the default socket file name used to collapse onto
    // one machine-global pipe, `\\.\pipe\-spark-daemon-sock`.
    const std::wstring treeA = NormalizePipeName("C:/SparkTreeA/build/.spark-daemon.sock");
    const std::wstring treeB = NormalizePipeName("C:/SparkTreeB/build/.spark-daemon.sock");
    EXPECT_FALSE(treeA.empty());
    EXPECT_FALSE(treeB.empty());
    EXPECT_TRUE(treeA != treeB);
    EXPECT_TRUE(treeA.starts_with(L"\\\\.\\pipe\\-spark-daemon-sock-"));

    // Separator style and ASCII case do not change the resolved path.
    EXPECT_TRUE(treeA == NormalizePipeName("c:\\sparktreea\\build\\.spark-daemon.sock"));

    // A relative endpoint resolves against the working directory.
    const std::u8string absolute = (std::filesystem::current_path() / ".spark-daemon.sock").u8string();
    EXPECT_TRUE(NormalizePipeName("./.spark-daemon.sock") ==
                NormalizePipeName(std::string(absolute.begin(), absolute.end())));

    // Bare names and explicit pipe names keep their literal mapping.
    EXPECT_TRUE(NormalizePipeName("spark-gateway-test.1") == L"\\\\.\\pipe\\spark-gateway-test-1");
    EXPECT_TRUE(NormalizePipeName("\\\\.\\pipe\\custom-name") == L"\\\\.\\pipe\\custom-name");

    // Malformed or oversized input fails closed.
    EXPECT_TRUE(NormalizePipeName(std::string("bad\xff", 4)).empty());
    EXPECT_TRUE(NormalizePipeName(std::string(40000, 'a')).empty());
}

TEST(SecDaemonPipe_ClientConnectsAtIdentificationLevel)
{
    const std::string endpoint = UniqueEndpoint("sqos");
    const std::wstring pipeName = Spark::Daemon::NormalizePipeName(endpoint);
    ASSERT_FALSE(pipeName.empty());

    HANDLE server = ::CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                       4096, 4096, 0, nullptr);
    ASSERT_TRUE(server != INVALID_HANDLE_VALUE);

    Spark::Daemon::DaemonClient client;
    auto connected = client.Connect(endpoint);
    EXPECT_TRUE(connected.has_value());
    if (!connected)
    {
        std::cerr << "  connect error: " << connected.error() << '\n';
        ::CloseHandle(server);
        return;
    }
    if (!::ConnectNamedPipe(server, nullptr))
        EXPECT_EQ(static_cast<unsigned long>(::GetLastError()), static_cast<unsigned long>(ERROR_PIPE_CONNECTED));

    // A pipe server can impersonate only after reading from the pipe.
    auto sent = client.SendOneWay(Spark::Daemon::ServiceId::Control,
                                  static_cast<uint16_t>(Spark::Daemon::ControlMessage::PingRequest), {});
    EXPECT_TRUE(sent.has_value());
    uint8_t header[Spark::Daemon::kFrameHeaderSize] = {};
    DWORD bytesRead = 0;
    EXPECT_TRUE(::ReadFile(server, header, sizeof(header), &bytesRead, nullptr) != FALSE);

    SECURITY_IMPERSONATION_LEVEL level = SecurityImpersonation;
    bool queried = false;
    if (::ImpersonateNamedPipeClient(server))
    {
        HANDLE token = nullptr;
        if (::OpenThreadToken(::GetCurrentThread(), TOKEN_QUERY, TRUE, &token))
        {
            DWORD returned = 0;
            queried = ::GetTokenInformation(token, TokenImpersonationLevel, &level, sizeof(level), &returned) != FALSE;
            ::CloseHandle(token);
        }
        ::RevertToSelf();
    }

    client.Disconnect();
    ::CloseHandle(server);

    // Without SECURITY_SQOS_PRESENT the level is SecurityImpersonation, which
    // lets whoever owns the pipe act with the engine user's token.
    EXPECT_TRUE(queried);
    EXPECT_TRUE(level == SecurityIdentification);
}

TEST(SecDaemonPipe_ServerIdentityCheckFailsClosed)
{
    using Spark::Daemon::IsPipeServerCurrentUser;
    using Spark::Daemon::ProcessRunsAsCurrentUser;

    EXPECT_TRUE(ProcessRunsAsCurrentUser(::GetCurrentProcessId()));
    EXPECT_FALSE(ProcessRunsAsCurrentUser(0));

    // PID 4 is the kernel System process. Unless this test itself runs as
    // LocalSystem, it is another user and must be rejected.
    Spark::Daemon::TokenUserStorage self;
    ASSERT_TRUE(Spark::Daemon::QueryProcessUser(::GetCurrentProcess(), self));
    if (::IsWellKnownSid(self.Sid(), WinLocalSystemSid) == FALSE)
        EXPECT_FALSE(ProcessRunsAsCurrentUser(4));

    // A handle that is not a pipe cannot name a server process.
    HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_TRUE(event != nullptr);
    EXPECT_FALSE(IsPipeServerCurrentUser(event));
    ::CloseHandle(event);

    // A pipe this process serves is accepted.
    const std::wstring pipeName = Spark::Daemon::NormalizePipeName(UniqueEndpoint("identity"));
    ASSERT_FALSE(pipeName.empty());
    HANDLE server = ::CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
    ASSERT_TRUE(server != INVALID_HANDLE_VALUE);
    HANDLE clientEnd = ::CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    EXPECT_TRUE(clientEnd != INVALID_HANDLE_VALUE);
    if (clientEnd != INVALID_HANDLE_VALUE)
    {
        EXPECT_TRUE(IsPipeServerCurrentUser(clientEnd));
        ::CloseHandle(clientEnd);
    }
    ::CloseHandle(server);
}

#endif // _WIN32

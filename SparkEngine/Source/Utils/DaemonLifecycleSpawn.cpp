/**
 * @file DaemonLifecycleSpawn.cpp
 * @brief Platform-aware SparkDaemon endpoint readiness and auto-spawn
 */

#include "DaemonLifecycleSpawn.h"

#include "DaemonConnection.h"
#include "DaemonFraming.h"
#include "DaemonProtocol.h"
#include "LogMacros.h"
#include "Process.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace Spark::Daemon::Detail
{

    namespace
    {

        Spark::Daemon::Expected<void, std::string> WaitForDaemonEndpoint(const std::string& endpoint,
                                                                         std::chrono::milliseconds timeout)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;

#if defined(_WIN32)
            const std::wstring pipeName = Spark::Daemon::NormalizePipeName(endpoint);
            if (pipeName.empty())
                return Spark::Daemon::Unexpected<std::string>("named-pipe endpoint is not valid UTF-8: " + endpoint);

            DWORD lastError = ERROR_FILE_NOT_FOUND;
            do
            {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline)
                    break;

                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
                const DWORD waitMs =
                    static_cast<DWORD>((std::max)(int64_t{1}, (std::min)(int64_t{50}, remaining.count())));
                if (::WaitNamedPipeW(pipeName.c_str(), waitMs))
                    return {};

                lastError = ::GetLastError();
                if (lastError != ERROR_FILE_NOT_FOUND && lastError != ERROR_PIPE_BUSY && lastError != ERROR_SEM_TIMEOUT)
                {
                    return Spark::Daemon::Unexpected<std::string>("named-pipe readiness probe failed for " + endpoint +
                                                                  " (Win32 error " + std::to_string(lastError) + ")");
                }

                // A pipe that has not been created yet returns immediately instead
                // of honouring waitMs, so avoid a startup-speed busy loop.
                if (lastError == ERROR_FILE_NOT_FOUND)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } while (std::chrono::steady_clock::now() < deadline);

            return Spark::Daemon::Unexpected<std::string>("named pipe did not become ready within " +
                                                          std::to_string(timeout.count()) + " ms at " + endpoint +
                                                          " (last Win32 error " + std::to_string(lastError) + ")");
#else
            sockaddr_un address{};
            if (endpoint.size() >= sizeof(address.sun_path))
                return Spark::Daemon::Unexpected<std::string>("Unix-socket endpoint exceeds sockaddr_un capacity: " +
                                                              endpoint);
            address.sun_family = AF_UNIX;
            std::memcpy(address.sun_path, endpoint.data(), endpoint.size());

            int lastError = ENOENT;
            do
            {
                struct stat endpointStatus = {};
                if (::stat(endpoint.c_str(), &endpointStatus) == 0)
                {
                    if (!S_ISSOCK(endpointStatus.st_mode))
                    {
                        return Spark::Daemon::Unexpected<std::string>(
                            "daemon endpoint exists but is not a Unix-domain socket: " + endpoint);
                    }

                    // The socket file appears at bind(), before the daemon calls listen(), so its
                    // existence is not readiness: a connect() in that window is refused. Probe with
                    // a real connect and keep waiting while it is refused. A non-blocking AF_UNIX
                    // connect either completes or reports EAGAIN when the backlog is full, and both
                    // mean the daemon is listening.
                    const int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
                    if (probe < 0)
                    {
                        return Spark::Daemon::Unexpected<std::string>(
                            std::string("Unix-socket readiness probe could not create a socket: ") +
                            std::strerror(errno));
                    }
                    const int probeFlags = ::fcntl(probe, F_GETFL, 0);
                    if (probeFlags >= 0)
                    {
                        (void)::fcntl(probe, F_SETFL, probeFlags | O_NONBLOCK);
                    }
                    const int connected =
                        ::connect(probe, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
                    const int connectError = connected == 0 ? 0 : errno;
                    ::close(probe);
                    if (connected == 0 || connectError == EAGAIN || connectError == EINPROGRESS)
                    {
                        return {};
                    }
                    lastError = connectError;
                    if (lastError != ECONNREFUSED && lastError != ENOENT)
                    {
                        return Spark::Daemon::Unexpected<std::string>("Unix-socket readiness probe failed for " +
                                                                      endpoint + ": " + std::strerror(lastError));
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }

                lastError = errno;
                if (lastError != ENOENT)
                {
                    return Spark::Daemon::Unexpected<std::string>("Unix-socket readiness probe failed for " + endpoint +
                                                                  ": " + std::strerror(lastError));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } while (std::chrono::steady_clock::now() < deadline);

            return Spark::Daemon::Unexpected<std::string>("Unix socket did not become ready within " +
                                                          std::to_string(timeout.count()) + " ms at " + endpoint +
                                                          ": " + std::strerror(lastError));
#endif
        }

    } // namespace

    bool TrySpawnDaemon(const std::string& socketPath, const std::string& binaryOverride)
    {
        std::string binary = binaryOverride;
        if (binary.empty())
        {
#if defined(_WIN32)
            binary = "./SparkDaemon.exe";
#else
            binary = "./SparkDaemon";
#endif
        }

        // Only attempt spawn if the binary is present on disk — failing fast
        // here keeps log noise down on machines where the daemon isn't
        // installed at all.
        std::error_code ec;
        if (!std::filesystem::exists(binary, ec))
        {
            SPARK_LOG_INFO(Spark::LogCategory::Core, "Daemon auto-spawn: binary not found at %s", binary.c_str());
            return false;
        }

        auto builder = Spark::Process::Builder(binary).Detached();
        if (!socketPath.empty())
        {
            builder.Arg("--socket").Arg(socketPath);
        }
        auto result = builder.Launch();
        if (!result)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Daemon auto-spawn: launch failed: %s", result.error().c_str());
            return false;
        }
        SPARK_LOG_INFO(Spark::LogCategory::Core, "Daemon auto-spawn: launched %s", binary.c_str());

        // Poll briefly for the platform IPC endpoint before retrying connect().
        // Windows publishes a named pipe (not a filesystem entry); POSIX
        // publishes an AF_UNIX socket. The probe owns its deadline on both
        // platforms so a crashed child cannot stall engine startup.
        const std::string probePath =
            socketPath.empty() ? std::string("./") + Spark::Daemon::kDefaultSocketName : socketPath;
        auto ready = WaitForDaemonEndpoint(probePath, std::chrono::seconds(2));
        if (!ready)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Daemon auto-spawn: %s", ready.error().c_str());
            return false;
        }
        return true;
    }

} // namespace Spark::Daemon::Detail

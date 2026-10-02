/**
 * @file TF120PeerProcess.h
 * @brief Peer test processes for the multi-process persistence tests (POSIX and Windows).
 *
 * A peer is a fresh SparkTests process that runs exactly one test, which sees
 * a role variable (SPARK_TF120_PEER_ROLE, SPARK_DATA120_DRILL_ROLE, ...) in its
 * environment and plays a second continent authority or a dying writer instead
 * of the coordinator. Peers are exec'd rather than bare fork()s of the runner:
 * the suite's other threads (async logger, job workers) may hold locks at fork
 * time, and a forked child that logs or allocates would then deadlock.
 *
 * POSIX launches the peer through env(1). Windows has no env(1) and
 * Spark::Process::Builder has no environment API, so the peer inherits a
 * scoped copy of this process's environment; the callers' CTests are
 * RUN_SERIAL and spawn from the test thread, so nothing else reads it
 * meanwhile. Process::Kill() is SIGKILL on POSIX and TerminateProcess on
 * Windows; either way the OS releases the peer's ExclusiveFileLock handle.
 * Shared by TestTF120SharedSaveRoot.cpp, TestTF120Residency.cpp and
 * TestDATA120BackupRestore.cpp.
 */
#pragma once

#include "Utils/Process.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <memory>
#include <utility>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace TF120Peer
{
    inline constexpr const char* kRoleEnv = "SPARK_TF120_PEER_ROLE";
    inline constexpr const char* kDbEnv = "SPARK_TF120_PEER_DB";
    inline constexpr const char* kCharEnv = "SPARK_TF120_PEER_CHAR";
    inline constexpr const char* kTagEnv = "SPARK_TF120_PEER_TAG";
    inline constexpr const char* kGateEnv = "SPARK_TF120_PEER_GATE";
    /// The parent's test selection, dropped so the peer runs exactly one test.
    inline constexpr const char* kParentSelection[] = {"SPARK_TEST_FILE", "SPARK_TEST_EXPECT_COUNT",
                                                       "SPARK_TEST_EXCLUDE", "SPARK_TEST_LIMIT"};

    inline std::string EnvOrEmpty(const char* name)
    {
        const char* value = std::getenv(name);
        return value ? value : "";
    }

    inline std::filesystem::path TestBinaryPath()
    {
#ifdef _WIN32
        std::wstring buffer(512, L'\0');
        for (;;)
        {
            const DWORD size = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (size == 0)
                return {};
            if (size < buffer.size() - 1)
            {
                buffer.resize(size);
                return std::filesystem::path(buffer);
            }
            if (buffer.size() >= 32768)
                return {};
            buffer.resize(buffer.size() * 2);
        }
#elif defined(__APPLE__)
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::string buffer(size, '\0');
        if (_NSGetExecutablePath(buffer.data(), &size) != 0)
            return {};
        return std::filesystem::path(buffer.c_str());
#else
        std::error_code error;
        const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", error);
        return error ? std::filesystem::path{} : self;
#endif
    }

#ifdef _WIN32
    /// Sets (or, for an empty value, removes) one variable of this process's environment and restores it.
    /// A child launched inside the scope inherits it.
    class ScopedEnvironmentVariable
    {
      public:
        ScopedEnvironmentVariable(std::string name, const std::string& value) : m_name(std::move(name))
        {
            if (const char* previous = std::getenv(m_name.c_str()))
            {
                m_wasSet = true;
                m_previous = previous;
            }
            _putenv_s(m_name.c_str(), value.c_str());
        }
        ~ScopedEnvironmentVariable() { _putenv_s(m_name.c_str(), m_wasSet ? m_previous.c_str() : ""); }
        ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
        ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

      private:
        std::string m_name;
        bool m_wasSet = false;
        std::string m_previous;
    };

    /// UTF-8 form of a path, as Spark::Process::Builder expects on Windows.
    inline std::string Utf8(const std::filesystem::path& path)
    {
        const std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    }
#endif

    /// Launch a fresh SparkTests process that runs only `testName`, with each "NAME=value" of `settings`
    /// in its environment.
    inline std::expected<Spark::Process, std::string> SpawnPeer(const char* testName,
                                                                const std::vector<std::string>& settings)
    {
        const std::filesystem::path self = TestBinaryPath();
        if (self.empty())
            return std::unexpected(std::string("cannot resolve the test binary path"));

#ifdef _WIN32
        std::vector<std::unique_ptr<ScopedEnvironmentVariable>> scoped;
        for (const char* selection : kParentSelection)
            scoped.push_back(std::make_unique<ScopedEnvironmentVariable>(selection, std::string()));
        scoped.push_back(std::make_unique<ScopedEnvironmentVariable>("SPARK_TEST_NAME", testName));
        for (const std::string& setting : settings)
        {
            const size_t equals = setting.find('=');
            if (equals == std::string::npos || equals == 0)
                return std::unexpected("malformed peer setting '" + setting + "'");
            scoped.push_back(
                std::make_unique<ScopedEnvironmentVariable>(setting.substr(0, equals), setting.substr(equals + 1)));
        }
        Spark::Process::Builder builder(Utf8(self));
        builder.WorkingDirectory(Utf8(std::filesystem::current_path()));
#else
        Spark::Process::Builder builder("env");
        // Drop the parent's test selection so the peer runs exactly one test.
        for (const char* selection : kParentSelection)
            builder.Arg("-u").Arg(selection);
        builder.Arg(std::string("SPARK_TEST_NAME=") + testName);
        for (const std::string& setting : settings)
            builder.Arg(setting);
        builder.Arg(self.string()).WorkingDirectory(std::filesystem::current_path().string());
#endif
        builder.CaptureStdout().MergeStderrIntoStdout();
        return builder.Launch();
    }

    inline void DrainPeer(Spark::Process& peer, std::string& log)
    {
        std::string line;
        while (peer.TryReadLine(line))
        {
            log += line;
            log += '\n';
        }
    }

    /// Wait for every peer while draining its output (so a full pipe never
    /// stalls it), killing any still running at the deadline. Returns each
    /// peer's exit code, -1 when it was killed or died on a signal.
    inline std::vector<int> WaitPeers(std::vector<Spark::Process>& peers, std::vector<std::string>& logs,
                                      std::chrono::seconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::vector<int> codes(peers.size(), -1);
        std::vector<bool> finished(peers.size(), false);
        size_t remaining = peers.size();
        while (remaining > 0)
        {
            for (size_t i = 0; i < peers.size(); ++i)
            {
                if (finished[i])
                    continue;
                DrainPeer(peers[i], logs[i]);
                if (const std::optional<int> code = peers[i].GetExitCode())
                {
                    logs[i] += peers[i].ReadAllStdout();
                    codes[i] = *code;
                    finished[i] = true;
                    --remaining;
                }
            }
            if (remaining == 0)
                break;
            if (std::chrono::steady_clock::now() >= deadline)
            {
                for (size_t i = 0; i < peers.size(); ++i)
                    if (!finished[i])
                        peers[i].Kill();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return codes;
    }

    /// Value after `key` on the peer's report line, or -1 when absent.
    inline long long PeerReport(const std::string& log, const std::string& key)
    {
        const size_t at = log.find(key);
        if (at == std::string::npos)
            return -1;
        return std::atoll(log.c_str() + at + key.size());
    }

    /// Drain `peer` until its output reports `key`=1, it exits, or `timeout` passes. True when it is
    /// running and reported ready.
    inline bool WaitPeerReady(Spark::Process& peer, std::string& log, const std::string& key,
                              std::chrono::seconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (PeerReport(log, key) < 0 && peer.IsRunning() && std::chrono::steady_clock::now() < deadline)
        {
            DrainPeer(peer, log);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return PeerReport(log, key) == 1 && peer.IsRunning();
    }
} // namespace TF120Peer

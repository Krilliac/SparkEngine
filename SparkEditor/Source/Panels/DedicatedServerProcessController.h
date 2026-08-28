/**
 * @file DedicatedServerProcessController.h
 * @brief Testable lifecycle adapter between the editor and SparkServer.
 */

#pragma once

#include "Utils/Process.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace SparkEditor
{
    struct DedicatedServerLaunchRequest
    {
        std::filesystem::path executable;
        std::filesystem::path workingDirectory;
        std::filesystem::path module;
        std::filesystem::path manifest;
        std::filesystem::path healthFile;
        std::filesystem::path stopFile;
        std::string serverName;
        std::string map;
        uint16_t port = 27015;
        uint32_t maxClients = 8;
        float tickRate = 60.0f;
        std::string bindAddress = "loopback";
        bool lanBroadcast = false;
    };

    enum class DedicatedServerProcessState
    {
        Idle,
        Running,
        Stopping,
        Exited,
        Failed
    };

    struct DedicatedServerProcessSnapshot
    {
        DedicatedServerProcessState state = DedicatedServerProcessState::Idle;
        std::optional<int> exitCode;
        std::string healthJson;
        std::string error;
    };

    /** Owns the child process and implements sentinel-based graceful shutdown. */
    class DedicatedServerProcessController
    {
      public:
        DedicatedServerProcessController() = default;
        ~DedicatedServerProcessController();

        DedicatedServerProcessController(const DedicatedServerProcessController&) = delete;
        DedicatedServerProcessController& operator=(const DedicatedServerProcessController&) = delete;

        [[nodiscard]] static std::vector<std::string> CreateArguments(const DedicatedServerLaunchRequest& request,
                                                                      std::string& error);
        [[nodiscard]] static bool PackageExecutable(const std::filesystem::path& sourceExecutable,
                                                    const std::filesystem::path& outputDirectory,
                                                    std::string_view outputName, std::string& error);
        [[nodiscard]] bool Launch(const DedicatedServerLaunchRequest& request);
        void RequestStop();
        void Update();
        [[nodiscard]] DedicatedServerProcessSnapshot GetSnapshot() const;
        std::vector<std::string> DrainLogLines();

      private:
        void FinishExitedProcess(int exitCode);
        void AppendMultiline(std::string text, std::string_view prefix);

        std::optional<Spark::Process> m_process;
        DedicatedServerLaunchRequest m_request;
        DedicatedServerProcessSnapshot m_snapshot;
        std::vector<std::string> m_logLines;
        std::chrono::steady_clock::time_point m_stopRequestedAt{};
    };
} // namespace SparkEditor

/**
 * @file CollabProcessSmoke.cpp
 * @brief Black-box SparkCollabServer startup, client operation, and shutdown smoke test.
 */

#include "Communication/StandaloneCollaborationClient.h"
#include "Utils/DaemonClient.h"
#include "Utils/DaemonFraming.h"
#include "Utils/DaemonProtocol.h"
#include "Utils/Process.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace
{
    int g_failures = 0;

    void Check(bool condition, const char* description)
    {
        if (!condition)
        {
            std::cerr << "FAILED: " << description << '\n';
            ++g_failures;
        }
    }

    std::string UniqueEndpoint()
    {
#if defined(_WIN32)
        return "spark-collab-process-smoke-" + std::to_string(::GetCurrentProcessId());
#else
        return "/tmp/spark-collab-process-smoke-" + std::to_string(static_cast<long long>(::getpid())) + ".sock";
#endif
    }

    bool WaitUntilReady(const std::string& endpoint, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
#if defined(_WIN32)
            const std::wstring pipeName = Spark::Daemon::NormalizePipeName(endpoint);
            if (!pipeName.empty() && ::WaitNamedPipeW(pipeName.c_str(), 20))
                return true;
#else
            if (std::filesystem::exists(endpoint))
                return true;
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: SparkCollabProcessSmoke <SparkCollabServer>\n";
        return 2;
    }

    const std::filesystem::path serverPath = std::filesystem::absolute(argv[1]);
    const std::string endpoint = UniqueEndpoint();
    auto launched = Spark::Process::Builder(serverPath.string())
                        .Arg("--socket")
                        .Arg(endpoint)
                        .WorkingDirectory(serverPath.parent_path().string())
                        .CaptureStdout()
                        .MergeStderrIntoStdout()
                        .NoWindow()
                        .Launch();
    if (!launched)
    {
        std::cerr << "FAILED: could not launch SparkCollabServer: " << launched.error() << '\n';
        return 1;
    }
    auto& server = *launched;

    if (!WaitUntilReady(endpoint, std::chrono::seconds(5)))
    {
        std::cerr << "FAILED: SparkCollabServer did not become ready\n";
        server.Kill();
        (void)server.WaitForExit(std::chrono::seconds(2));
        std::cerr << server.ReadAllStdout();
        return 1;
    }

    SparkEditor::StandaloneCollaborationClient alice;
    SparkEditor::StandaloneCollaborationClient bob;
    auto aliceConnected = alice.Connect(endpoint);
    Check(aliceConnected.has_value(), "first production client connects and pings broker");
    if (!aliceConnected)
        std::cerr << aliceConnected.error() << '\n';

    std::string administrationToken;
    if (aliceConnected)
    {
        auto created = alice.CreateSession("process-smoke");
        Check(created.has_value(), "client creates broker session");
        if (created)
            administrationToken = *created;
        else
            std::cerr << created.error() << '\n';

        auto joined = alice.JoinSession("process-smoke", "Alice");
        Check(joined.has_value() && *joined != 0, "first editor joins broker session");
        if (!joined)
            std::cerr << joined.error() << '\n';
    }

    auto bobConnected = bob.Connect(endpoint);
    Check(bobConnected.has_value(), "second production client connects to running process");
    if (bobConnected)
    {
        auto joined = bob.JoinSession("process-smoke", "Bob");
        Check(joined.has_value() && *joined != 0, "second editor joins broker session");
        if (!joined)
            std::cerr << joined.error() << '\n';
    }

    if (alice.GetPeerId() != 0 && bob.GetPeerId() != 0)
    {
        auto presence = alice.PublishPresence("selection=node-42");
        Check(presence.has_value(), "editor publishes presence");
        auto acquired = alice.AcquireLock("node-42");
        Check(acquired.has_value() && *acquired, "editor acquires authoritative node lock");
        auto submitted = alice.SubmitEdit("node-42", "position=1,2,3");
        Check(submitted.has_value() && *submitted == 1, "editor submits sequenced edit");

        auto snapshot = bob.GetSnapshot();
        Check(snapshot.has_value(), "second editor reads broker snapshot");
        if (snapshot)
        {
            Check(snapshot->peers.size() == 2, "snapshot contains both editor peers");
            Check(snapshot->locks.size() == 1 && snapshot->locks.front().nodeId == "node-42",
                  "snapshot contains authoritative lock");
            Check(snapshot->edits.size() == 1 && snapshot->edits.front().payload == "position=1,2,3",
                  "snapshot contains submitted edit payload");
            bool foundPresence = false;
            for (const auto& peer : snapshot->peers)
                foundPresence |= peer.id == alice.GetPeerId() && peer.presence == "selection=node-42";
            Check(foundPresence, "snapshot exposes published presence to another editor");
        }

        auto released = alice.ReleaseLock("node-42");
        Check(released.has_value(), "editor releases authoritative node lock");
        auto unlocked = bob.GetSnapshot();
        Check(unlocked.has_value() && unlocked->locks.empty(), "released lock disappears from broker snapshot");
    }

    auto bobLeft = bob.LeaveSession();
    Check(bobLeft.has_value(), "second editor leaves cleanly");
    bob.Disconnect();
    auto aliceLeft = alice.LeaveSession();
    Check(aliceLeft.has_value(), "first editor leaves cleanly");
    if (!administrationToken.empty())
    {
        auto deleted = alice.DeleteSession("process-smoke", administrationToken);
        Check(deleted.has_value(), "session administrator deletes completed session");
    }
    alice.Disconnect();

    Spark::Daemon::DaemonClient control;
    auto controlConnected = control.Connect(endpoint);
    Check(controlConnected.has_value(), "control client reconnects for graceful shutdown");
    if (controlConnected)
    {
        auto shutdown = control.Request(Spark::Daemon::ServiceId::Control,
                                        static_cast<uint16_t>(Spark::Daemon::ControlMessage::ShutdownRequest), {});
        Check(shutdown.has_value() &&
                  shutdown->messageType == static_cast<uint16_t>(Spark::Daemon::ControlMessage::ShutdownAck),
              "broker acknowledges graceful shutdown");
    }
    control.Disconnect();

    Check(server.WaitForExit(std::chrono::seconds(5)), "SparkCollabServer exits after shutdown request");
    if (server.IsRunning())
        server.Kill();
    const int exitCode = server.WaitForExit();
    Check(exitCode == 0, "SparkCollabServer exits successfully");
    const std::string output = server.ReadAllStdout();
    Check(output.find("listening on") != std::string::npos, "server reports listening endpoint");
    Check(output.find("shutdown complete") != std::string::npos, "server reports completed shutdown");

#if !defined(_WIN32)
    std::error_code removeError;
    std::filesystem::remove(endpoint, removeError);
#endif
    if (g_failures == 0)
        std::cout << "SparkCollabProcessSmoke: all checks passed\n";
    return g_failures == 0 ? 0 : 1;
}

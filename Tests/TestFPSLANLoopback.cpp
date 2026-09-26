/**
 * @file TestFPSLANLoopback.cpp
 * @brief MOD-315: FPSLAN two-client convergence across three real processes.
 *
 * FPSLAN_ThreeProcessLoopbackConvergence launches Tests/Fixtures/FPSLANLoopbackPeer.cpp three
 * times: one server and two independent clients, each with its own NetworkManager and
 * FPSMultiplayerSystem, talking only over loopback UDP. The clients play the round in
 * Tests/Fixtures/FPSLANLoopbackScenario.h (spawn, move, kill, respawn, score) through the FPS
 * module's public API. The test then requires:
 *
 * - every client's spawn and the target's respawn to be exactly the server's, on a server spawn point;
 * - the server to record exactly one death, the target's;
 * - all three views to agree on every player's position, life, health, kills, deaths and score,
 *   with the shooter credited one kill and the target one death;
 * - the server to drop both players once their clients quit (real departure, not local state).
 */

#include "TestFramework.h"
#include "Fixtures/FPSLANLoopbackScenario.h"
#include "Utils/Process.h"

#ifndef SPARK_TEST_FPSLAN_PEER_PATH
#error "SPARK_TEST_FPSLAN_PEER_PATH must name the SparkFPSLANLoopbackPeer executable (Tests/CMakeLists.txt)"
#endif

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using namespace FPSLANScenario;
    using Fields = std::map<std::string, std::string>;

    constexpr auto kScenarioTimeout = std::chrono::seconds(90);
    constexpr auto kCommandTimeout = std::chrono::seconds(10);
    constexpr const char* kPeerTimeoutSeconds = "150";
    constexpr size_t kLogTailLines = 60;

    /// `key=value` tokens of one protocol line (the text after the `FPSLAN ` prefix).
    Fields ParseFields(std::string_view line)
    {
        Fields fields;
        std::istringstream stream{std::string(line)};
        std::string token;
        while (stream >> token)
        {
            const size_t equals = token.find('=');
            if (equals != std::string::npos)
                fields[token.substr(0, equals)] = token.substr(equals + 1);
        }
        return fields;
    }

    bool StartsWith(std::string_view text, std::string_view prefix)
    {
        return text.substr(0, prefix.size()) == prefix;
    }

    /// One harness process: its protocol lines, and a bounded tail of everything else it wrote.
    struct Peer
    {
        explicit Peer(std::string peerName) : name(std::move(peerName)) {}

        std::string name;
        std::optional<Spark::Process> process;
        std::vector<std::string> lines;
        std::deque<std::string> logTail;
        std::optional<int> exitCode;

        void Pump()
        {
            if (!process)
                return;
            std::string line;
            while (process->TryReadLine(line))
                Record(line);
            if (!exitCode)
            {
                exitCode = process->GetExitCode();
                if (exitCode)
                {
                    std::istringstream rest(process->ReadAllStdout());
                    while (std::getline(rest, line))
                        Record(line);
                }
            }
        }

        void Record(std::string line)
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (StartsWith(line, kLinePrefix))
                lines.push_back(line.substr(kLinePrefix.size()));
            logTail.push_back(std::move(line));
            if (logTail.size() > kLogTailLines)
                logTail.pop_front();
        }

        /// Fields of the first protocol line at or after @p from that starts with @p head.
        std::optional<Fields> Find(std::string_view head, size_t from = 0) const
        {
            for (size_t index = from; index < lines.size(); ++index)
            {
                if (StartsWith(lines[index], head))
                    return ParseFields(lines[index]);
            }
            return std::nullopt;
        }

        size_t Count(std::string_view head) const
        {
            size_t count = 0;
            for (const std::string& line : lines)
                count += StartsWith(line, head) ? 1 : 0;
            return count;
        }

        void Send(std::string_view command)
        {
            if (process && !exitCode)
                process->WriteStdin(std::string(command) + "\n");
        }

        void Dump() const
        {
            std::cout << "---- " << name << " (exit " << (exitCode ? std::to_string(*exitCode) : std::string("running"))
                      << ") ----\n";
            for (const std::string& line : logTail)
                std::cout << "  " << line << '\n';
        }
    };

    std::optional<Spark::Process> LaunchPeer(const std::vector<std::string>& args)
    {
        Spark::Process::Builder builder(SPARK_TEST_FPSLAN_PEER_PATH);
        for (const std::string& arg : args)
            builder.Arg(arg);
        builder.Arg("--timeout").Arg(kPeerTimeoutSeconds);
        auto launched = builder.CaptureStdin().CaptureStdout().MergeStderrIntoStdout().NoWindow().Launch();
        if (!launched)
        {
            std::cout << "FPSLAN: cannot launch " << SPARK_TEST_FPSLAN_PEER_PATH << ": " << launched.error() << '\n';
            return std::nullopt;
        }
        return std::move(*launched);
    }

    /// Pump every peer until @p done holds. Fails early when a peer exits that must keep running.
    bool PumpUntil(std::vector<Peer*> peers, const std::function<bool()>& done, std::chrono::seconds timeout,
                   bool peersMayExit = false)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            for (Peer* peer : peers)
                peer->Pump();
            if (done())
                return true;
            if (!peersMayExit)
            {
                for (const Peer* peer : peers)
                {
                    if (peer->exitCode)
                        return false;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    struct PlayerView
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        bool alive = false;
        float health = 0.0f;
        long long kills = -1;
        long long deaths = -1;
        long long score = -1;
    };

    struct Report
    {
        uint32_t self = 0;
        std::map<uint32_t, PlayerView> players;
    };

    float FieldFloat(const Fields& fields, const char* key)
    {
        const auto it = fields.find(key);
        return it != fields.end() ? std::strtof(it->second.c_str(), nullptr) : std::nanf("");
    }

    long long FieldInt(const Fields& fields, const char* key)
    {
        const auto it = fields.find(key);
        return it != fields.end() ? std::atoll(it->second.c_str()) : -1;
    }

    /// Ask @p peer for its view and collect the `player` lines of that one report.
    std::optional<Report> RequestReport(Peer& peer)
    {
        const size_t from = peer.lines.size();
        peer.Send(kCommandReport);
        if (!PumpUntil({&peer}, [&] { return peer.Find("report-end", from).has_value(); }, kCommandTimeout))
            return std::nullopt;

        Report report;
        report.self = static_cast<uint32_t>(FieldInt(*peer.Find("report-end", from), "self"));
        for (size_t index = from; index < peer.lines.size(); ++index)
        {
            if (StartsWith(peer.lines[index], "report-end"))
                break;
            if (!StartsWith(peer.lines[index], "player "))
                continue;
            const Fields fields = ParseFields(peer.lines[index]);
            PlayerView view;
            view.x = FieldFloat(fields, "x");
            view.y = FieldFloat(fields, "y");
            view.z = FieldFloat(fields, "z");
            view.alive = FieldInt(fields, "alive") == 1;
            view.health = FieldFloat(fields, "health");
            view.kills = FieldInt(fields, "kills");
            view.deaths = FieldInt(fields, "deaths");
            view.score = FieldInt(fields, "score");
            report.players[static_cast<uint32_t>(FieldInt(fields, "id"))] = view;
        }
        return report;
    }

    bool IsSpawnPoint(float x, float y, float z)
    {
        for (const Point& point : kSpawnPoints)
        {
            if (std::abs(point.x - x) < 1e-3f && std::abs(point.y - y) < 1e-3f && std::abs(point.z - z) < 1e-3f)
                return true;
        }
        return false;
    }

    /// Client @p event (spawn or respawn) for @p id must match the server's record exactly.
    void ExpectSameEvent(const Peer& server, const Peer& client, const std::string& event, uint32_t id)
    {
        const std::string head = "event=" + event + " id=" + std::to_string(id) + " ";
        const std::optional<Fields> authoritative = server.Find(head);
        const std::optional<Fields> observed = client.Find(head);
        ASSERT_TRUE(authoritative.has_value());
        ASSERT_TRUE(observed.has_value());
        const float x = FieldFloat(*authoritative, "x");
        const float y = FieldFloat(*authoritative, "y");
        const float z = FieldFloat(*authoritative, "z");
        EXPECT_TRUE(IsSpawnPoint(x, y, z));
        EXPECT_NEAR(FieldFloat(*observed, "x"), x, 1e-3f);
        EXPECT_NEAR(FieldFloat(*observed, "y"), y, 1e-3f);
        EXPECT_NEAR(FieldFloat(*observed, "z"), z, 1e-3f);
    }

    void ExpectPost(const PlayerView& view, const Point& post)
    {
        EXPECT_NEAR(view.x, post.x, kConvergenceTolerance);
        EXPECT_NEAR(view.z, post.z, kConvergenceTolerance);
    }

    void ExpectSameView(const Report& server, const Report& client)
    {
        ASSERT_EQ(client.players.size(), server.players.size());
        for (const auto& [id, authoritative] : server.players)
        {
            const auto it = client.players.find(id);
            ASSERT_TRUE(it != client.players.end());
            const PlayerView& observed = it->second;
            EXPECT_NEAR(observed.x, authoritative.x, kConvergenceTolerance);
            EXPECT_NEAR(observed.y, authoritative.y, kConvergenceTolerance);
            EXPECT_NEAR(observed.z, authoritative.z, kConvergenceTolerance);
            EXPECT_EQ(observed.alive, authoritative.alive);
            EXPECT_NEAR(observed.health, authoritative.health, 1e-3f);
            EXPECT_EQ(observed.kills, authoritative.kills);
            EXPECT_EQ(observed.deaths, authoritative.deaths);
            EXPECT_EQ(observed.score, authoritative.score);
        }
    }
} // namespace

TEST(FPSLAN_ThreeProcessLoopbackConvergence)
{
    Peer server("server");
    Peer shooter("shooter");
    Peer target("target");
    const auto dumpAll = [&]
    {
        server.Dump();
        shooter.Dump();
        target.Dump();
    };

    server.process = LaunchPeer({"--role", "server"});
    ASSERT_TRUE(server.process.has_value());
    const bool ready = PumpUntil({&server}, [&] { return server.Find("ready ").has_value(); }, kCommandTimeout);
    if (!ready)
        dumpAll();
    ASSERT_TRUE(ready);
    const Fields readyFields = *server.Find("ready ");
    const std::string port = readyFields.at("port");
    const std::string hostText = readyFields.at("host");
    const auto hostId = static_cast<uint32_t>(std::stoul(hostText));

    shooter.process = LaunchPeer({"--role", "shooter", "--port", port, "--host-id", hostText});
    target.process = LaunchPeer({"--role", "target", "--port", port, "--host-id", hostText});
    ASSERT_TRUE(shooter.process.has_value());
    ASSERT_TRUE(target.process.has_value());

    // The whole round plays out with no coordinator involvement: both clients report done only
    // when their own view shows the kill, the score, the respawn and both players at rest.
    const bool finished = PumpUntil(
        {&server, &shooter, &target}, [&]
        { return shooter.Find("event=done").has_value() && target.Find("event=done").has_value(); }, kScenarioTimeout);
    if (!finished)
        dumpAll();
    ASSERT_TRUE(finished);

    const uint32_t shooterId = static_cast<uint32_t>(FieldInt(*shooter.Find("event=done"), "id"));
    const uint32_t targetId = static_cast<uint32_t>(FieldInt(*target.Find("event=done"), "id"));
    EXPECT_NE(shooterId, targetId);
    EXPECT_NE(shooterId, hostId);
    EXPECT_NE(targetId, hostId);

    // Spawn and respawn are server decisions that each client observed exactly.
    ExpectSameEvent(server, shooter, "spawn", shooterId);
    ExpectSameEvent(server, target, "spawn", targetId);
    ExpectSameEvent(server, target, "respawn", targetId);
    EXPECT_EQ(server.Count("event=death "), size_t{1});
    EXPECT_TRUE(server.Find("event=death id=" + std::to_string(targetId)).has_value());
    EXPECT_TRUE(target.Find("event=death id=" + std::to_string(targetId)).has_value());
    EXPECT_TRUE(shooter.Find("event=kill-observed target=" + std::to_string(targetId)).has_value());

    const std::optional<Report> authoritative = RequestReport(server);
    const std::optional<Report> shooterView = RequestReport(shooter);
    const std::optional<Report> targetView = RequestReport(target);
    if (!authoritative || !shooterView || !targetView)
        dumpAll();
    ASSERT_TRUE(authoritative.has_value());
    ASSERT_TRUE(shooterView.has_value());
    ASSERT_TRUE(targetView.has_value());
    EXPECT_EQ(authoritative->self, hostId);
    EXPECT_EQ(shooterView->self, shooterId);
    EXPECT_EQ(targetView->self, targetId);

    // The authoritative round: one kill credited, one death, everyone alive, both clients on post.
    ASSERT_EQ(authoritative->players.size(), size_t{3});
    ASSERT_TRUE(authoritative->players.contains(hostId));
    ASSERT_TRUE(authoritative->players.contains(shooterId));
    ASSERT_TRUE(authoritative->players.contains(targetId));
    const PlayerView& shooterState = authoritative->players.at(shooterId);
    const PlayerView& targetState = authoritative->players.at(targetId);
    const PlayerView& hostState = authoritative->players.at(hostId);
    EXPECT_EQ(shooterState.kills, 1LL);
    EXPECT_EQ(shooterState.deaths, 0LL);
    EXPECT_EQ(shooterState.score, static_cast<long long>(kKillScore));
    EXPECT_EQ(targetState.kills, 0LL);
    EXPECT_EQ(targetState.deaths, 1LL);
    EXPECT_EQ(targetState.score, 0LL);
    EXPECT_EQ(hostState.kills, 0LL);
    EXPECT_EQ(hostState.deaths, 0LL);
    EXPECT_TRUE(shooterState.alive);
    EXPECT_TRUE(targetState.alive);
    EXPECT_NEAR(targetState.health, kFullHealth, 1e-3f);
    ExpectPost(shooterState, kShooterPost);
    ExpectPost(targetState, kTargetReturnPost);

    // Both independent clients converged on exactly that state.
    ExpectSameView(*authoritative, *shooterView);
    ExpectSameView(*authoritative, *targetView);

    // Leaving is real connection state: the server drops each player only when its client quits.
    shooter.Send(kCommandQuit);
    target.Send(kCommandQuit);
    const bool clientsExited = PumpUntil(
        {&server, &shooter, &target}, [&] { return shooter.exitCode.has_value() && target.exitCode.has_value(); },
        kCommandTimeout, true);
    if (!clientsExited)
        dumpAll();
    ASSERT_TRUE(clientsExited);
    EXPECT_EQ(*shooter.exitCode, kExitOk);
    EXPECT_EQ(*target.exitCode, kExitOk);

    const bool departed = PumpUntil(
        {&server},
        [&]
        {
            return server.Find("event=leave id=" + std::to_string(shooterId)).has_value() &&
                   server.Find("event=leave id=" + std::to_string(targetId)).has_value();
        },
        kCommandTimeout);
    if (!departed)
        dumpAll();
    ASSERT_TRUE(departed);
    const std::optional<Report> afterLeave = RequestReport(server);
    ASSERT_TRUE(afterLeave.has_value());
    EXPECT_EQ(afterLeave->players.size(), size_t{1});
    EXPECT_TRUE(afterLeave->players.contains(hostId));

    server.Send(kCommandQuit);
    const bool serverExited = PumpUntil({&server}, [&] { return server.exitCode.has_value(); }, kCommandTimeout, true);
    if (!serverExited)
        dumpAll();
    ASSERT_TRUE(serverExited);
    EXPECT_EQ(*server.exitCode, kExitOk);
}

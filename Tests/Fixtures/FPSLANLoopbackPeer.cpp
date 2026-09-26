/**
 * @file FPSLANLoopbackPeer.cpp
 * @brief MOD-315: one process of the three-process FPSLAN loopback harness.
 *
 * The peer runs the production SparkFPS::FPSMultiplayerSystem (the FPS module's only network
 * path) against the real NetworkManager singleton of its own process, so the server and both
 * clients share nothing but loopback UDP datagrams:
 *
 *   SparkFPSLANLoopbackPeer --role server [--timeout <seconds>]
 *   SparkFPSLANLoopbackPeer --role shooter|target --port <port> --host-id <id> [--timeout <seconds>]
 *
 * The server hosts on an ephemeral port and prints `FPSLAN ready port=<port> host=<id>`. Each
 * client plays its side of the round in FPSLANLoopbackScenario.h, driving the system only through
 * its public API (Update, SendInput at 60 Hz, GetAllPlayerStates, GetScoreboard), and prints
 * `FPSLAN event=done` once its own view shows the finished round. Every protocol line starts with
 * `FPSLAN `; the coordinator (Tests/TestFPSLANLoopback.cpp) ignores the rest of the output.
 *
 * stdin carries the coordinator's commands, one per line: `report` prints this peer's view of
 * every player followed by `FPSLAN report-end`, and `quit` (or end of input) leaves the session
 * through Shutdown and exits 0. Scenario, network and timeout failures print `FPSLAN error ...`
 * and exit with the non-zero codes in FPSLANLoopbackScenario.h.
 *
 * Contract: single game thread plus one detached stdin reader that only sets atomics. Frames are
 * paced at 60 Hz of wall-clock time; a late frame is not caught up, so a client never submits
 * input faster than the server's input budget earns it.
 */

#include "FPSLANLoopbackScenario.h"
#include "Engine/Networking/NetworkManager.h"
#include "Game/MultiplayerSystem.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <expected>
#include <format>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace
{
    using namespace FPSLANScenario;
    using SparkFPS::FPSMultiplayerSystem;
    using SparkFPS::NetworkPlayerState;
    using SparkFPS::PlayerInput;
    using SparkFPS::PlayerScore;

    constexpr float kPi = 3.14159265358979f;
    /// A turn is finished when the held yaw is this close (radians) to the wanted heading.
    constexpr float kYawTolerance = 1e-3f;
    /// A resting player's replicated speed (m/s) stays below this.
    constexpr float kRestingSpeed = 1e-3f;
    /// The shooter gives up when this many fire inputs (10 s) have not killed the target.
    constexpr uint32_t kMaxFireInputs = 600;
    /// Longest frame the server simulates; a stalled process resumes instead of jumping.
    constexpr float kMaxFrameSeconds = 0.1f;

    enum class Role
    {
        Server,
        Shooter,
        Target,
    };

    struct Options
    {
        Role role = Role::Server;
        uint16_t port = 0;
        uint32_t hostId = 0;
        bool hasHostId = false;
        double timeoutSeconds = 120.0;
    };

    template <typename... Args> void Emit(std::format_string<Args...> format, Args&&... args)
    {
        std::string line(kLinePrefix);
        line += std::format(format, std::forward<Args>(args)...);
        line += '\n';
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fflush(stdout);
    }

    std::optional<Options> ParseOptions(int argc, char** argv)
    {
        Options options;
        bool hasRole = false;
        bool hasPort = false;
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view flag = argv[index];
            if (index + 1 >= argc)
                return std::nullopt;
            const std::string value = argv[++index];
            try
            {
                if (flag == "--role")
                {
                    hasRole = true;
                    if (value == "server")
                        options.role = Role::Server;
                    else if (value == "shooter")
                        options.role = Role::Shooter;
                    else if (value == "target")
                        options.role = Role::Target;
                    else
                        return std::nullopt;
                }
                else if (flag == "--port")
                {
                    const unsigned long port = std::stoul(value);
                    if (port == 0 || port > 65535)
                        return std::nullopt;
                    options.port = static_cast<uint16_t>(port);
                    hasPort = true;
                }
                else if (flag == "--host-id")
                {
                    options.hostId = static_cast<uint32_t>(std::stoul(value));
                    options.hasHostId = true;
                }
                else if (flag == "--timeout")
                {
                    options.timeoutSeconds = std::stod(value);
                    if (!(options.timeoutSeconds > 0.0))
                        return std::nullopt;
                }
                else
                {
                    return std::nullopt;
                }
            }
            catch (const std::exception&)
            {
                return std::nullopt;
            }
        }

        const bool isClient = options.role != Role::Server;
        if (!hasRole || isClient != hasPort || isClient != options.hasHostId)
            return std::nullopt;
        return options;
    }

    /// Coordinator commands read by a detached thread. The thread owns a shared reference to the
    /// flags, so it may outlive main (still blocked on stdin) without touching freed memory.
    class CommandChannel
    {
      public:
        CommandChannel() : m_state(std::make_shared<State>())
        {
            std::thread(
                [state = m_state]
                {
                    std::string line;
                    while (std::getline(std::cin, line))
                    {
                        if (!line.empty() && line.back() == '\r')
                            line.pop_back();
                        if (line == kCommandReport)
                            state->reportRequests.fetch_add(1);
                        else if (line == kCommandQuit)
                            break;
                    }
                    state->quit.store(true);
                })
                .detach();
        }

        bool TakeReportRequest()
        {
            int pending = m_state->reportRequests.load();
            while (pending > 0 && !m_state->reportRequests.compare_exchange_weak(pending, pending - 1))
            {
            }
            return pending > 0;
        }

        bool QuitRequested() const { return m_state->quit.load(); }

      private:
        struct State
        {
            std::atomic<int> reportRequests{0};
            std::atomic<bool> quit{false};
        };
        std::shared_ptr<State> m_state;
    };

    /// 60 Hz wall-clock pacing. Returns the real seconds since the previous frame.
    class FrameClock
    {
      public:
        FrameClock() : m_previous(Clock::now()), m_next(m_previous) {}

        float Tick()
        {
            m_next += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(kInputStep));
            std::this_thread::sleep_until(m_next);
            const Clock::time_point now = Clock::now();
            if (now - m_next > std::chrono::milliseconds(100))
                m_next = now; // Do not replay a stall as a burst of frames.
            const float elapsed = std::chrono::duration<float>(now - m_previous).count();
            m_previous = now;
            return std::clamp(elapsed, 1e-4f, kMaxFrameSeconds);
        }

      private:
        using Clock = std::chrono::steady_clock;
        Clock::time_point m_previous;
        Clock::time_point m_next;
    };

    float PlanarDistance(const NetworkPlayerState& state, const Point& point)
    {
        return std::hypot(point.x - state.posX, point.z - state.posZ);
    }

    float PlanarSpeed(const NetworkPlayerState& state)
    {
        return std::hypot(state.velX, state.velZ);
    }

    std::optional<PlayerScore> ScoreOf(const FPSMultiplayerSystem& system, uint32_t clientId)
    {
        for (const PlayerScore& score : system.GetScoreboard())
        {
            if (score.clientId == clientId)
                return score;
        }
        return std::nullopt;
    }

    void PrintReport(const FPSMultiplayerSystem& system, std::string_view role)
    {
        const std::map<uint32_t, NetworkPlayerState> players(system.GetAllPlayerStates().begin(),
                                                             system.GetAllPlayerStates().end());
        for (const auto& [id, state] : players)
        {
            const PlayerScore score = ScoreOf(system, id).value_or(PlayerScore{});
            Emit("player id={} x={:.4f} y={:.4f} z={:.4f} alive={} health={:.2f} kills={} deaths={} score={}", id,
                 state.posX, state.posY, state.posZ, state.isAlive ? 1 : 0, state.health, score.kills, score.deaths,
                 score.score);
        }
        Emit("report-end role={} self={} players={}", role, system.GetLocalClientId(), players.size());
    }

    /// Hold still, keeping the current heading.
    PlayerInput IdleInput(const NetworkPlayerState& self)
    {
        PlayerInput input;
        input.yaw = self.yaw;
        return input;
    }

    /// One step toward @p post. The server moves each input along the heading held before it,
    /// so a heading change is sent as a standing turn and the walk follows on later steps.
    PlayerInput SteerTo(const NetworkPlayerState& self, const Point& post, bool& arrived)
    {
        const float distance = PlanarDistance(self, post);
        arrived = distance <= kArrivalTolerance;
        if (arrived)
            return IdleInput(self);

        PlayerInput input;
        input.yaw = std::atan2(post.z - self.posZ, post.x - self.posX);
        const float turn = std::remainder(input.yaw - self.yaw, 2.0f * kPi);
        if (std::abs(turn) <= kYawTolerance)
            input.forward = (std::min)(1.0f, distance / (kMoveSpeed * kInputStep));
        return input;
    }

    /// The client's side of the round. It sees the session only as the FPS module's public API
    /// exposes it: its predicted own state and the replicated states and scores of the others.
    class ClientScript
    {
      public:
        ClientScript(Role role, uint32_t hostId) : m_role(role), m_hostId(hostId) {}

        std::string_view PhaseName() const { return kPhaseNames[static_cast<size_t>(m_phase)]; }

        /// Next input for this frame, or an error text when the round cannot finish.
        std::expected<PlayerInput, std::string> Step(const FPSMultiplayerSystem& system)
        {
            const uint32_t selfId = system.GetLocalClientId();
            const NetworkPlayerState* self = system.GetPlayerState(selfId);
            if (!self)
                return std::unexpected(std::string("own player state missing"));
            const NetworkPlayerState* peer = FindPeer(system, selfId);

            switch (m_phase)
            {
            case Phase::Spawn:
                // sequenceNumber stays 0 until the first authoritative snapshot is reconciled.
                if (self->sequenceNumber != 0)
                {
                    Emit("event=spawn id={} x={:.4f} y={:.4f} z={:.4f}", selfId, self->posX, self->posY, self->posZ);
                    m_phase = Phase::Approach;
                }
                return IdleInput(*self);

            case Phase::Approach:
            {
                bool arrived = false;
                const PlayerInput input = SteerTo(*self, m_role == Role::Shooter ? kShooterPost : kTargetPost, arrived);
                if (arrived)
                {
                    Emit("event=arrived id={}", selfId);
                    m_phase = Phase::Hold;
                }
                return input;
            }

            case Phase::Hold:
                if (m_role == Role::Shooter)
                {
                    if (peer && peer->isAlive && PlanarDistance(*peer, kTargetPost) <= kConvergenceTolerance)
                    {
                        Emit("event=engage target={}", peer->clientId);
                        m_phase = Phase::Fire;
                    }
                }
                else if (!self->isAlive)
                {
                    Emit("event=death id={}", selfId);
                    m_phase = Phase::Dead;
                }
                return IdleInput(*self);

            case Phase::Fire:
            {
                if (!peer)
                    return std::unexpected(std::string("target left while under fire"));
                if (!peer->isAlive)
                {
                    Emit("event=kill-observed target={} shots={}", peer->clientId, m_fireInputs);
                    m_phase = Phase::Settle;
                    return IdleInput(*self);
                }
                if (++m_fireInputs > kMaxFireInputs)
                    return std::unexpected(std::format("target survived {} fire inputs", kMaxFireInputs));
                PlayerInput input;
                input.yaw = std::atan2(peer->posZ - self->posZ, peer->posX - self->posX);
                input.fire = true;
                return input;
            }

            case Phase::Dead:
                if (self->isAlive)
                {
                    Emit("event=respawn id={} x={:.4f} y={:.4f} z={:.4f}", selfId, self->posX, self->posY, self->posZ);
                    m_phase = Phase::Return;
                }
                return IdleInput(*self);

            case Phase::Return:
            {
                bool arrived = false;
                const PlayerInput input = SteerTo(*self, kTargetReturnPost, arrived);
                if (arrived)
                {
                    Emit("event=arrived id={}", selfId);
                    m_phase = Phase::Settle;
                }
                return input;
            }

            case Phase::Settle:
                if (peer && RoundFinished(system, selfId, *self, *peer))
                {
                    Emit("event=done id={}", selfId);
                    m_phase = Phase::Done;
                }
                return IdleInput(*self);

            case Phase::Done:
                return IdleInput(*self);
            }
            return std::unexpected(std::string("unknown phase"));
        }

      private:
        enum class Phase : size_t
        {
            Spawn,
            Approach,
            Hold,
            Fire,
            Dead,
            Return,
            Settle,
            Done,
        };
        static constexpr std::array<std::string_view, 8> kPhaseNames{"spawn", "approach", "hold",   "fire",
                                                                     "dead",  "return",   "settle", "done"};

        /// The other client: the one player that is neither this client nor the host.
        const NetworkPlayerState* FindPeer(const FPSMultiplayerSystem& system, uint32_t selfId) const
        {
            const NetworkPlayerState* peer = nullptr;
            for (const auto& [id, state] : system.GetAllPlayerStates())
            {
                if (id == selfId || id == m_hostId)
                    continue;
                if (peer)
                    return nullptr; // More than one other client: not this scenario's session.
                peer = &state;
            }
            return peer;
        }

        /// This client's view shows the whole round: the shooter credited with the one kill, the
        /// target dead once and alive again at full health, both at rest on their final posts. Posts
        /// use the arrival tolerance, tighter than the coordinator's convergence check, so a remote
        /// player still interpolating toward its post does not end the round early.
        bool RoundFinished(const FPSMultiplayerSystem& system, uint32_t selfId, const NetworkPlayerState& self,
                           const NetworkPlayerState& peer) const
        {
            const bool selfIsShooter = m_role == Role::Shooter;
            const NetworkPlayerState& shooter = selfIsShooter ? self : peer;
            const NetworkPlayerState& target = selfIsShooter ? peer : self;
            const std::optional<PlayerScore> shooterScore = ScoreOf(system, shooter.clientId);
            const std::optional<PlayerScore> targetScore = ScoreOf(system, target.clientId);
            const std::optional<PlayerScore> hostScore = ScoreOf(system, m_hostId);
            if (!shooterScore || !targetScore || !hostScore || system.GetAllPlayerStates().size() != 3 ||
                system.GetPlayerState(m_hostId) == nullptr || selfId != self.clientId)
            {
                return false;
            }

            return shooterScore->kills == 1 && shooterScore->deaths == 0 && shooterScore->score == kKillScore &&
                   targetScore->kills == 0 && targetScore->deaths == 1 && targetScore->score == 0 &&
                   hostScore->kills == 0 && hostScore->deaths == 0 && shooter.isAlive && target.isAlive &&
                   target.health == kFullHealth && PlanarSpeed(shooter) < kRestingSpeed &&
                   PlanarSpeed(target) < kRestingSpeed && PlanarDistance(shooter, kShooterPost) <= kArrivalTolerance &&
                   PlanarDistance(target, kTargetReturnPost) <= kArrivalTolerance;
        }

        Role m_role;
        uint32_t m_hostId;
        Phase m_phase = Phase::Spawn;
        uint32_t m_fireInputs = 0;
    };

    /// Server: announce each authoritative spawn, death, respawn and departure as it happens, so
    /// the coordinator can hold every client's observation against the server's own record.
    class ServerObserver
    {
      public:
        void Observe(const FPSMultiplayerSystem& system)
        {
            const auto& players = system.GetAllPlayerStates();
            for (const auto& [id, state] : players)
            {
                const auto known = m_alive.find(id);
                if (known == m_alive.end())
                {
                    Emit("event=spawn id={} x={:.4f} y={:.4f} z={:.4f}", id, state.posX, state.posY, state.posZ);
                }
                else if (known->second && !state.isAlive)
                {
                    Emit("event=death id={}", id);
                }
                else if (!known->second && state.isAlive)
                {
                    Emit("event=respawn id={} x={:.4f} y={:.4f} z={:.4f}", id, state.posX, state.posY, state.posZ);
                }
                m_alive[id] = state.isAlive;
            }
            for (auto it = m_alive.begin(); it != m_alive.end();)
            {
                if (!players.contains(it->first))
                {
                    Emit("event=leave id={}", it->first);
                    it = m_alive.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

      private:
        std::map<uint32_t, bool> m_alive;
    };

    int Leave(FPSMultiplayerSystem& system, int exitCode)
    {
        system.Shutdown();
        Spark::Net::NetworkManager::GetInstance().Shutdown();
        return exitCode;
    }

    int RunServer(const Options& options, CommandChannel& commands)
    {
        auto& system = FPSMultiplayerSystem::GetInstance();
        for (const Point& point : kSpawnPoints)
            system.AddSpawnPoint({point.x, point.y, point.z, 0.0f});
        system.Initialize(true);
        if (!system.StartServer(0, 4))
        {
            Emit("error reason=start-server-failed");
            return Leave(system, kExitNetworkFailure);
        }
        Emit("ready port={} host={}", Spark::Net::NetworkManager::GetInstance().GetBoundPort(),
             system.GetLocalClientId());

        ServerObserver observer;
        FrameClock clock;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(options.timeoutSeconds);
        while (!commands.QuitRequested())
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                Emit("error reason=timeout role=server");
                return Leave(system, kExitTimeout);
            }
            system.Update(clock.Tick());
            if (!system.IsActive())
            {
                Emit("error reason=server-stopped");
                return Leave(system, kExitNetworkFailure);
            }
            observer.Observe(system);
            while (commands.TakeReportRequest())
                PrintReport(system, "server");
        }
        return Leave(system, kExitOk);
    }

    int RunClient(const Options& options, CommandChannel& commands)
    {
        auto& system = FPSMultiplayerSystem::GetInstance();
        system.Initialize(false);
        if (!system.Connect("127.0.0.1", options.port))
        {
            Emit("error reason=connect-failed port={}", options.port);
            return Leave(system, kExitNetworkFailure);
        }

        const std::string_view roleName = options.role == Role::Shooter ? "shooter" : "target";
        ClientScript script(options.role, options.hostId);
        FrameClock clock;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(options.timeoutSeconds);
        while (!commands.QuitRequested())
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                Emit("error reason=timeout role={} phase={}", roleName, script.PhaseName());
                return Leave(system, kExitTimeout);
            }
            system.Update(clock.Tick());
            if (!system.IsActive())
            {
                Emit("error reason=session-ended role={} phase={}", roleName, script.PhaseName());
                return Leave(system, kExitNetworkFailure);
            }
            while (commands.TakeReportRequest())
                PrintReport(system, roleName);

            // No player exists until the handshake assigns this client an id.
            if (system.GetLocalClientId() == Spark::Net::INVALID_CLIENT)
                continue;

            const std::expected<PlayerInput, std::string> input = script.Step(system);
            if (!input)
            {
                Emit("error reason=scenario role={} phase={} detail=\"{}\"", roleName, script.PhaseName(),
                     input.error());
                return Leave(system, kExitScenarioFailure);
            }
            system.SendInput(*input);
        }
        return Leave(system, kExitOk);
    }
} // namespace

int main(int argc, char** argv)
{
    const std::optional<Options> options = ParseOptions(argc, argv);
    if (!options)
    {
        std::fputs("usage: SparkFPSLANLoopbackPeer --role server [--timeout <s>]\n"
                   "       SparkFPSLANLoopbackPeer --role shooter|target --port <port> --host-id <id> "
                   "[--timeout <s>]\n",
                   stderr);
        return kExitUsage;
    }

    CommandChannel commands;
    return options->role == Role::Server ? RunServer(*options, commands) : RunClient(*options, commands);
}

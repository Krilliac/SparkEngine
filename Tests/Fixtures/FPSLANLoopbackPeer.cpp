/**
 * @file FPSLANLoopbackPeer.cpp
 * @brief MOD-315: one process of the three-process FPSLAN loopback harness.
 *
 * The peer runs the production SparkFPS::FPSMultiplayerSystem (the FPS module's only network
 * path) against the real NetworkManager singleton of its own process, so the server and both
 * clients share nothing but loopback UDP datagrams:
 *
 *   SparkFPSLANLoopbackPeer --role server [--timeout <seconds>]
 *   SparkFPSLANLoopbackPeer --role shooter|target --port <port> --host-id <id> --server-key <hex>
 *                           [--timeout <seconds>]
 *   SparkFPSLANLoopbackPeer --role intruder --port <port> [--timeout <seconds>]
 *
 * The server hosts on an ephemeral port with an ephemeral NET-100 identity and prints
 * `FPSLAN ready port=<port> host=<id> key=<hex>`; clients pin that key (--server-key). Each
 * client plays its side of the round in FPSLANLoopbackScenario.h, driving the system only through
 * its public API (Update, SendInput at 60 Hz, GetAllPlayerStates, GetScoreboard), and prints
 * `FPSLAN event=done` once its own view shows the finished round. Every protocol line starts with
 * `FPSLAN `; the coordinator (Tests/TestFPSLANLoopback.cpp) ignores the rest of the output.
 *
 * stdin carries the coordinator's commands, one per line: `report` prints this peer's view of
 * every player and its NetworkManager transport counters (`FPSLAN stats ...`) followed by
 * `FPSLAN report-end`, and `quit` (or end of input) leaves the session through Shutdown and exits
 * 0. Scenario, network and timeout failures print `FPSLAN error ...` and exit with the non-zero
 * codes in FPSLANLoopbackScenario.h.
 *
 * The intruder is no player: it opens a bare UDPTransport socket and sends the server hostile
 * datagrams (unframed and unknown-kind bytes, forged and truncated sealed frames, maximum-size
 * datagrams, bursts, and Connects with no handshake magic, a future protocol version or a
 * malformed ClientHello), collects whatever the server answers, prints one
 * `FPSLAN intruder ...` summary of what it sent and received, and exits 0 within a few seconds
 * (it accepts --timeout like every role but never runs that long).
 *
 * Contract: single game thread plus one detached stdin reader that only sets atomics. Frames are
 * paced at 60 Hz of wall-clock time; a late frame is not caught up, so a client never submits
 * input faster than the server's input budget earns it.
 */

#include "FPSLANLoopbackScenario.h"
#include "Engine/Networking/ITransport.h"
#include "Engine/Networking/NetworkManager.h"
#include "Engine/Networking/NetworkWireLimits.h"
#include "Game/MultiplayerSystem.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
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
#include <numeric>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

/// The intruder's bare UDPTransport (Tests/Fixtures/FPSLANIntruderTransport.cpp).
std::unique_ptr<Spark::Net::ITransport> MakeFPSLANIntruderTransport();

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
        Intruder,
    };

    struct Options
    {
        Role role = Role::Server;
        uint16_t port = 0;
        uint32_t hostId = 0;
        bool hasHostId = false;
        double timeoutSeconds = 120.0;
        Spark::Net::ServerPublicKey serverKey{}; ///< NET-100: clients pin the key the server printed
        bool hasServerKey = false;
    };

    std::string KeyToHex(const Spark::Net::ServerPublicKey& key)
    {
        std::string hex;
        for (const uint8_t byte : key)
            hex += std::format("{:02x}", byte);
        return hex;
    }

    bool HexToKey(std::string_view hex, Spark::Net::ServerPublicKey& key)
    {
        if (hex.size() != key.size() * 2)
            return false;
        for (size_t i = 0; i < key.size(); ++i)
        {
            unsigned value = 0;
            const auto [end, ec] = std::from_chars(hex.data() + 2 * i, hex.data() + 2 * i + 2, value, 16);
            if (ec != std::errc() || end != hex.data() + 2 * i + 2)
                return false;
            key[i] = static_cast<uint8_t>(value);
        }
        return true;
    }

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
                    else if (value == "intruder")
                        options.role = Role::Intruder;
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
                else if (flag == "--server-key")
                {
                    if (!HexToKey(value, options.serverKey))
                        return std::nullopt;
                    options.hasServerKey = true;
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

        // Every role but the server targets a port; only the two players join the session.
        const bool targetsServer = options.role != Role::Server;
        const bool isPlayer = options.role == Role::Shooter || options.role == Role::Target;
        if (!hasRole || targetsServer != hasPort || isPlayer != options.hasHostId || isPlayer != options.hasServerKey)
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
        // NET-100 refusal counters: the coordinator holds hostile datagrams against them.
        const Spark::Net::NetworkStats stats = Spark::Net::NetworkManager::GetInstance().GetStats();
        const uint64_t securityDrops =
            std::accumulate(stats.securityDrops.begin(), stats.securityDrops.end(), uint64_t{0});
        Emit("stats plaintext-dropped={} handshake-failures={} security-drops={} rate-limited={}",
             stats.plaintextFramesDropped, stats.handshakeFailures, securityDrops, stats.connectsRateLimited);
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

    /// Stay on @p post after arriving. Arrival is judged on the predicted state, before the
    /// server has applied the last steps; when the server drops one of them (its input budget
    /// after a stalled frame under a slow sanitizer run), reconciliation moves the player back
    /// off the post, and holding still would leave it there for the rest of the round.
    PlayerInput HoldPost(const NetworkPlayerState& self, const Point& post)
    {
        bool onPost = false;
        return SteerTo(self, post, onPost);
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
                    return IdleInput(*self);
                }
                return HoldPost(*self, m_role == Role::Shooter ? kShooterPost : kTargetPost);

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
                    return IdleInput(*self);
                }
                return HoldPost(*self, m_role == Role::Shooter ? kShooterPost : kTargetReturnPost);

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
        // NET-100: an ephemeral in-memory identity (never the per-user key file); the
        // coordinator hands its public key to the clients, which pin it.
        auto identity = Spark::Net::GenerateServerIdentity();
        if (!identity)
        {
            Emit("error reason=identity-failed");
            return Leave(system, kExitNetworkFailure);
        }
        const Spark::Net::ServerPublicKey serverKey = identity->publicKey;
        Spark::Net::NetworkSecurityConfig security;
        security.identity = std::move(*identity);
        Spark::Net::NetworkManager::GetInstance().SetSecurityConfig(std::move(security));
        if (!system.StartServer(0, 4))
        {
            Emit("error reason=start-server-failed");
            return Leave(system, kExitNetworkFailure);
        }
        Emit("ready port={} host={} key={}", Spark::Net::NetworkManager::GetInstance().GetBoundPort(),
             system.GetLocalClientId(), KeyToHex(serverKey));

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
        Spark::Net::NetworkSecurityConfig security;
        security.trust = Spark::Net::ServerTrust::Pin(options.serverKey);
        Spark::Net::NetworkManager::GetInstance().SetSecurityConfig(std::move(security));
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

    /// One unframed wire message in NetworkManager::SerializeMessage's layout
    /// (docs/specs/networking-wire-format.md), built here so the intruder's bytes owe nothing to
    /// the serializer under test.
    std::vector<uint8_t> WireMessage(Spark::Net::MessageType type, const std::vector<uint8_t>& payload)
    {
        Spark::Net::NetBuffer buffer;
        buffer.WriteUint32(0x5350524B); // "SPRK"
        buffer.WriteUint16(static_cast<uint16_t>(type));
        buffer.WriteUint8(static_cast<uint8_t>(Spark::Net::ChannelType::Unreliable));
        buffer.WriteUint32(0); // sender
        buffer.WriteUint32(0); // sequence
        buffer.WriteFloat(0.0f);
        buffer.WriteUint32(static_cast<uint32_t>(payload.size()));
        buffer.WriteBytes(payload.data(), payload.size());
        return buffer.GetData();
    }

    /// [frame kind][Connect] carrying @p magic and @p version, padded to the Connect schema's
    /// 8-byte minimum so the server answers it instead of dropping it at the packet validator.
    std::vector<uint8_t> HandshakeConnect(uint32_t magic, uint16_t version)
    {
        Spark::Net::NetBuffer hello;
        hello.WriteUint32(magic);
        hello.WriteUint16(version);
        hello.WriteUint16(0);
        std::vector<uint8_t> frame{Spark::Net::NETWORK_FRAME_HANDSHAKE};
        const std::vector<uint8_t> message = WireMessage(Spark::Net::MessageType::Connect, hello.GetData());
        frame.insert(frame.end(), message.begin(), message.end());
        return frame;
    }

    /// What the intruder sent, and what the server answered it.
    struct IntrusionTally
    {
        uint32_t refusedFrames = 0;     ///< Datagrams the server must drop as unframed or unopenable
        uint32_t noMagicConnects = 0;   ///< Connects without the SPNH handshake magic
        uint32_t futureConnects = 0;    ///< Connects naming a protocol version the server does not speak
        uint32_t malformedConnects = 0; ///< Connects with magic and version but a truncated ClientHello
        uint32_t sendFailures = 0;
        std::map<Spark::Net::ConnectRejectReason, uint32_t> rejections;
        uint32_t accepted = 0; ///< ConnectAccepted answers: the intruder must never get one
        uint32_t otherReplies = 0;
    };

    /// Count one datagram the server sent the intruder. Only Handshake frames can reach an
    /// endpoint that never finished a handshake.
    void TallyReply(const uint8_t* data, size_t size, IntrusionTally& tally)
    {
        if (size < 1 + Spark::Net::NETWORK_WIRE_HEADER_SIZE || data[0] != Spark::Net::NETWORK_FRAME_HANDSHAKE)
        {
            ++tally.otherReplies;
            return;
        }
        Spark::Net::NetBuffer wire;
        wire.WriteBytes(data + 1, size - 1);
        const uint32_t magic = wire.ReadUint32();
        const auto type = static_cast<Spark::Net::MessageType>(wire.ReadUint16());
        wire.ReadUint8();  // channel
        wire.ReadUint32(); // sender
        wire.ReadUint32(); // sequence
        wire.ReadFloat();  // timestamp
        const uint32_t payloadSize = wire.ReadUint32();
        if (wire.HasError() || magic != 0x5350524B || payloadSize != wire.RemainingBytes())
        {
            ++tally.otherReplies;
            return;
        }
        if (type == Spark::Net::MessageType::ConnectAccepted)
        {
            ++tally.accepted;
            return;
        }
        wire.ReadString(); // human-readable reason
        const auto reason = static_cast<Spark::Net::ConnectRejectReason>(wire.ReadUint8());
        if (type != Spark::Net::MessageType::ConnectRejected || wire.HasError())
        {
            ++tally.otherReplies;
            return;
        }
        ++tally.rejections[reason];
    }

    /// Hostile traffic against a running server, from an endpoint that never joins. The server
    /// must refuse every datagram and count it (NetworkStats), answer each Connect with its typed
    /// rejection, and keep the round it is hosting undisturbed (Tests/TestFPSLANLoopback.cpp).
    /// Sends are paced so the server's 64 KiB socket buffer never overflows: every datagram sent
    /// must reach the server, or its counters could not be held against the sent totals.
    int RunIntruder(const Options& options)
    {
        using namespace std::chrono_literals;
        const std::unique_ptr<Spark::Net::ITransport> transport = MakeFPSLANIntruderTransport();
        Spark::Net::ITransport& socket = *transport;
        if (!socket.Initialize(0))
        {
            Emit("error reason=intruder-socket");
            return kExitNetworkFailure;
        }

        IntrusionTally tally;
        std::vector<uint8_t> reply(Spark::Net::MAX_UDP_WIRE_DATAGRAM_SIZE);
        const auto drainReplies = [&]
        {
            std::string from;
            uint16_t fromPort = 0;
            for (int received = 0; (received = socket.Receive(reply.data(), reply.size(), from, fromPort)) > 0;)
                TallyReply(reply.data(), static_cast<size_t>(received), tally);
        };
        const auto send = [&](const std::vector<uint8_t>& datagram, uint32_t& counter)
        {
            if (socket.Send(datagram.data(), datagram.size(), "127.0.0.1", options.port))
                ++counter;
            else
                ++tally.sendFailures;
        };
        const auto pause = [&](std::chrono::milliseconds duration)
        {
            std::this_thread::sleep_for(duration);
            drainReplies();
        };

        std::mt19937 random(0x5EED315u); // fixed seed: the same forged bytes every run
        const auto noise = [&](uint8_t frameKind, size_t size)
        {
            std::vector<uint8_t> datagram(size);
            for (uint8_t& byte : datagram)
                byte = static_cast<uint8_t>(random());
            datagram[0] = frameKind;
            return datagram;
        };

        // Unframed bytes: a pre-v2 message with no frame byte, and raw text.
        send(WireMessage(Spark::Net::MessageType::Heartbeat, {}), tally.refusedFrames);
        send({'h', 'e', 'l', 'l', 'o'}, tally.refusedFrames);
        // Frame kinds v2 does not define.
        for (const uint8_t kind : {uint8_t{0x00}, uint8_t{0x03}, uint8_t{0x7F}, uint8_t{0xFF}})
            send(noise(kind, 48), tally.refusedFrames);
        // Sealed frames from an endpoint that owns no SecureChannel: forged ciphertext of plausible
        // sizes, and headers cut short (a lone kind byte, a partial sequence number).
        for (const size_t size : {size_t{1}, size_t{4}, size_t{29}, size_t{64}, size_t{200}})
            send(noise(Spark::Net::NETWORK_FRAME_SEALED, size), tally.refusedFrames);
        pause(50ms);

        // The largest datagram UDP can carry, twice, each into a drained socket buffer.
        for (int copy = 0; copy < 2; ++copy)
        {
            send(noise(Spark::Net::NETWORK_FRAME_SEALED, Spark::Net::MAX_UDP_WIRE_DATAGRAM_SIZE), tally.refusedFrames);
            pause(100ms);
        }

        // Handshake frames the server must answer with a typed rejection, never a session.
        const uint32_t spnh = Spark::Net::NETWORK_HANDSHAKE_MAGIC;
        const uint16_t version = Spark::Net::NETWORK_PROTOCOL_VERSION;
        for (int copy = 0; copy < 2; ++copy)
        {
            send(HandshakeConnect(0x21474E57u, version), tally.noMagicConnects); // "WNG!", not "SPNH"
            send(HandshakeConnect(spnh, static_cast<uint16_t>(version + 97)), tally.futureConnects);
            send(HandshakeConnect(spnh, version), tally.malformedConnects); // 8 bytes, not a ClientHello
        }
        pause(50ms);

        // Bursts: back-to-back datagrams, spaced so the server drains each before the next.
        constexpr int kBursts = 4;
        constexpr int kBurstSize = 32;
        for (int burst = 0; burst < kBursts; ++burst)
        {
            for (int index = 0; index < kBurstSize; ++index)
                send(noise(0x04, 24), tally.refusedFrames);
            pause(250ms);
        }

        // Collect the remaining answers: one per Connect, or give up after two seconds.
        const uint32_t connects = tally.noMagicConnects + tally.futureConnects + tally.malformedConnects;
        const auto answered = [&]
        {
            uint32_t total = tally.accepted;
            for (const auto& [reason, count] : tally.rejections)
                total += count;
            return total;
        };
        const auto settle = std::chrono::steady_clock::now() + 2s;
        while (answered() < connects && std::chrono::steady_clock::now() < settle)
            pause(10ms);

        using Reason = Spark::Net::ConnectRejectReason;
        Emit("intruder sent-refused={} sent-no-magic={} sent-future-version={} sent-malformed-hello={} "
             "send-failures={} rejected-protocol-missing={} rejected-too-new={} rejected-malformed={} "
             "rejected-total={} accepted={} other-replies={}",
             tally.refusedFrames, tally.noMagicConnects, tally.futureConnects, tally.malformedConnects,
             tally.sendFailures, tally.rejections[Reason::ProtocolMissing], tally.rejections[Reason::ProtocolTooNew],
             tally.rejections[Reason::MalformedHandshake], answered() - tally.accepted, tally.accepted,
             tally.otherReplies);
        return kExitOk;
    }
} // namespace

int main(int argc, char** argv)
{
    const std::optional<Options> options = ParseOptions(argc, argv);
    if (!options)
    {
        std::fputs("usage: SparkFPSLANLoopbackPeer --role server [--timeout <s>]\n"
                   "       SparkFPSLANLoopbackPeer --role shooter|target --port <port> --host-id <id> "
                   "--server-key <hex> [--timeout <s>]\n"
                   "       SparkFPSLANLoopbackPeer --role intruder --port <port> [--timeout <s>]\n",
                   stderr);
        return kExitUsage;
    }
    if (options->role == Role::Intruder)
        return RunIntruder(*options);

    CommandChannel commands;
    return options->role == Role::Server ? RunServer(*options, commands) : RunClient(*options, commands);
}

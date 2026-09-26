/**
 * @file FPSLANLoopbackScenario.h
 * @brief MOD-315: the scripted spawn-move-kill-respawn-score round shared by the FPSLAN
 *        loopback peer (Tests/Fixtures/FPSLANLoopbackPeer.cpp) and its coordinator test
 *        (Tests/TestFPSLANLoopback.cpp).
 *
 * One server process and two client processes run the production SparkFPS::FPSMultiplayerSystem
 * over real loopback UDP. The shooter walks to kShooterPost, the target walks to kTargetPost,
 * the shooter fires until the server kills the target, the target respawns at a server-chosen
 * spawn point and walks to kTargetReturnPost. Every peer reports what it sees; the coordinator
 * requires all three views to agree with the server.
 *
 * Contract: header-only constants, no allocation, usable from any thread.
 */

#pragma once

#include <array>
#include <string_view>

namespace FPSLANScenario
{
    struct Point
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    /// The server's only spawn points. They sit on z = 0, well clear of the posts below, so the
    /// host player never stands in the shooter's line of fire.
    inline constexpr std::array<Point, 4> kSpawnPoints{
        Point{-20.0f, 1.0f, 0.0f},
        Point{-10.0f, 1.0f, 0.0f},
        Point{10.0f, 1.0f, 0.0f},
        Point{20.0f, 1.0f, 0.0f},
    };

    inline constexpr Point kShooterPost{0.0f, 1.0f, 30.0f};
    inline constexpr Point kTargetPost{0.0f, 1.0f, 50.0f};
    inline constexpr Point kTargetReturnPost{-8.0f, 1.0f, 45.0f};

    /// FPSMultiplayerSystem's planar move speed (m/s). Steering only uses it to shorten the last
    /// step onto a post; a different speed still converges, one overshoot at a time.
    inline constexpr float kMoveSpeed = 8.0f;
    /// One PlayerInput is one fixed 60 Hz simulation step on the client and on the server.
    inline constexpr float kInputStep = 1.0f / 60.0f;

    /// A peer counts itself on a post inside this planar distance (metres).
    inline constexpr float kArrivalTolerance = 0.02f;
    /// Largest disagreement (metres) between a client's view of a resting player and the server.
    inline constexpr float kConvergenceTolerance = 0.05f;

    /// Scoring rules the FPS server applies to one kill.
    inline constexpr int kKillScore = 100;
    inline constexpr float kFullHealth = 100.0f;

    /// Every protocol line a peer writes starts with this token; everything else is log output.
    inline constexpr std::string_view kLinePrefix = "FPSLAN ";

    /// Coordinator commands, one per stdin line. End of input also means quit.
    inline constexpr std::string_view kCommandReport = "report";
    inline constexpr std::string_view kCommandQuit = "quit";

    /// Exit codes of the peer process.
    inline constexpr int kExitOk = 0;
    inline constexpr int kExitUsage = 1;
    inline constexpr int kExitNetworkFailure = 2;
    inline constexpr int kExitScenarioFailure = 3;
    inline constexpr int kExitTimeout = 4;
} // namespace FPSLANScenario

/**
 * @file TestMOD380RacingCompleteRaceReal.cpp
 * @brief MOD-380: complete-race tests that drive the real SparkGameRacing flow.
 *
 * Every test builds the module's own roster with SetupRaceRoster() and advances
 * it with StepRaceFrame() + RacingVehicleSystem::FixedUpdate() -- the same calls
 * SparkGameRacingModule::OnUpdate/OnFixedUpdate make -- so lap validation,
 * AI driving, standings, and results are exercised end to end rather than by
 * calling race-manager bookkeeping directly.
 */

#include "TestFramework.h"

#ifdef SPARK_TEST_HAS_IMGUI

#include "../GameModules/SparkGameRacing/Source/AI/RacingAIDriver.h"
#include "../GameModules/SparkGameRacing/Source/Core/RacingRaceFlow.h"
#include "../GameModules/SparkGameRacing/Source/Race/RacingRaceManager.h"
#include "../GameModules/SparkGameRacing/Source/Track/RacingTrackSystem.h"
#include "../GameModules/SparkGameRacing/Source/Vehicle/RacingVehicleSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>

using namespace Racing;

namespace
{
    constexpr float kFrameDt = 1.0f / 60.0f;

    struct RaceRig
    {
        RacingVehicleSystem vehicles;
        RacingTrackSystem track;
        RacingRaceManager race;
        RacingAIDriver ai;

        explicit RaceRig(uint32_t trackIndex)
        {
            track.Initialize(nullptr);
            track.LoadDemoTrack(trackIndex);
            SetupRaceRoster(Sim(), nullptr);
        }

        ~RaceRig()
        {
            ai.Shutdown();
            race.Shutdown();
            vehicles.Shutdown();
            track.Shutdown();
        }

        RaceSimulation Sim() { return {vehicles, track, race, ai}; }

        /// One module frame: OnUpdate's race step followed by OnFixedUpdate's vehicle integration.
        void Frame(const PlayerDriveInput& input)
        {
            StepRaceFrame(Sim(), &input, kFrameDt);
            vehicles.FixedUpdate(kFrameDt);
        }

        /// Player autopilot that follows the authored racing line at full throttle.
        PlayerDriveInput Autopilot()
        {
            PlayerDriveInput input;
            input.throttle = 1.0f;
            input.steer = ComputeTrackSteer(*vehicles.GetPlayerVehicle(), track);
            return input;
        }

        /// Run until the race reaches Finished or the simulated time budget runs out.
        void RunToResults(float maxSeconds)
        {
            for (float t = 0.0f; t < maxSeconds && race.GetState() != RaceState::Finished; t += kFrameDt)
                Frame(Autopilot());
        }

        void DumpStandings() const { std::printf("%s", race.GetStandingsString().c_str()); }
    };

    /// Steering that points a vehicle at an arbitrary world position (used to script track cuts).
    float SteerToward(const VehicleInstance& vehicle, float targetX, float targetZ)
    {
        const float desired = std::atan2(targetX - vehicle.positionX, targetZ - vehicle.positionZ);
        const float error = std::atan2(std::sin(desired - vehicle.heading), std::cos(desired - vehicle.heading));
        return std::clamp(error * 1.5f, -1.0f, 1.0f);
    }

    float DistanceTo(const VehicleInstance& vehicle, float x, float z)
    {
        return std::hypot(vehicle.positionX - x, vehicle.positionZ - z);
    }

    /// Every racer must have finished a full valid race and the placings must follow finish order.
    void ExpectValidResults(const RaceRig& rig)
    {
        const RacingRaceManager& race = rig.race;
        EXPECT_TRUE(race.GetState() == RaceState::Finished);

        std::set<uint32_t> placings;
        for (const RacerState& racer : race.GetRacers())
        {
            EXPECT_TRUE(racer.finished);
            EXPECT_FALSE(racer.dnf);
            EXPECT_EQ(racer.currentLap, race.GetTotalLaps());
            EXPECT_EQ(racer.lapTimes.size(), static_cast<size_t>(race.GetTotalLaps()));
            EXPECT_GT(racer.bestLapTime, 0.0f);
            placings.insert(racer.position);
        }
        EXPECT_EQ(placings.size(), race.GetRacerCount());
        EXPECT_EQ(*placings.begin(), 1u);
        EXPECT_EQ(*placings.rbegin(), static_cast<uint32_t>(race.GetRacerCount()));

        for (const RacerState& a : race.GetRacers())
        {
            for (const RacerState& b : race.GetRacers())
            {
                if (a.position < b.position)
                    EXPECT_LE(a.finishTime, b.finishTime);
            }
        }
        EXPECT_STR_CONTAINS(race.GetResultsString(), "=== Race Results ===");
    }
} // namespace

TEST(RacingCompleteRace_CircuitPlayerAndAIFinishValidLaps)
{
    RaceRig rig(0);
    EXPECT_EQ(rig.race.GetRacerCount(), static_cast<size_t>(6));
    EXPECT_EQ(rig.ai.GetDriverCount(), static_cast<size_t>(5));
    EXPECT_TRUE(rig.race.GetState() == RaceState::Countdown);

    rig.RunToResults(290.0f);
    if (rig.race.GetState() != RaceState::Finished)
        rig.DumpStandings();
    ExpectValidResults(rig);

    // A finished race must not keep counting laps while cars sit on the finish line.
    for (int frame = 0; frame < 600; ++frame)
        rig.Frame(rig.Autopilot());
    for (const RacerState& racer : rig.race.GetRacers())
        EXPECT_EQ(racer.currentLap, rig.race.GetTotalLaps());
}

TEST(RacingCompleteRace_PointToPointPlayerAndAIReachAuthoredFinish)
{
    RaceRig rig(1);
    rig.RunToResults(290.0f);
    if (rig.race.GetState() != RaceState::Finished)
        rig.DumpStandings();
    ExpectValidResults(rig);
}

TEST(RacingCompleteRace_Figure8PlayerAndAIFinishValidLaps)
{
    RaceRig rig(2);
    rig.RunToResults(290.0f);
    if (rig.race.GetState() != RaceState::Finished)
        rig.DumpStandings();
    ExpectValidResults(rig);
}

TEST(RacingCompleteRace_CuttingTheInfieldDoesNotCompleteALap)
{
    RaceRig rig(0);
    const TrackData& circuit = rig.track.GetCurrentTrack();
    ASSERT_EQ(circuit.checkpoints.size(), static_cast<size_t>(4));
    const Checkpoint& finish = circuit.checkpoints[0];
    const Checkpoint& first = circuit.checkpoints[1];
    const uint32_t playerId = rig.vehicles.GetPlayerVehicle()->id;

    // Race to checkpoint 1 on the racing line, then cut straight across the infield to the finish line,
    // skipping checkpoints 2 and 3.
    float t = 0.0f;
    while (t < 60.0f && rig.race.GetRacer(playerId)->lastCheckpoint != first.index)
    {
        rig.Frame(rig.Autopilot());
        t += kFrameDt;
    }
    ASSERT_EQ(rig.race.GetRacer(playerId)->lastCheckpoint, first.index);

    bool reachedFinishLine = false;
    for (int frame = 0; frame < 60 * 30 && !reachedFinishLine; ++frame)
    {
        PlayerDriveInput cut;
        cut.throttle = 1.0f;
        cut.steer = SteerToward(*rig.vehicles.GetPlayerVehicle(), finish.x, finish.z);
        rig.Frame(cut);
        reachedFinishLine = DistanceTo(*rig.vehicles.GetPlayerVehicle(), finish.x, finish.z) < finish.radius * 0.5f;
    }
    ASSERT_TRUE(reachedFinishLine);
    EXPECT_EQ(rig.race.GetRacer(playerId)->currentLap, 0u);
    EXPECT_TRUE(rig.race.GetRacer(playerId)->lapTimes.empty());
    EXPECT_EQ(rig.race.GetRacer(playerId)->lastCheckpoint, first.index);
}

TEST(RacingCompleteRace_RestartAfterResultsStartsAFreshRace)
{
    RaceRig rig(0);
    rig.RunToResults(290.0f);
    ASSERT_TRUE(rig.race.GetState() == RaceState::Finished);

    SetupRaceRoster(rig.Sim(), nullptr);
    EXPECT_TRUE(rig.race.GetState() == RaceState::Countdown);
    EXPECT_EQ(rig.race.GetRacerCount(), static_cast<size_t>(6));
    EXPECT_EQ(rig.vehicles.GetVehicleCount(), static_cast<size_t>(6));
    EXPECT_EQ(rig.ai.GetDriverCount(), static_cast<size_t>(5));
    for (const RacerState& racer : rig.race.GetRacers())
    {
        EXPECT_EQ(racer.currentLap, 0u);
        EXPECT_FALSE(racer.finished);
        EXPECT_TRUE(racer.lapTimes.empty());
    }
    const TrackWaypoint& start = rig.track.GetCurrentTrack().waypoints[0];
    EXPECT_LT(DistanceTo(*rig.vehicles.GetPlayerVehicle(), start.x, start.z), 5.0f);
    EXPECT_STR_CONTAINS(rig.race.GetResultsString(), "not finished");

    // The restarted race is itself completable.
    rig.RunToResults(290.0f);
    ExpectValidResults(rig);
}

#endif // SPARK_TEST_HAS_IMGUI

/**
 * @file TestMOD380RacingCompleteRaceReal.cpp
 * @brief MOD-380: complete-race tests that drive the real SparkGameRacing flow on Jolt vehicles.
 *
 * Every test binds the module's systems to a real engine PhysicsSystem (RacingPhysicsTestWorld), builds the
 * module's own roster with SetupRaceRoster(), and advances it with StepRaceFrame() +
 * RacingVehicleSystem::FixedUpdate() -- the same calls SparkGameRacingModule::OnUpdate/OnFixedUpdate make, one
 * shared-world physics tick per 60 Hz frame -- so the cars, the track colliders they drive on, lap validation,
 * AI driving, standings, and results are exercised end to end.
 */

#include "TestFramework.h"

#if defined(SPARK_TEST_HAS_IMGUI) && defined(SPARK_TEST_HAS_PHYSICS)

#include "RacingPhysicsTestWorld.h"

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
        RacingPhysicsTestWorld world; // declared first: outlives every system below
        RacingVehicleSystem vehicles;
        RacingTrackSystem track;
        RacingRaceManager race;
        RacingAIDriver ai;
        bool ready = false;

        explicit RaceRig(uint32_t trackIndex)
        {
            if (!world.ready)
                return;
            track.Initialize(world.Context());
            track.LoadDemoTrack(trackIndex);
            ready = SetupRaceRoster(Sim(), world.Context());
        }

        ~RaceRig()
        {
            ai.Shutdown();
            race.Shutdown();
            vehicles.Shutdown();
            track.Shutdown();
        }

        RaceSimulation Sim() { return {vehicles, track, race, ai}; }

        /// One module frame: OnUpdate's race step followed by OnFixedUpdate's single shared physics tick.
        void Frame(const PlayerDriveInput& input)
        {
            StepRaceFrame(Sim(), &input, kFrameDt);
            vehicles.FixedUpdate(kFrameDt);
        }

        /// Player autopilot that follows the authored racing line, braking for corners like the AI does.
        PlayerDriveInput Autopilot()
        {
            const VehicleInstance& player = *vehicles.GetPlayerVehicle();
            PlayerDriveInput input;
            input.throttle = 1.0f;
            input.steer = ComputeTrackSteer(player, track);
            const float limit = ComputeCornerSpeedLimit(player, track);
            if (player.speed > limit)
            {
                input.throttle = 0.0f;
                input.brake = std::clamp((player.speed - limit) / 20.0f, 0.2f, 1.0f);
            }
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
    ASSERT_TRUE(rig.ready);
    EXPECT_EQ(rig.race.GetRacerCount(), static_cast<size_t>(6));
    EXPECT_EQ(rig.ai.GetDriverCount(), static_cast<size_t>(5));
    EXPECT_TRUE(rig.race.GetState() == RaceState::Countdown);

    rig.RunToResults(400.0f);
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
    ASSERT_TRUE(rig.ready);
    rig.RunToResults(400.0f);
    if (rig.race.GetState() != RaceState::Finished)
        rig.DumpStandings();
    ExpectValidResults(rig);
}

TEST(RacingCompleteRace_Figure8PlayerAndAIFinishValidLaps)
{
    RaceRig rig(2);
    ASSERT_TRUE(rig.ready);
    rig.RunToResults(400.0f);
    if (rig.race.GetState() != RaceState::Finished)
        rig.DumpStandings();
    ExpectValidResults(rig);
}

TEST(RacingCompleteRace_CuttingTheInfieldDoesNotCompleteALap)
{
    RaceRig rig(0);
    ASSERT_TRUE(rig.ready);
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
    for (int frame = 0; frame < 60 * 60 && !reachedFinishLine; ++frame)
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
    ASSERT_TRUE(rig.ready);
    rig.RunToResults(400.0f);
    ASSERT_TRUE(rig.race.GetState() == RaceState::Finished);

    ASSERT_TRUE(SetupRaceRoster(rig.Sim(), rig.world.Context()));
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
    rig.RunToResults(400.0f);
    ExpectValidResults(rig);
}

TEST(RacingCompleteRace_VehiclesAreJoltBodiesSteppedOnTheSharedTimestep)
{
    // No kinematic fallback: without the engine's live Jolt world the vehicle system refuses to start.
    RacingVehicleSystem noPhysics;
    EXPECT_FALSE(noPhysics.Initialize(nullptr));
    EXPECT_EQ(noPhysics.CreateVehicle("Orphan", VehicleType::Kart, false, {}), 0u);

    RaceRig rig(0);
    ASSERT_TRUE(rig.ready);
    EXPECT_GE(rig.track.GetColliderCount(), static_cast<size_t>(2)); // road surface(s) + run-off slab
    for (const VehicleInstance& vehicle : rig.vehicles.GetVehicles())
        EXPECT_TRUE(rig.vehicles.HasChassis(vehicle.id));

    // The countdown settles every car onto the road collider under the grid, one physics tick per frame.
    const uint64_t ticksBefore = rig.vehicles.GetStepCount();
    for (int frame = 0; frame < 120; ++frame)
        rig.Frame({});
    EXPECT_EQ(rig.vehicles.GetStepCount() - ticksBefore, static_cast<uint64_t>(120));
    EXPECT_NEAR(rig.world.physics->GetTimeStep(), kFrameDt, 1.0e-6f);
    for (const VehicleInstance& vehicle : rig.vehicles.GetVehicles())
    {
        EXPECT_NEAR(vehicle.positionY, 0.0f, 0.25f);
        EXPECT_LT(vehicle.speed, 1.0f);
    }

    // Racing: the player's throttle reaches the Jolt controller and the chassis drives down the straight.
    while (rig.race.GetState() != RaceState::Racing)
        rig.Frame({});
    const VehicleInstance start = *rig.vehicles.GetPlayerVehicle();
    PlayerDriveInput floorIt;
    floorIt.throttle = 1.0f;
    for (int frame = 0; frame < 240; ++frame)
        rig.Frame(floorIt);
    const VehicleInstance& player = *rig.vehicles.GetPlayerVehicle();
    EXPECT_GT(player.speed, 50.0f);
    EXPECT_GT(player.rpm, 1000.0f);
    EXPECT_GT(DistanceTo(player, start.positionX, start.positionZ), 25.0f);
    EXPECT_NEAR(player.positionY, 0.0f, 0.25f);
}

TEST(RacingCompleteRace_MountainPassRoadCarriesTheCarsUphill)
{
    RaceRig rig(1);
    ASSERT_TRUE(rig.ready);

    // The Mountain Pass climbs to 20 m. A car only gains that height by driving on the elevated road colliders;
    // the run-off slab under the layout stays at 0 m. The tolerance covers the short hops over the crests.
    float highestPlayerY = 0.0f;
    for (float t = 0.0f; t < 400.0f && rig.race.GetState() != RaceState::Finished; t += kFrameDt)
    {
        rig.Frame(rig.Autopilot());
        const VehicleInstance& player = *rig.vehicles.GetPlayerVehicle();
        highestPlayerY = std::max(highestPlayerY, player.positionY);
        if (rig.vehicles.HasChassis(player.id))
        {
            const TrackProjection projection =
                rig.track.ProjectOntoTrack(player.positionX, player.positionZ, player.heading);
            EXPECT_NEAR(player.positionY, rig.track.GetCenterlineHeight(projection), 3.0f);
        }
    }
    EXPECT_GT(highestPlayerY, 15.0f);
    ExpectValidResults(rig);
}

TEST(RacingCompleteRace_FinishedRacersLeaveThePhysicsWorld)
{
    RaceRig rig(0);
    ASSERT_TRUE(rig.ready);
    rig.RunToResults(400.0f);
    ASSERT_TRUE(rig.race.GetState() == RaceState::Finished);

    // StepRaceFrame parks every finisher: its chassis is gone and it holds its final pose.
    rig.Frame(rig.Autopilot());
    for (const VehicleInstance& vehicle : rig.vehicles.GetVehicles())
    {
        EXPECT_FALSE(rig.vehicles.HasChassis(vehicle.id));
        EXPECT_NEAR(vehicle.speed, 0.0f, 0.001f);
    }
    const VehicleInstance parked = *rig.vehicles.GetPlayerVehicle();
    for (int frame = 0; frame < 60; ++frame)
        rig.Frame(rig.Autopilot());
    EXPECT_NEAR(rig.vehicles.GetPlayerVehicle()->positionX, parked.positionX, 1.0e-4f);
    EXPECT_NEAR(rig.vehicles.GetPlayerVehicle()->positionZ, parked.positionZ, 1.0e-4f);
}

#endif // SPARK_TEST_HAS_IMGUI && SPARK_TEST_HAS_PHYSICS

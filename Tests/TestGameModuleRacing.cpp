/**
 * @file TestGameModuleRacing.cpp
 * @brief Tests for Racing game module systems: vehicle, track, and race manager
 *
 * All tests are behind SPARK_TEST_HAS_IMGUI because the Racing module .cpp
 * files include ImGui for debug UI rendering.
 */

#include "TestFramework.h"

#ifdef SPARK_TEST_HAS_IMGUI

#include "../GameModules/SparkGameRacing/Source/Vehicle/RacingVehicleSystem.h"
#include "../GameModules/SparkGameRacing/Source/Track/RacingTrackSystem.h"
#include "../GameModules/SparkGameRacing/Source/Race/RacingRaceManager.h"
#include "../GameModules/SparkGameRacing/Source/AI/RacingAIDriver.h"
#include "../GameModules/SparkGameRacing/Source/Camera/RacingCameraSystem.h"
#include "../GameModules/SparkGameRacing/Source/Core/RacingPersistence.h"
#include "../GameModules/SparkGameRacing/Source/Core/RacingRaceFlow.h"
#include "../GameModules/SparkGameRacing/Source/HUD/RacingHUDSystem.h"

#ifdef SPARK_TEST_HAS_PHYSICS
#include "RacingPhysicsTestWorld.h"
#endif

#include <cmath>

using namespace Racing;

namespace
{
    class RacingTestContext final : public Spark::IEngineContext
    {
      public:
        GraphicsEngine* GetGraphics() override { return nullptr; }
        const GraphicsEngine* GetGraphics() const override { return nullptr; }
        InputManager* GetInput() override { return nullptr; }
        const InputManager* GetInput() const override { return nullptr; }
        Timer* GetTimer() override { return nullptr; }
        const Timer* GetTimer() const override { return nullptr; }
        Spark::EventBus* GetEventBus() override { return nullptr; }
        const Spark::EventBus* GetEventBus() const override { return nullptr; }
        AudioEngine* GetAudio() override { return nullptr; }
        const AudioEngine* GetAudio() const override { return nullptr; }
        PhysicsSystem* GetPhysics() override { return nullptr; }
        const PhysicsSystem* GetPhysics() const override { return nullptr; }
        uint32_t GetEngineVersion() const override { return 0; }
        uint32_t GetSDKVersion() const override { return 0; }
    };
} // namespace

// ============================================================================
// RacingVehicleSystem
// ============================================================================

TEST(Racing_VehicleSystem_InitializeRequiresSharedPhysics)
{
    // Racing vehicles only exist as Jolt bodies: no context, or a context without physics, is refused.
    RacingVehicleSystem sys;
    EXPECT_FALSE(sys.Initialize(nullptr));
    RacingTestContext noPhysics;
    EXPECT_FALSE(sys.Initialize(&noPhysics));
    EXPECT_EQ(sys.CreateVehicle("Orphan", VehicleType::SportsCar, true, {}), 0u);
    EXPECT_EQ(sys.GetVehicleCount(), 0u);
#ifdef SPARK_TEST_HAS_PHYSICS
    RacingPhysicsTestWorld world;
    ASSERT_TRUE(world.ready);
    EXPECT_TRUE(sys.Initialize(world.Context()));
#endif
    sys.Shutdown();
}

#ifdef SPARK_TEST_HAS_PHYSICS
TEST(Racing_VehicleSystem_CreateAndGetVehicle)
{
    RacingPhysicsTestWorld world;
    ASSERT_TRUE(world.ready);
    RacingVehicleSystem sys;
    ASSERT_TRUE(sys.Initialize(world.Context()));

    uint32_t id = sys.CreateVehicle("Speedster", VehicleType::SportsCar, true, {12.0f, 0.0f, -4.0f, 0.5f});
    EXPECT_GT(id, 0u);

    const VehicleInstance* vehicle = sys.GetVehicle(id);
    ASSERT_TRUE(vehicle != nullptr);
    EXPECT_EQ(vehicle->name, std::string("Speedster"));
    EXPECT_TRUE(vehicle->isPlayer);
    EXPECT_TRUE(sys.HasChassis(id));
    EXPECT_NEAR(vehicle->positionX, 12.0f, 0.001f);
    EXPECT_NEAR(vehicle->heading, 0.5f, 0.001f);
    sys.Shutdown();
}
#endif

TEST(Racing_VehicleSystem_GetDefaultStats)
{
    VehicleStats stats = RacingVehicleSystem::GetDefaultStats(VehicleType::Formula);
    EXPECT_GT(stats.maxSpeed, 0.0f);
    EXPECT_GT(stats.handling, 0.0f);
    EXPECT_GT(stats.braking, 0.0f);
}

TEST(Racing_VehicleSystem_GetSurfaceGrip)
{
    float asphaltGrip = RacingVehicleSystem::GetSurfaceGrip(SurfaceType::Asphalt);
    float iceGrip = RacingVehicleSystem::GetSurfaceGrip(SurfaceType::Ice);

    EXPECT_GT(asphaltGrip, 0.0f);
    EXPECT_GT(iceGrip, 0.0f);
    EXPECT_GT(asphaltGrip, iceGrip); // Asphalt should grip better than ice
}

#ifdef SPARK_TEST_HAS_PHYSICS
TEST(Racing_VehicleSystem_VehicleCountAndList)
{
    RacingPhysicsTestWorld world;
    ASSERT_TRUE(world.ready);
    RacingVehicleSystem sys;
    ASSERT_TRUE(sys.Initialize(world.Context()));

    EXPECT_EQ(sys.GetVehicleCount(), 0u);

    sys.CreateVehicle("Car A", VehicleType::MuscleCar, false, {0.0f, 0.0f, 0.0f, 0.0f});
    sys.CreateVehicle("Car B", VehicleType::Kart, false, {6.0f, 0.0f, 0.0f, 0.0f});
    EXPECT_EQ(sys.GetVehicleCount(), 2u);

    std::string list = sys.GetVehicleListString();
    EXPECT_FALSE(list.empty());
    sys.Shutdown();
}

namespace
{
    struct FrameRateRun
    {
        float nitro = 0.0f;
        float speed = 0.0f;
        float positionX = 0.0f;
        float positionZ = 0.0f;
        bool ok = false;
    };

    // Drives one car for two seconds of game time on a fresh Jolt world, rendering frames at framesPerSecond and
    // stepping the vehicle system at the fixed 60 Hz physics rate from an accumulator (the module's frame loop).
    FrameRateRun DriveAtFrameRate(int framesPerSecond)
    {
        FrameRateRun run;
        RacingPhysicsTestWorld world;
        if (!world.ready)
            return run;
        RacingTrackSystem track;
        RacingVehicleSystem vehicles;
        track.Initialize(world.Context());
        if (!vehicles.Initialize(world.Context()))
            return run;

        const auto& start = track.GetWaypoint(0);
        const auto& next = track.GetWaypoint(1);
        const float heading = std::atan2(next.x - start.x, next.z - start.z);
        vehicles.CreateVehicle("Player", VehicleType::SportsCar, true, {start.x, start.y, start.z, heading});

        constexpr float kFixedStep = 1.0f / 60.0f;
        const float frameTime = 1.0f / static_cast<float>(framesPerSecond);
        float accumulator = 0.0f;
        for (int frame = 0; frame < 2 * framesPerSecond; ++frame)
        {
            vehicles.ApplyInput(0.75f, 0.0f, 0.0f, true, false, frameTime);
            vehicles.Update(frameTime);
            accumulator += frameTime;
            while (accumulator >= kFixedStep - 1.0e-6f)
            {
                vehicles.FixedUpdate(kFixedStep);
                accumulator -= kFixedStep;
            }
        }

        const VehicleInstance* player = vehicles.GetPlayerVehicle();
        if (player)
        {
            run.nitro = player->nitro;
            run.speed = player->speed;
            run.positionX = player->positionX;
            run.positionZ = player->positionZ;
            run.ok = vehicles.GetStepCount() == 120u;
        }
        vehicles.Shutdown();
        track.Shutdown();
        return run;
    }
} // namespace

TEST(Racing_VehicleSystem_InputIsFrameRateIndependent)
{
    // The same held input at 60 and 30 rendered frames per second yields the same car after two seconds: nitro
    // drains by elapsed time, and the latched command drives the same 120 fixed Jolt ticks either way.
    const FrameRateRun sixtyHz = DriveAtFrameRate(60);
    const FrameRateRun thirtyHz = DriveAtFrameRate(30);
    ASSERT_TRUE(sixtyHz.ok);
    ASSERT_TRUE(thirtyHz.ok);

    EXPECT_NEAR(sixtyHz.nitro, thirtyHz.nitro, 0.001f);
    EXPECT_GT(sixtyHz.speed, 20.0f); // the car really drove
    EXPECT_NEAR(sixtyHz.speed, thirtyHz.speed, 0.02f * sixtyHz.speed);
    EXPECT_NEAR(sixtyHz.positionX, thirtyHz.positionX, 0.25f);
    EXPECT_NEAR(sixtyHz.positionZ, thirtyHz.positionZ, 0.25f);
}

TEST(Racing_VehicleSystem_InputRejectsInvalidDeltaTime)
{
    RacingPhysicsTestWorld world;
    ASSERT_TRUE(world.ready);
    RacingVehicleSystem sys;
    ASSERT_TRUE(sys.Initialize(world.Context()));
    sys.CreateVehicle("Player", VehicleType::SportsCar, true, {});

    sys.ApplyInput(1.0f, 0.0f, 1.0f, true, true, 0.0f);
    EXPECT_EQ(sys.GetPlayerVehicle()->throttleInput, 0.0f);
    EXPECT_EQ(sys.GetPlayerVehicle()->steerAngle, 0.0f);
    EXPECT_EQ(sys.GetPlayerVehicle()->nitro, 1.0f);

    sys.Shutdown();
}
#endif

// ============================================================================
// RacingTrackSystem
// ============================================================================

TEST(Racing_TrackSystem_Initialize)
{
    RacingTrackSystem sys;
    EXPECT_TRUE(sys.Initialize(nullptr));
    sys.Shutdown();
}

TEST(Racing_TrackSystem_LoadDemoTrack)
{
    RacingTrackSystem sys;
    sys.Initialize(nullptr);

    sys.LoadDemoTrack(0);
    EXPECT_FALSE(sys.GetCurrentTrack().waypoints.empty());
    sys.Shutdown();
}

TEST(Racing_TrackSystem_GetTrackCount)
{
    RacingTrackSystem sys;
    sys.Initialize(nullptr);

    EXPECT_GT(sys.GetTrackCount(), 0u);
    sys.Shutdown();
}

TEST(Racing_TrackSystem_GetCheckpointCount)
{
    RacingTrackSystem sys;
    sys.Initialize(nullptr);

    sys.LoadDemoTrack(0);
    EXPECT_GT(sys.GetCheckpointCount(), 0u);
    sys.Shutdown();
}

TEST(Racing_TrackSystem_GetSurfaceAtAndListString)
{
    RacingTrackSystem sys;
    sys.Initialize(nullptr);

    sys.LoadDemoTrack(0);

    // Query surface at origin — should return a valid surface type
    SurfaceType surface = sys.GetSurfaceAt(0.0f, 0.0f);
    EXPECT_LT(static_cast<uint8_t>(surface), static_cast<uint8_t>(SurfaceType::Count));

    std::string list = sys.GetTrackListString();
    EXPECT_FALSE(list.empty());
    sys.Shutdown();
}

// ============================================================================
// RacingRaceManager
// ============================================================================

TEST(Racing_RaceManager_Initialize)
{
    RacingRaceManager mgr;
    EXPECT_TRUE(mgr.Initialize(nullptr));
    mgr.Shutdown();
}

TEST(Racing_RaceManager_RegisterRacer)
{
    RacingRaceManager mgr;
    mgr.Initialize(nullptr);

    mgr.RegisterRacer(1, "Player 1", true);
    mgr.RegisterRacer(2, "AI Racer", false);

    EXPECT_EQ(mgr.GetRacerCount(), 2u);
    mgr.Shutdown();
}

TEST(Racing_RaceManager_StartRace)
{
    RacingRaceManager mgr;
    mgr.Initialize(nullptr);

    mgr.RegisterRacer(1, "Player 1", true);
    mgr.StartRace(RaceMode::SingleRace, 3);

    EXPECT_TRUE(mgr.GetState() == RaceState::Countdown);
    EXPECT_EQ(mgr.GetTotalLaps(), 3u);
    mgr.Shutdown();
}

TEST(Racing_RaceManager_GetState)
{
    RacingRaceManager mgr;
    mgr.Initialize(nullptr);

    // Before starting, state should be Countdown (default)
    EXPECT_TRUE(mgr.GetState() == RaceState::Countdown);
    mgr.Shutdown();
}

TEST(Racing_RaceManager_StandingsString)
{
    RacingRaceManager mgr;
    mgr.Initialize(nullptr);

    mgr.RegisterRacer(1, "Alice", true);
    mgr.RegisterRacer(2, "Bob", false);
    mgr.RegisterRacer(3, "Charlie", false);

    EXPECT_EQ(mgr.GetRacerCount(), 3u);

    std::string standings = mgr.GetStandingsString();
    EXPECT_FALSE(standings.empty());
    mgr.Shutdown();
}

TEST(Racing_RaceFlow_CircuitRequiresOrderedCheckpoints)
{
    RacingTrackSystem track;
    RacingRaceManager race;
    track.Initialize(nullptr);
    race.Initialize(nullptr);
    track.LoadDemoTrack(0);
    race.RegisterRacer(1, "Player", true);
    race.StartRace(RaceMode::SingleRace, 1);

    const TrackData& circuit = track.GetCurrentTrack();
    EXPECT_GT(circuit.checkpoints.size(), static_cast<size_t>(2));
    EXPECT_FALSE(ProcessOrderedCheckpoint(race, circuit, 1, 2));
    EXPECT_EQ(race.GetRacer(1)->currentLap, 0u);

    for (uint32_t checkpoint = 1; checkpoint < circuit.checkpoints.size(); ++checkpoint)
        EXPECT_FALSE(ProcessOrderedCheckpoint(race, circuit, 1, checkpoint));
    EXPECT_TRUE(ProcessOrderedCheckpoint(race, circuit, 1, 0));
    EXPECT_EQ(race.GetRacer(1)->currentLap, 1u);
    EXPECT_TRUE(race.GetRacer(1)->finished);

    race.Shutdown();
    track.Shutdown();
}

TEST(Racing_RaceFlow_PointToPointUsesAuthoredFinish)
{
    RacingTrackSystem track;
    RacingRaceManager race;
    track.Initialize(nullptr);
    race.Initialize(nullptr);
    track.LoadDemoTrack(1);
    race.RegisterRacer(1, "Player", true);
    race.StartRace(RaceMode::SingleRace, 1);

    const TrackData& pointToPoint = track.GetCurrentTrack();
    EXPECT_TRUE(pointToPoint.checkpoints.back().isFinishLine);
    EXPECT_FALSE(ProcessOrderedCheckpoint(race, pointToPoint, 1, 1));
    EXPECT_TRUE(ProcessOrderedCheckpoint(race, pointToPoint, 1, 2));
    EXPECT_EQ(race.GetRacer(1)->currentLap, 1u);
    EXPECT_TRUE(race.GetRacer(1)->finished);

    race.Shutdown();
    track.Shutdown();
}

TEST(Racing_RaceManager_RefreshesCurrentFrameStandings)
{
    RacingRaceManager race;
    race.Initialize(nullptr);
    race.RegisterRacer(1, "Player", true);
    race.RegisterRacer(2, "AI", false);
    race.UpdateRacerDistance(1, 10.0f);
    race.UpdateRacerDistance(2, 20.0f);
    race.RefreshPositions();

    EXPECT_EQ(race.GetRacer(1)->position, 2u);
    EXPECT_EQ(race.GetRacer(2)->position, 1u);
    race.Shutdown();
}

#ifdef SPARK_TEST_HAS_PHYSICS
TEST(Racing_RaceFlow_AIDriverMovesAlongAuthoredTrack)
{
    RacingPhysicsTestWorld world;
    ASSERT_TRUE(world.ready);
    RacingTrackSystem track;
    RacingVehicleSystem vehicles;
    RacingAIDriver ai;
    track.Initialize(world.Context());
    ASSERT_TRUE(vehicles.Initialize(world.Context()));
    EXPECT_TRUE(ai.Initialize(world.Context()));

    const auto& start = track.GetWaypoint(0);
    const auto& next = track.GetWaypoint(1);
    const float heading = std::atan2(next.x - start.x, next.z - start.z);
    const uint32_t vehicleId =
        vehicles.CreateVehicle("AI", VehicleType::SportsCar, false, {start.x, start.y, start.z, heading});
    const VehicleInstance* vehicle = vehicles.GetVehicle(vehicleId);
    ASSERT_TRUE(vehicle != nullptr);

    AIDriverConfig config{};
    config.vehicleId = vehicleId;
    config.reactionTime = 0.0f;
    ai.AddDriver(config);
    ai.Update(0.1f);
    const AIDriverState* state = ai.GetDriverState(vehicleId);
    ASSERT_TRUE(state != nullptr);
    EXPECT_GT(state->throttle, 0.0f);

    // One second of the AI's command through the shared Jolt world moves the car along the track.
    for (int tick = 0; tick < 60; ++tick)
    {
        vehicles.ApplyInputToVehicle(vehicleId, state->throttle, state->brake, ComputeTrackSteer(*vehicle, track),
                                     state->useNitro, state->useDrift, 1.0f / 60.0f);
        vehicles.FixedUpdate(1.0f / 60.0f);
    }
    EXPECT_GT(std::hypot(vehicle->positionX - start.x, vehicle->positionZ - start.z), 1.0f);
    EXPECT_GT(vehicle->speed, 5.0f);

    ai.Shutdown();
    vehicles.Shutdown();
    track.Shutdown();
}
#endif

TEST(Racing_Presentation_CameraAndHUDSnapshotsStaySynchronized)
{
    RacingTestContext context;
    RacingCameraSystem camera;
    RacingHUDSystem hud;
    EXPECT_TRUE(camera.Initialize(&context));
    EXPECT_TRUE(hud.Initialize(&context));

    camera.SetTarget(10.0f, 2.0f, 20.0f, 0.5f, 120.0f);
    camera.Update(0.1f);
    EXPECT_NEAR(camera.GetState().targetX, 10.0f, 0.001f);
    EXPECT_NEAR(camera.GetState().targetZ, 20.0f, 0.001f);

    HUDData data;
    data.speed = 120.0f;
    data.position = 2;
    hud.SetHUDData(data);
    hud.SetMinimapEntries({{10.0f, 20.0f, 2, true}, {12.0f, 24.0f, 1, false}});
    EXPECT_NEAR(hud.GetHUDData().speed, 120.0f, 0.001f);
    EXPECT_EQ(hud.GetHUDData().position, 2u);
    EXPECT_EQ(hud.GetMinimapEntries().size(), static_cast<size_t>(2));
    EXPECT_TRUE(hud.GetMinimapEntries()[0].isPlayer);

    hud.Shutdown();
    camera.Shutdown();
}

#ifdef SPARK_TEST_HAS_PHYSICS
TEST(Racing_RaceFlow_TerminalRacersStopWhileAnotherRacerContinues)
{
    RacingPhysicsTestWorld world;
    ASSERT_TRUE(world.ready);
    RacingTrackSystem track;
    RacingRaceManager race;
    RacingVehicleSystem vehicles;
    track.Initialize(world.Context());
    race.Initialize(nullptr);
    ASSERT_TRUE(vehicles.Initialize(world.Context()));

    // Three cars side by side at the circuit start line, all facing +Z.
    const TrackWaypoint& start = track.GetWaypoint(0);
    const uint32_t playerId =
        vehicles.CreateVehicle("Finished Player", VehicleType::SportsCar, true, {start.x - 4.0f, 0.0f, start.z, 0.0f});
    const uint32_t dnfAIId =
        vehicles.CreateVehicle("DNF AI", VehicleType::SportsCar, false, {start.x, 0.0f, start.z, 0.0f});
    const uint32_t activeAIId =
        vehicles.CreateVehicle("Active AI", VehicleType::SportsCar, false, {start.x + 4.0f, 0.0f, start.z, 0.0f});
    race.RegisterRacer(playerId, "Finished Player", true);
    race.RegisterRacer(dnfAIId, "DNF AI", false);
    race.RegisterRacer(activeAIId, "Active AI", false);
    race.StartRace(RaceMode::SingleRace, 1);
    race.Update(3.1f);
    race.OnLapCompleted(playerId);
    race.MarkDNF(dnfAIId);

    EXPECT_TRUE(race.GetState() == RaceState::Racing);
    vehicles.ApplyInputToVehicle(playerId, 1.0f, 0.0f, 0.8f, true, false, 1.0f / 60.0f);
    vehicles.GetVehicle(dnfAIId)->driftState = DriftState::Drifting;

    const float playerStartZ = vehicles.GetVehicle(playerId)->positionZ;
    const float dnfStartZ = vehicles.GetVehicle(dnfAIId)->positionZ;
    const float activeStartZ = vehicles.GetVehicle(activeAIId)->positionZ;
    EXPECT_TRUE(StopTerminalRacer(race, vehicles, playerId));
    EXPECT_TRUE(StopTerminalRacer(race, vehicles, dnfAIId));
    EXPECT_FALSE(StopTerminalRacer(race, vehicles, activeAIId));
    EXPECT_FALSE(vehicles.HasChassis(playerId));
    EXPECT_FALSE(vehicles.HasChassis(dnfAIId));
    EXPECT_TRUE(vehicles.HasChassis(activeAIId));

    for (int tick = 0; tick < 60; ++tick)
    {
        vehicles.ApplyInputToVehicle(activeAIId, 1.0f, 0.0f, 0.0f, false, false, 1.0f / 60.0f);
        vehicles.FixedUpdate(1.0f / 60.0f);
    }

    EXPECT_NEAR(vehicles.GetVehicle(playerId)->positionZ, playerStartZ, 0.001f);
    EXPECT_NEAR(vehicles.GetVehicle(dnfAIId)->positionZ, dnfStartZ, 0.001f);
    EXPECT_GT(vehicles.GetVehicle(activeAIId)->positionZ, activeStartZ + 1.0f);
    EXPECT_NEAR(vehicles.GetVehicle(playerId)->speed, 0.0f, 0.001f);
    EXPECT_NEAR(vehicles.GetVehicle(dnfAIId)->speed, 0.0f, 0.001f);
    EXPECT_NEAR(vehicles.GetVehicle(playerId)->steerAngle, 0.0f, 0.001f);
    EXPECT_NEAR(vehicles.GetVehicle(playerId)->throttleInput, 0.0f, 0.001f);
    EXPECT_NEAR(vehicles.GetVehicle(playerId)->boostTimer, 0.0f, 0.001f);
    EXPECT_TRUE(vehicles.GetVehicle(dnfAIId)->driftState == DriftState::None);
    EXPECT_TRUE(vehicles.GetVehicle(playerId)->isActive);
    EXPECT_TRUE(vehicles.GetVehicle(dnfAIId)->isActive);

    vehicles.Shutdown();
    race.Shutdown();
    track.Shutdown();
}
#endif

TEST(Racing_RaceFlow_RepeatedDNFDoesNotReawardChampionshipPoints)
{
    RacingRaceManager race;
    ASSERT_TRUE(race.Initialize(nullptr));
    race.RegisterRacer(1, "Player", true);
    race.RegisterRacer(2, "Rival", false);
    race.StartRace(RaceMode::Championship, 1);
    race.Update(3.1f);

    race.OnLapCompleted(1);
    race.MarkDNF(2);
    ASSERT_TRUE(race.GetState() == RaceState::Finished);
    ASSERT_TRUE(race.GetRacer(1) != nullptr);
    ASSERT_TRUE(race.GetRacer(2) != nullptr);
    const uint32_t playerPoints = race.GetRacer(1)->championshipPoints;
    const uint32_t rivalPoints = race.GetRacer(2)->championshipPoints;

    race.MarkDNF(2);
    race.MarkDNF(999);
    EXPECT_EQ(race.GetRacer(1)->championshipPoints, playerPoints);
    EXPECT_EQ(race.GetRacer(2)->championshipPoints, rivalPoints);

    race.Shutdown();
}

#ifdef SPARK_TEST_HAS_PHYSICS
TEST(Racing_RaceFlow_TerminalRacersRetainNonDrivingControlEdges)
{
    RacingPhysicsTestWorld world;
    ASSERT_TRUE(world.ready);
    RacingTestContext context;
    RacingRaceManager race;
    RacingVehicleSystem vehicles;
    RacingCameraSystem camera;
    race.Initialize(nullptr);
    ASSERT_TRUE(vehicles.Initialize(world.Context()));
    EXPECT_TRUE(camera.Initialize(&context));

    const uint32_t playerId = vehicles.CreateVehicle("Finished Player", VehicleType::SportsCar, true, {});
    const uint32_t activeAIId =
        vehicles.CreateVehicle("Active AI", VehicleType::SportsCar, false, {6.0f, 0.0f, 0.0f, 0.0f});
    race.RegisterRacer(playerId, "Finished Player", true);
    race.RegisterRacer(activeAIId, "Active AI", false);
    race.StartRace(RaceMode::SingleRace, 1);
    race.Update(3.1f);
    race.OnLapCompleted(playerId);
    vehicles.ApplyInput(1.0f, 0.0f, 0.0f, false, false, 1.0f / 60.0f);

    bool restartHeld = false;
    bool cameraHeld = false;
    const RaceControlEdges cameraPress = PollRaceControlEdges(false, true, restartHeld, cameraHeld);
    EXPECT_FALSE(cameraPress.restartRequested);
    EXPECT_TRUE(cameraPress.cameraCycleRequested);
    camera.CycleMode();
    EXPECT_TRUE(camera.GetMode() == CameraMode::Cockpit);
    EXPECT_TRUE(StopTerminalRacer(race, vehicles, playerId));
    EXPECT_NEAR(vehicles.GetVehicle(playerId)->speed, 0.0f, 0.001f);

    const RaceControlEdges heldPress = PollRaceControlEdges(false, true, restartHeld, cameraHeld);
    EXPECT_FALSE(heldPress.restartRequested);
    EXPECT_FALSE(heldPress.cameraCycleRequested);
    const RaceControlEdges released = PollRaceControlEdges(false, false, restartHeld, cameraHeld);
    EXPECT_FALSE(released.restartRequested);
    EXPECT_FALSE(released.cameraCycleRequested);
    const RaceControlEdges restartPress = PollRaceControlEdges(true, false, restartHeld, cameraHeld);
    EXPECT_TRUE(restartPress.restartRequested);
    EXPECT_FALSE(restartPress.cameraCycleRequested);

    camera.Shutdown();
    vehicles.Shutdown();
    race.Shutdown();
}
#endif

// ============================================================================
// Racing persistence
// ============================================================================

TEST(Racing_Persistence_ValidatesPortableSlotNames)
{
    EXPECT_TRUE(RacingPersistence::IsValidSlotName("career-01_alpha"));
    EXPECT_TRUE(RacingPersistence::IsValidSlotName(std::string(64, 'r')));
    EXPECT_FALSE(RacingPersistence::IsValidSlotName(""));
    EXPECT_FALSE(RacingPersistence::IsValidSlotName(std::string(65, 'r')));
    EXPECT_FALSE(RacingPersistence::IsValidSlotName("../escape"));
    EXPECT_FALSE(RacingPersistence::IsValidSlotName("nested/slot"));
    EXPECT_FALSE(RacingPersistence::IsValidSlotName("slot name"));
}

#ifdef SPARK_TEST_HAS_PHYSICS
TEST(Racing_Persistence_RoundTripsAndAtomicallyRestoresRaceState)
{
    RacingPhysicsTestWorld world;
    ASSERT_TRUE(world.ready);
    RacingTrackSystem tracks;
    EXPECT_TRUE(tracks.Initialize(world.Context()));
    RacingVehicleSystem vehicles;
    ASSERT_TRUE(vehicles.Initialize(world.Context()));
    RacingRaceManager race;
    EXPECT_TRUE(race.Initialize(nullptr));
    RacingAIDriver ai;
    ASSERT_TRUE(ai.Initialize(nullptr));

    const uint32_t playerId =
        vehicles.CreateVehicle("Player \"One\"", VehicleType::SuperCar, true, {17.25f, 0.0f, -44.5f, 1.25f});
    VehicleInstance* player = vehicles.GetVehicle(playerId);
    ASSERT_TRUE(player != nullptr);
    // A car captured mid-race: the snapshot records its chassis speed and gameplay state.
    player->speed = 211.75f;
    player->nitro = 0.375f;
    player->damage = 12.5f;
    race.RegisterRacer(playerId, player->name, true);
    race.StartRace(RaceMode::Championship, 1);
    race.Update(3.5f);
    race.Update(42.25f);
    race.OnLapCompleted(playerId);
    ai.SetGlobalDifficulty(AIDifficulty::Hard);

    const RacingPersistenceSnapshot captured = RacingPersistence::Capture(tracks, vehicles, race, ai);
    EXPECT_NEAR(captured.race.countdownTimer, 0.0f, 0.001f);
    std::string error;
    const std::string encoded = RacingPersistence::Serialize(captured, error);
    EXPECT_EQ(error, std::string());
    ASSERT_FALSE(encoded.empty());

    RacingPersistenceSnapshot decoded;
    ASSERT_TRUE(RacingPersistence::Deserialize(encoded, decoded, error));
    EXPECT_EQ(RacingPersistence::Serialize(decoded), encoded);

    ASSERT_TRUE(vehicles.SetVehiclePose(playerId, {999.0f, 0.0f, 0.0f, 0.0f}));
    tracks.LoadDemoTrack(2);
    race.StartRace(RaceMode::TimeTrial, 5);
    ai.SetGlobalDifficulty(AIDifficulty::Easy);

    ASSERT_TRUE(RacingPersistence::Apply(decoded, tracks, vehicles, race, ai, error));
    EXPECT_EQ(tracks.GetCurrentTrack().id, captured.trackId);
    const VehicleInstance* restoredPlayer = vehicles.GetVehicle(playerId);
    ASSERT_TRUE(restoredPlayer != nullptr);
    EXPECT_NEAR(restoredPlayer->positionX, 17.25f, 0.001f);
    EXPECT_NEAR(restoredPlayer->speed, 211.75f, 0.001f);
    EXPECT_TRUE(vehicles.HasChassis(playerId));
    EXPECT_TRUE(race.GetState() == RaceState::Finished);
    EXPECT_TRUE(race.GetMode() == RaceMode::Championship);
    const RacerState* restoredRacer = race.GetRacer(playerId);
    ASSERT_TRUE(restoredRacer != nullptr);
    EXPECT_EQ(restoredRacer->lapTimes.size(), static_cast<size_t>(1));
    EXPECT_TRUE(ai.GetGlobalDifficulty() == AIDifficulty::Hard);

    // The rebuilt chassis carries the saved speed into the next physics tick.
    vehicles.FixedUpdate(1.0f / 60.0f);
    EXPECT_GT(vehicles.GetVehicle(playerId)->speed, 190.0f);

    vehicles.Shutdown();
    tracks.Shutdown();
}

TEST(Racing_Persistence_RejectsOutOfRangeVehicleSnapshotsWithoutTouchingTheRace)
{
    // Saved vehicle values go straight into Jolt on restore, so a hand-edited save that is merely finite must still
    // be refused: foreign stats (mass/torque/gearing), runaway speed or an off-world position.
    RacingPhysicsTestWorld world;
    ASSERT_TRUE(world.ready);
    RacingTrackSystem tracks;
    EXPECT_TRUE(tracks.Initialize(world.Context()));
    RacingVehicleSystem vehicles;
    ASSERT_TRUE(vehicles.Initialize(world.Context()));
    RacingRaceManager race;
    EXPECT_TRUE(race.Initialize(nullptr));
    RacingAIDriver ai;
    ASSERT_TRUE(ai.Initialize(nullptr));

    const uint32_t playerId = vehicles.CreateVehicle("Player", VehicleType::Formula, true, {5.0f, 0.0f, -3.0f, 0.25f});
    ASSERT_TRUE(playerId != 0u);
    race.RegisterRacer(playerId, "Player", true);
    race.StartRace(RaceMode::TimeTrial, 2);

    const RacingPersistenceSnapshot valid = RacingPersistence::Capture(tracks, vehicles, race, ai);
    std::string error;
    ASSERT_TRUE(RacingPersistence::Validate(valid, error));
    ASSERT_EQ(valid.vehicles.size(), static_cast<size_t>(1));

    std::vector<RacingPersistenceSnapshot> corrupt(5, valid);
    corrupt[0].vehicles[0].baseStats.weight = 3.0e38f; // overflows Jolt inertia
    corrupt[1].vehicles[0].baseStats = RacingVehicleSystem::GetDefaultStats(VehicleType::Kart); // wrong type
    corrupt[2].vehicles[0].speed = 1.0e6f;     // above the body's limit
    corrupt[3].vehicles[0].positionX = 1.0e7f; // off the world
    corrupt[4].vehicles[0].rpm = 1.0e9f;

    const RacingRaceSnapshot raceBefore = race.CaptureState();
    for (const RacingPersistenceSnapshot& snapshot : corrupt)
    {
        EXPECT_FALSE(RacingVehicleSystem::ValidateSnapshot(snapshot.vehicles));
        EXPECT_FALSE(RacingPersistence::Validate(snapshot, error));
        EXPECT_FALSE(error.empty());
        EXPECT_FALSE(RacingPersistence::Apply(snapshot, tracks, vehicles, race, ai, error));
        EXPECT_FALSE(vehicles.RestoreState(snapshot.vehicles));

        // The running race keeps its car, chassis, stats and pose.
        const VehicleInstance* player = vehicles.GetVehicle(playerId);
        ASSERT_TRUE(player != nullptr);
        EXPECT_TRUE(vehicles.HasChassis(playerId));
        EXPECT_EQ(player->baseStats.weight, RacingVehicleSystem::GetDefaultStats(VehicleType::Formula).weight);
        EXPECT_NEAR(player->positionX, 5.0f, 0.001f);
        EXPECT_EQ(vehicles.GetVehicleCount(), static_cast<size_t>(1));
        EXPECT_TRUE(race.GetMode() == RaceMode::TimeTrial);
        EXPECT_EQ(race.CaptureState().racers.size(), raceBefore.racers.size());
    }

    // The untouched capture still restores.
    EXPECT_TRUE(RacingPersistence::Apply(valid, tracks, vehicles, race, ai, error));
    EXPECT_TRUE(vehicles.HasChassis(playerId));

    ai.Shutdown();
    vehicles.Shutdown();
    tracks.Shutdown();
}
#endif

TEST(Racing_Persistence_RejectsCorruptionWithoutReplacingOutput)
{
    RacingPersistenceSnapshot output;
    output.trackId = 77;
    std::string error;

    EXPECT_FALSE(RacingPersistence::Deserialize("SPARK_RACING_STATE_V1\nTRACK 1\n", output, error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(output.trackId, static_cast<uint32_t>(77));
}

#endif // SPARK_TEST_HAS_IMGUI

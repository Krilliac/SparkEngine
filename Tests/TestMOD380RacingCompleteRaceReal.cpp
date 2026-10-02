/**
 * @file TestMOD380RacingCompleteRaceReal.cpp
 * @brief MOD-380: complete-race tests that drive the real SparkGameRacing flow on Jolt vehicles.
 *
 * Every test binds the module's systems to a real engine PhysicsSystem (RacingPhysicsTestWorld), builds the
 * module's own roster with SetupRaceRoster(), and advances it with StepRaceFrame() +
 * RacingVehicleSystem::FixedUpdate() -- the same calls SparkGameRacingModule::OnUpdate/OnFixedUpdate make, one
 * shared-world physics tick per 60 Hz frame -- so the cars, the track colliders they drive on, the barrier walls
 * that keep them on the circuit, lap validation through the Jolt checkpoint sensor gates, AI driving, standings,
 * and results are exercised end to end. The RacingCompleteRace_*Save* tests carry a finished race's results and best
 * laps through RacingEngineSystems::SaveRaceData/LoadRaceData on the real SaveSystem into a restarted module.
 */

#include "TestFramework.h"

#if defined(SPARK_TEST_HAS_IMGUI) && defined(SPARK_TEST_HAS_PHYSICS)

#include "RacingPhysicsTestWorld.h"

#include "../GameModules/SparkGameRacing/Source/AI/RacingAIDriver.h"
#include "../GameModules/SparkGameRacing/Source/Core/RacingEngineSystems.h"
#include "../GameModules/SparkGameRacing/Source/Core/RacingPersistence.h"
#include "../GameModules/SparkGameRacing/Source/Core/RacingRaceFlow.h"
#include "../GameModules/SparkGameRacing/Source/Race/RacingRaceManager.h"
#include "../GameModules/SparkGameRacing/Source/Track/RacingTrackSystem.h"
#include "../GameModules/SparkGameRacing/Source/Vehicle/RacingVehicleSystem.h"
#include "Engine/ECS/Components.h"
#include "Engine/SaveSystem/SaveSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

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

        /// The module's race_autopilot: follows the authored racing line, braking for corners like the AI does.
        PlayerDriveInput Autopilot() { return ComputePlayerAutopilotInput(*vehicles.GetPlayerVehicle(), track); }

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
        reachedFinishLine = DistanceTo(*rig.vehicles.GetPlayerVehicle(), finish.x, finish.z) < 8.0f;
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

TEST(RacingCompleteRace_BarrierStopsCarLeavingTrack)
{
    RaceRig rig(0);
    ASSERT_TRUE(rig.ready);
    EXPECT_GT(rig.track.GetBarrierCount(), static_cast<size_t>(0));
    const float roadHalfWidth = rig.track.GetCurrentTrack().waypoints[0].width;
    const float barrierLine = roadHalfWidth + RacingTrackSystem::kBarrierClearance;
    const uint32_t playerId = rig.vehicles.GetPlayerVehicle()->id;

    // The player's car runs alone. With the AI field on the grid, the car starting behind it follows the bend
    // while the player holds straight, catches its rear quarter and spins it out -- a racing incident whose
    // outcome turns on contact-solver and libm rounding, so it differs between compilers. This test is about
    // the barrier, so the other chassis leave the physics world before the start.
    std::vector<uint32_t> aiIds;
    for (const VehicleInstance& vehicle : rig.vehicles.GetVehicles())
    {
        if (vehicle.id != playerId)
            aiIds.push_back(vehicle.id);
    }
    for (const uint32_t id : aiIds)
    {
        rig.vehicles.NeutralizeVehicle(id);
        EXPECT_FALSE(rig.vehicles.HasChassis(id));
    }

    // Full throttle with the wheel held straight: the circuit bends away under the car, so it runs wide off the
    // outside of the bend. The outside barrier must catch it -- its center never gets further off the centerline
    // than the barrier's inner face plus the chassis half-width and a little contact slop.
    while (rig.race.GetState() != RaceState::Racing)
        rig.Frame({});
    PlayerDriveInput straight;
    straight.throttle = 1.0f;
    float widest = 0.0f;
    float speedRunningWide = -1.0f; // km/h as the car crosses the middle of the road half heading for the edge
    for (int frame = 0; frame < 60 * 12; ++frame)
    {
        rig.Frame(straight);
        const VehicleInstance& player = *rig.vehicles.GetVehicle(playerId);
        const float lateral = rig.track.ProjectOntoTrack(player.positionX, player.positionZ).lateralDistance;
        widest = std::max(widest, lateral);
        if (speedRunningWide < 0.0f && lateral > roadHalfWidth * 0.5f)
            speedRunningWide = player.speed;
        EXPECT_LT(lateral, barrierLine + 1.5f);
    }
    EXPECT_GT(widest, roadHalfWidth); // it did leave the road and reach the barrier
    // It crosses the half-way line about 56 m from the grid. 40 km/h there needs only ~1.1 m/s^2 on average, far
    // below the SportsCar's 7 m/s^2 rated launch even with the 1-2 shift, so an unobstructed car clears it with
    // wide margin (about 67 km/h) and only a car that was blocked or spun falls short.
    EXPECT_GT(speedRunningWide, 40.0f);
    std::printf("barrier: ran wide at %.1f km/h, widest %.2f m off the centerline (barrier face at %.2f m)\n",
                speedRunningWide, widest, barrierLine);
}

TEST(RacingCompleteRace_CheckpointSensorOrderStillRejectsInfieldCut)
{
    RaceRig rig(0);
    ASSERT_TRUE(rig.ready);
    const TrackData& circuit = rig.track.GetCurrentTrack();
    ASSERT_EQ(circuit.checkpoints.size(), static_cast<size_t>(4));
    EXPECT_EQ(rig.track.GetCheckpointGateCount(), circuit.checkpoints.size());
    const Checkpoint& finish = circuit.checkpoints[0];
    const Checkpoint& first = circuit.checkpoints[1];
    const uint32_t playerId = rig.vehicles.GetPlayerVehicle()->id;

    // Checkpoint 1 sits on the top of the ellipse, where the racing line runs toward -X. The crossing registers
    // when the chassis reaches the gate's sensor box (kGateDepth deep), not anywhere inside a wide trigger radius.
    float t = 0.0f;
    while (t < 60.0f && rig.race.GetRacer(playerId)->lastCheckpoint != first.index)
    {
        rig.Frame(rig.Autopilot());
        t += kFrameDt;
    }
    ASSERT_EQ(rig.race.GetRacer(playerId)->lastCheckpoint, first.index);
    const float approach = -(rig.vehicles.GetPlayerVehicle()->positionX - first.x); // + once past the gate plane
    EXPECT_GT(approach, -(RacingTrackSystem::kGateDepth * 0.5f + 2.1f + 2.0f));
    EXPECT_LT(approach, 3.0f);

    // Cut straight across the infield and through the finish gate from the inside, skipping checkpoints 2 and 3.
    bool throughFinishGate = false;
    for (int frame = 0; frame < 60 * 60 && !throughFinishGate; ++frame)
    {
        PlayerDriveInput cut;
        cut.throttle = 1.0f;
        cut.steer = SteerToward(*rig.vehicles.GetPlayerVehicle(), finish.x, finish.z);
        rig.Frame(cut);
        throughFinishGate = DistanceTo(*rig.vehicles.GetPlayerVehicle(), finish.x, finish.z) < 3.0f;
    }
    ASSERT_TRUE(throughFinishGate);
    for (int frame = 0; frame < 30; ++frame)
        rig.Frame(rig.Autopilot());
    EXPECT_EQ(rig.race.GetRacer(playerId)->currentLap, 0u);
    EXPECT_EQ(rig.race.GetRacer(playerId)->lastCheckpoint, first.index);

    // Driving on around the circuit passes gates 1 (again, ignored), 2, and 3 in order, and the next pass through
    // the finish gate completes exactly one lap.
    for (t = 0.0f; t < 120.0f && rig.race.GetRacer(playerId)->currentLap == 0u; t += kFrameDt)
        rig.Frame(rig.Autopilot());
    EXPECT_EQ(rig.race.GetRacer(playerId)->currentLap, 1u);
    EXPECT_EQ(rig.race.GetRacer(playerId)->lapTimes.size(), static_cast<size_t>(1));
    EXPECT_EQ(rig.race.GetRacer(playerId)->lastCheckpoint, finish.index);
}

namespace
{
    /// Engine context the module's save bridge sees: the rig's shared physics world, a World, and the SaveSystem.
    class RacingSaveContext final : public Spark::IEngineContext
    {
      public:
        RacingSaveContext(PhysicsSystem* physics, Spark::SaveSystem* saveSystem, ::World* world)
            : m_physics(physics), m_saveSystem(saveSystem), m_world(world)
        {
        }

        GraphicsEngine* GetGraphics() override { return nullptr; }
        const GraphicsEngine* GetGraphics() const override { return nullptr; }
        InputManager* GetInput() override { return nullptr; }
        const InputManager* GetInput() const override { return nullptr; }
        Timer* GetTimer() override { return nullptr; }
        const Timer* GetTimer() const override { return nullptr; }
        Spark::EventBus* GetEventBus() override { return nullptr; }
        const Spark::EventBus* GetEventBus() const override { return nullptr; }
        ::AudioEngine* GetAudio() override { return nullptr; }
        const ::AudioEngine* GetAudio() const override { return nullptr; }
        PhysicsSystem* GetPhysics() override { return m_physics; }
        const PhysicsSystem* GetPhysics() const override { return m_physics; }
        ::World* GetWorld() override { return m_world; }
        const ::World* GetWorld() const override { return m_world; }
        Spark::SaveSystem* GetSaveSystem() override { return m_saveSystem; }
        const Spark::SaveSystem* GetSaveSystem() const override { return m_saveSystem; }
        uint32_t GetEngineVersion() const override { return 0; }
        uint32_t GetSDKVersion() const override { return 0; }

      private:
        PhysicsSystem* m_physics = nullptr;
        Spark::SaveSystem* m_saveSystem = nullptr;
        ::World* m_world = nullptr;
    };

    /**
     * The module's save bridge on the real SaveSystem singleton, writing into an isolated directory that outlives
     * every rig bound to it. Bind() models a module (re)start: a fresh World, a fresh RacingEngineSystems wired to
     * the rig exactly as SparkGameRacingModule::OnLoad does, and the SaveSystem re-initialized onto the directory
     * (RacingEngineSystems::Initialize points it at "Saves/Racing"). The original directory is restored on
     * destruction. Declare the bridge before the rigs it binds.
     */
    class RacingSaveBridge
    {
      public:
        explicit RacingSaveBridge(const char* name)
            : m_saveSystem(Spark::SaveSystem::GetInstance()), m_previousDirectory(m_saveSystem.GetSaveDirectory()),
              m_directory(std::filesystem::temp_directory_path() / (std::string("spark_mod380_") + name))
        {
            std::error_code error;
            std::filesystem::remove_all(m_directory, error);
        }

        ~RacingSaveBridge()
        {
            Unbind();
            m_saveSystem.SetSaveDirectory(m_previousDirectory);
            std::error_code error;
            std::filesystem::remove_all(m_directory, error);
        }

        RacingSaveBridge(const RacingSaveBridge&) = delete;
        RacingSaveBridge& operator=(const RacingSaveBridge&) = delete;

        bool Bind(RaceRig& rig)
        {
            Unbind();
            m_world = std::make_unique<::World>();
            m_world->AddComponent<Transform>(m_world->CreateEntity("mod380-race-world"));
            m_context = std::make_unique<RacingSaveContext>(rig.world.physics.get(), &m_saveSystem, m_world.get());
            m_engine = std::make_unique<RacingEngineSystems>();
            const bool bound = m_engine->Initialize(m_context.get(), &rig.vehicles, &rig.track, &rig.race, &rig.ai);
            m_saveSystem.SetFileCache(nullptr);
            return m_saveSystem.Initialize(m_directory.string()) && bound;
        }

        void Unbind()
        {
            if (m_engine)
                m_engine->Shutdown();
            m_engine.reset();
            m_context.reset();
            m_world.reset();
        }

        RacingEngineSystems& Engine() { return *m_engine; }
        ::World& World() { return *m_world; }
        Spark::SaveSystem& Save() { return m_saveSystem; }
        const std::filesystem::path& Directory() const { return m_directory; }

        /// Write a slot whose only racing state is @p payload, as a damaged or edited save would carry it.
        bool WriteRawSlot(const std::string& slot, const std::string& payload)
        {
            Spark::SaveMetadata meta;
            meta.saveName = slot;
            meta.sceneName = "RacingTampered";
            return m_saveSystem.Save(
                slot, *m_world, meta,
                std::unordered_map<std::string, std::string>{{std::string(RacingPersistence::StateKey), payload}});
        }

      private:
        Spark::SaveSystem& m_saveSystem;
        std::string m_previousDirectory;
        std::filesystem::path m_directory;
        std::unique_ptr<::World> m_world;
        std::unique_ptr<RacingSaveContext> m_context;
        std::unique_ptr<RacingEngineSystems> m_engine;
    };

    bool StartsWith(const std::string& text, const std::string& prefix)
    {
        return text.rfind(prefix, 0) == 0;
    }

    /**
     * Rewrite one whitespace-separated field of vehicle @p vehicleId's "R" record in a SPARK_RACING_STATE_V1
     * payload (field 0 is the "R" tag; racer names in the demo roster carry no spaces).
     */
    std::string EditRacerField(const std::string& payload, uint32_t vehicleId, size_t field, const std::string& value)
    {
        std::istringstream lines(payload);
        std::string out;
        std::string line;
        const std::string recordPrefix = "R " + std::to_string(vehicleId) + " ";
        while (std::getline(lines, line))
        {
            if (StartsWith(line, recordPrefix))
            {
                std::istringstream tokens(line);
                std::vector<std::string> fields;
                for (std::string token; tokens >> token;)
                    fields.push_back(token);
                if (field < fields.size())
                    fields[field] = value;
                line.clear();
                for (size_t i = 0; i < fields.size(); ++i)
                    line += (i == 0 ? "" : " ") + fields[i];
            }
            out += line + '\n';
        }
        return out;
    }

    // Field indices of the "R" record (see RacingPersistence::Serialize).
    constexpr size_t kRacerCurrentLapField = 4;
    constexpr size_t kRacerPositionField = 7;
    constexpr size_t kRacerBestLapField = 10;

    std::string FloatText(float value)
    {
        std::ostringstream out;
        out.precision(9);
        out << value;
        return out.str();
    }

    /// Run a rig to results and let one more frame park every finisher, as the module does on its next update.
    void FinishAndPark(RaceRig& rig)
    {
        rig.RunToResults(400.0f);
        rig.Frame(rig.Autopilot());
    }
} // namespace

// A finished circuit race is saved through the module's race_save path, the module and its physics world are torn
// down, and a restarted module on another track loads the slot: every placing, finish time, lap time, and best lap
// comes back bit-identical, stays fixed while the loaded results are on screen, and the player can restart from
// the loaded results into a fresh race that is itself completable.
TEST(RacingCompleteRace_ResultsAndBestLapsSurviveSaveSystemRestart)
{
    RacingSaveBridge bridge("results");
    auto source = std::make_unique<RaceRig>(0);
    ASSERT_TRUE(source->ready);
    ASSERT_TRUE(bridge.Bind(*source));
    FinishAndPark(*source);
    ExpectValidResults(*source);

    const RacingRaceSnapshot expected = source->race.CaptureState();
    const std::string expectedResults = source->race.GetResultsString();
    const float expectedPlayerBestLap = source->race.GetPlayerBestLap();
    const uint32_t expectedTrackId = source->track.GetCurrentTrack().id;
    const std::string expectedTrackName = source->track.GetCurrentTrack().name;
    ASSERT_TRUE(expectedPlayerBestLap > 0.0f);

    ASSERT_EQ(bridge.Engine().SaveRaceData("mod380_results"), std::string("Race data saved to slot: mod380_results"));
    bridge.Unbind();
    source.reset();

    // Restart: a new module on a different track with its own countdown roster, bound to a fresh World.
    auto restarted = std::make_unique<RaceRig>(2);
    ASSERT_TRUE(restarted->ready);
    ASSERT_TRUE(bridge.Bind(*restarted));
    ASSERT_NE(restarted->track.GetCurrentTrack().id, expectedTrackId);
    ASSERT_TRUE(restarted->race.GetState() == RaceState::Countdown);

    ASSERT_TRUE(bridge.Save().SaveExists("mod380_results"));
    Spark::SaveMetadata meta;
    ASSERT_TRUE(bridge.Save().GetSaveMetadata("mod380_results", meta));
    EXPECT_EQ(meta.sceneName, expectedTrackName);

    ASSERT_EQ(bridge.Engine().LoadRaceData("mod380_results"),
              std::string("Race data loaded from slot: mod380_results"));
    EXPECT_EQ(restarted->track.GetCurrentTrack().id, expectedTrackId);
    EXPECT_TRUE(restarted->race.GetState() == RaceState::Finished);
    EXPECT_EQ(restarted->race.GetTotalLaps(), expected.totalLaps);
    EXPECT_EQ(restarted->race.GetRaceTime(), expected.raceTime);
    EXPECT_EQ(restarted->race.GetPlayerBestLap(), expectedPlayerBestLap);
    EXPECT_EQ(restarted->race.GetResultsString(), expectedResults);

    const auto expectRacersMatchSavedResults = [&]()
    {
        ASSERT_EQ(restarted->race.GetRacerCount(), expected.racers.size());
        for (const RacerState& saved : expected.racers)
        {
            const RacerState* loaded = restarted->race.GetRacer(saved.vehicleId);
            ASSERT_TRUE(loaded != nullptr);
            EXPECT_EQ(loaded->name, saved.name);
            EXPECT_EQ(loaded->isPlayer, saved.isPlayer);
            EXPECT_EQ(loaded->position, saved.position);
            EXPECT_TRUE(loaded->finished);
            EXPECT_FALSE(loaded->dnf);
            EXPECT_EQ(loaded->currentLap, saved.currentLap);
            EXPECT_EQ(loaded->finishTime, saved.finishTime);
            EXPECT_EQ(loaded->totalTime, saved.totalTime);
            EXPECT_EQ(loaded->bestLapTime, saved.bestLapTime);
            EXPECT_TRUE(loaded->lapTimes == saved.lapTimes);
        }
    };
    expectRacersMatchSavedResults();

    // The loaded results hold while the restarted module keeps running its frames.
    for (int frame = 0; frame < 300; ++frame)
        restarted->Frame(restarted->Autopilot());
    EXPECT_TRUE(restarted->race.GetState() == RaceState::Finished);
    expectRacersMatchSavedResults();
    EXPECT_EQ(restarted->race.GetResultsString(), expectedResults);

    // Restart from the loaded results: a fresh race on the saved track that the whole field completes.
    ASSERT_TRUE(SetupRaceRoster(restarted->Sim(), restarted->world.Context()));
    EXPECT_TRUE(restarted->race.GetState() == RaceState::Countdown);
    for (const RacerState& racer : restarted->race.GetRacers())
    {
        EXPECT_EQ(racer.currentLap, 0u);
        EXPECT_TRUE(racer.lapTimes.empty());
        EXPECT_EQ(racer.bestLapTime, -1.0f);
    }
    restarted->RunToResults(400.0f);
    ExpectValidResults(*restarted);
}

// Edited results are refused before the SaveSystem restores the World: a best lap faster than any lap driven, a
// duplicated placing, and a finish without the final lap each fail race_load with the live race and World intact.
TEST(RacingCompleteRace_SaveRejectsEditedResultsBeforeWorldRestore)
{
    RacingSaveBridge bridge("tampered");
    RaceRig rig(0);
    ASSERT_TRUE(rig.ready);
    ASSERT_TRUE(bridge.Bind(rig));
    FinishAndPark(rig);
    ExpectValidResults(rig);

    const RacingPersistenceSnapshot finished = RacingPersistence::Capture(rig.track, rig.vehicles, rig.race, rig.ai);
    std::string error;
    const std::string honest = RacingPersistence::Serialize(finished, error);
    ASSERT_FALSE(honest.empty());

    const RacerState* player = nullptr;
    const RacerState* rival = nullptr;
    for (const RacerState& racer : finished.race.racers)
    {
        if (racer.isPlayer)
            player = &racer;
        else if (!rival)
            rival = &racer;
    }
    ASSERT_TRUE(player != nullptr && rival != nullptr);

    const std::vector<std::pair<std::string, std::string>> edits = {
        {"faster_best_lap",
         EditRacerField(honest, player->vehicleId, kRacerBestLapField, FloatText(player->bestLapTime - 5.0f))},
        {"duplicate_placing",
         EditRacerField(honest, player->vehicleId, kRacerPositionField, std::to_string(rival->position))},
        {"finish_without_final_lap",
         EditRacerField(honest, player->vehicleId, kRacerCurrentLapField, std::to_string(player->currentLap - 1))},
    };
    RacingPersistenceSnapshot decoded;
    ASSERT_TRUE(RacingPersistence::Deserialize(honest, decoded, error));
    ASSERT_TRUE(bridge.WriteRawSlot("honest", honest));
    for (const auto& [slot, payload] : edits)
    {
        ASSERT_NE(payload, honest);
        EXPECT_FALSE(RacingPersistence::Deserialize(payload, decoded, error));
        ASSERT_TRUE(bridge.WriteRawSlot(slot, payload));
    }

    // A live race is running when the edited slots are loaded; neither it nor the World may change.
    ASSERT_TRUE(SetupRaceRoster(rig.Sim(), rig.world.Context()));
    for (int frame = 0; frame < 30; ++frame)
        rig.Frame({});
    bridge.World().CreateEntity("mod380-after-save");
    const size_t liveEntities = bridge.World().GetEntityCount();
    const RacingRaceSnapshot live = rig.race.CaptureState();
    for (const auto& [slot, payload] : edits)
    {
        const std::string result = bridge.Engine().LoadRaceData(slot);
        EXPECT_STR_CONTAINS(result, "Invalid racing state in slot '" + slot + "'");
        EXPECT_EQ(bridge.World().GetEntityCount(), liveEntities);
        EXPECT_TRUE(rig.race.GetState() == live.state);
        EXPECT_EQ(rig.race.GetCountdownTimer(), live.countdownTimer);
        EXPECT_EQ(rig.race.GetRacerCount(), live.racers.size());
        for (const RacerState& racer : rig.race.GetRacers())
        {
            EXPECT_EQ(racer.currentLap, 0u);
            EXPECT_EQ(racer.bestLapTime, -1.0f);
        }
    }

    // The untouched slot still loads the saved results.
    EXPECT_EQ(bridge.Engine().LoadRaceData("honest"), std::string("Race data loaded from slot: honest"));
    EXPECT_TRUE(rig.race.GetState() == RaceState::Finished);
    EXPECT_EQ(rig.race.GetPlayerBestLap(), player->bestLapTime);
}

#endif // SPARK_TEST_HAS_IMGUI && SPARK_TEST_HAS_PHYSICS

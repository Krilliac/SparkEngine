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

using namespace Racing;

// ============================================================================
// RacingVehicleSystem
// ============================================================================

TEST(Racing_VehicleSystem_Initialize)
{
    RacingVehicleSystem sys;
    EXPECT_TRUE(sys.Initialize(nullptr));
    sys.Shutdown();
}

TEST(Racing_VehicleSystem_CreateAndGetVehicle)
{
    RacingVehicleSystem sys;
    sys.Initialize(nullptr);

    uint32_t id = sys.CreateVehicle("Speedster", VehicleType::SportsCar, true);
    EXPECT_GT(id, 0u);

    const VehicleInstance* vehicle = sys.GetVehicle(id);
    EXPECT_TRUE(vehicle != nullptr);
    EXPECT_EQ(vehicle->name, std::string("Speedster"));
    EXPECT_TRUE(vehicle->isPlayer);
    sys.Shutdown();
}

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

TEST(Racing_VehicleSystem_VehicleCountAndList)
{
    RacingVehicleSystem sys;
    sys.Initialize(nullptr);

    EXPECT_EQ(sys.GetVehicleCount(), 0u);

    sys.CreateVehicle("Car A", VehicleType::MuscleCar);
    sys.CreateVehicle("Car B", VehicleType::Kart);
    EXPECT_EQ(sys.GetVehicleCount(), 2u);

    std::string list = sys.GetVehicleListString();
    EXPECT_FALSE(list.empty());
    sys.Shutdown();
}

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

#endif // SPARK_TEST_HAS_IMGUI

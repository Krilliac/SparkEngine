/**
 * @file TestMOD360OpenWorldPersistenceReal.cpp
 * @brief MOD-360: OpenWorld save/restart/load through the production SaveSystem and
 *        the module's `.ow_save` sidecar.
 *
 * The codec and per-subsystem restore are covered by the Gated_OWPersistence_* tests in
 * TestOpenWorldModule.cpp. These tests drive the console-facing OWEngineSystems::SaveGame
 * and LoadGame paths end to end: the engine slot is written by the real SaveSystem
 * singleton into an isolated temporary directory, every gameplay system is destroyed and
 * rebuilt as a restarted process would, and LoadGame must reproduce the captured state.
 * Damaged sidecars and a missing engine slot must be rejected without touching live state.
 *
 * OpenWorldAssets_* registers the module's real music tracks and area streaming manifests
 * and requires every file they name to exist in the repository asset tree.
 */

#include "TestFramework.h"

#ifdef SPARK_TEST_HAS_IMGUI

#include "../GameModules/SparkGameOpenWorld/Source/Core/OWEngineSystems.h"
#include "../GameModules/SparkGameOpenWorld/Source/Events/OWDynamicEventSystem.h"
#include "../GameModules/SparkGameOpenWorld/Source/Exploration/OWExplorationSystem.h"
#include "../GameModules/SparkGameOpenWorld/Source/Gathering/OWGatheringSystem.h"
#include "../GameModules/SparkGameOpenWorld/Source/Player/OWPlayerSystem.h"
#include "../GameModules/SparkGameOpenWorld/Source/Settlement/OWSettlementSystem.h"
#include "../GameModules/SparkGameOpenWorld/Source/Wildlife/OWWildlifeSystem.h"
#include "../GameModules/SparkGameOpenWorld/Source/World/OWWorldSetup.h"
#include "Audio/MusicManager.h"
#include "Engine/ECS/Components.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Engine/Streaming/SeamlessAreaManager.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace OpenWorld;

namespace
{
    /// Engine context exposing only the real SaveSystem and a World, as the module sees them.
    class OpenWorldPersistenceContext final : public Spark::IEngineContext
    {
      public:
        OpenWorldPersistenceContext(Spark::SaveSystem* saveSystem, ::World* world)
            : m_saveSystem(saveSystem), m_world(world)
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
        PhysicsSystem* GetPhysics() override { return nullptr; }
        const PhysicsSystem* GetPhysics() const override { return nullptr; }
        ::World* GetWorld() override { return m_world; }
        const ::World* GetWorld() const override { return m_world; }
        Spark::SaveSystem* GetSaveSystem() override { return m_saveSystem; }
        const Spark::SaveSystem* GetSaveSystem() const override { return m_saveSystem; }
        uint32_t GetEngineVersion() const override { return 0; }
        uint32_t GetSDKVersion() const override { return 0; }

      private:
        Spark::SaveSystem* m_saveSystem = nullptr;
        ::World* m_world = nullptr;
    };

    /// One process lifetime of the module: every gameplay system plus the engine bridge.
    struct OpenWorldSession
    {
        OWPlayerSystem player;
        OWExplorationSystem exploration;
        OWGatheringSystem gathering;
        OWSettlementSystem settlements;
        OWWildlifeSystem wildlife;
        OWDynamicEventSystem events;
        OWEngineSystems engine;

        bool Start(Spark::IEngineContext& context)
        {
            // Gameplay systems run context-free here, exactly as the Gated_OW* unit tests
            // construct them; only the persistence bridge needs the engine context.
            if (!player.Initialize(nullptr) || !exploration.Initialize(nullptr) || !gathering.Initialize(nullptr) ||
                !settlements.Initialize(nullptr) || !wildlife.Initialize(nullptr) || !events.Initialize(nullptr))
                return false;
            if (!engine.Initialize(&context))
                return false;
            engine.BindGameState(player, exploration, gathering, settlements, wildlife, events);
            return true;
        }

        /// Serialized snapshot of the live state, built from the public capture API.
        std::string Snapshot() const
        {
            OWGameSaveData data;
            data.player = player.CaptureSaveState();
            data.exploration = exploration.CaptureSaveState();
            data.gathering = gathering.CaptureSaveState();
            data.settlements = settlements.CaptureSaveState();
            data.wildlife = wildlife.CaptureSaveState();
            data.events = events.CaptureSaveState();
            return OWEngineSystems::SerializeSnapshot(data);
        }
    };

    /// Mutates player, world, gathered inventory, a joined event, and a placed camp.
    void PlayMOD360Session(OpenWorldSession& session)
    {
        session.player.SetPosition(412.5f, 18.25f, -903.75f);
        session.player.SetFacing(137.0f);
        session.player.TakeDamage(31.5f);
        session.player.Eat(12.0f);
        session.exploration.RevealPOI(3);
        session.gathering.AddResource(ResourceType::Crystal, 9);
        session.gathering.HarvestNode(1);
        const uint32_t campId = session.settlements.PlaceCamp("Ridge Camp", 405.0f, 18.0f, -900.0f, 2);
        session.settlements.UpgradeCamp(campId);
        const uint32_t eventId = session.events.TriggerEvent(2, 2, 410.0f, -905.0f);
        session.events.JoinEvent(eventId);
    }

    /// Temporary save root that also restores the SaveSystem singleton's directory.
    class ScopedMOD360SaveDirectory
    {
      public:
        explicit ScopedMOD360SaveDirectory(const char* name)
            : m_saveSystem(Spark::SaveSystem::GetInstance()), m_previousDirectory(m_saveSystem.GetSaveDirectory()),
              m_directory(std::filesystem::temp_directory_path() / (std::string("spark_mod360_") + name))
        {
            std::error_code error;
            std::filesystem::remove_all(m_directory, error);
            std::filesystem::create_directories(m_directory, error);
            m_saveSystem.SetFileCache(nullptr);
            m_initialized = m_saveSystem.Initialize(m_directory.string());
        }

        ~ScopedMOD360SaveDirectory()
        {
            m_saveSystem.SetSaveDirectory(m_previousDirectory);
            std::error_code error;
            std::filesystem::remove_all(m_directory, error);
        }

        ScopedMOD360SaveDirectory(const ScopedMOD360SaveDirectory&) = delete;
        ScopedMOD360SaveDirectory& operator=(const ScopedMOD360SaveDirectory&) = delete;

        bool IsInitialized() const { return m_initialized; }
        Spark::SaveSystem& System() { return m_saveSystem; }
        const std::filesystem::path& Path() const { return m_directory; }

      private:
        Spark::SaveSystem& m_saveSystem;
        std::string m_previousDirectory;
        std::filesystem::path m_directory;
        bool m_initialized = false;
    };

    std::string ReadMOD360File(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    bool WriteMOD360File(const std::filesystem::path& path, const std::string& bytes)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return output.good();
    }

    bool StartsWith(const std::string& text, const char* prefix)
    {
        return text.rfind(prefix, 0) == 0;
    }
} // namespace

TEST(OpenWorldPersistence_RestartRestoresPlayerWorldEventSettlementState)
{
    ScopedMOD360SaveDirectory saves("restart");
    ASSERT_TRUE(saves.IsInitialized());
    const std::filesystem::path sidecarPath = saves.Path() / "OpenWorld" / "restart.ow_save";

    std::string savedSnapshot;
    {
        World world;
        world.AddComponent<Transform>(world.CreateEntity("mod360-landmark"));
        OpenWorldPersistenceContext context(&saves.System(), &world);
        auto session = std::make_unique<OpenWorldSession>();
        ASSERT_TRUE(session->Start(context));
        PlayMOD360Session(*session);
        savedSnapshot = session->Snapshot();

        const std::string result = session->engine.SaveGame("restart");
        EXPECT_TRUE(StartsWith(result, "Saved OpenWorld game"));
        // The sidecar pairs with the engine slot inside the SaveSystem directory.
        EXPECT_TRUE(saves.System().SaveExists("restart"));
        EXPECT_TRUE(std::filesystem::exists(sidecarPath));
        EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(sidecarPath).concat(".tmp")));
        EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(sidecarPath).concat(".bak")));
    }

    // Restart: a brand-new world and freshly initialized systems start from defaults.
    World restartedWorld;
    OpenWorldPersistenceContext context(&saves.System(), &restartedWorld);
    auto restarted = std::make_unique<OpenWorldSession>();
    ASSERT_TRUE(restarted->Start(context));
    EXPECT_TRUE(restarted->Snapshot() != savedSnapshot);
    EXPECT_EQ(restarted->settlements.GetCampCount(), static_cast<size_t>(0));

    const std::string result = restarted->engine.LoadGame("restart");
    EXPECT_TRUE(StartsWith(result, "Loaded OpenWorld game"));
    EXPECT_EQ(restarted->Snapshot(), savedSnapshot);
    EXPECT_NEAR(restarted->player.GetWorldState().posX, 412.5f, 0.001f);
    EXPECT_NEAR(restarted->player.GetWorldState().posZ, -903.75f, 0.001f);
    EXPECT_EQ(restarted->gathering.GetInventory().Get(ResourceType::Crystal), 9u);
    EXPECT_EQ(restarted->settlements.GetCampCount(), static_cast<size_t>(1));
    EXPECT_EQ(restarted->events.GetActiveEventCount(), static_cast<size_t>(1));
    EXPECT_TRUE(restarted->events.CaptureSaveState().activeEvents.front().playerParticipating);
    // The engine world came back through SaveSystem as well.
    EXPECT_EQ(restartedWorld.GetEntityCount(), 1u);

    // Saving over an existing slot replaces it atomically and leaves no backup behind.
    restarted->player.SetPosition(10.0f, 2.0f, 30.0f);
    EXPECT_TRUE(StartsWith(restarted->engine.SaveGame("restart"), "Saved OpenWorld game"));
    EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(sidecarPath).concat(".bak")));
    const std::string resaved = restarted->Snapshot();
    restarted->player.SetPosition(-1.0f, -1.0f, -1.0f);
    EXPECT_TRUE(StartsWith(restarted->engine.LoadGame("restart"), "Loaded OpenWorld game"));
    EXPECT_EQ(restarted->Snapshot(), resaved);
    restarted->engine.Shutdown();
}

TEST(OpenWorldPersistence_CorruptSidecarLeavesStateUnchanged)
{
    ScopedMOD360SaveDirectory saves("corrupt");
    ASSERT_TRUE(saves.IsInitialized());
    const std::filesystem::path sidecarPath = saves.Path() / "OpenWorld" / "corrupt.ow_save";

    {
        World world;
        OpenWorldPersistenceContext context(&saves.System(), &world);
        auto session = std::make_unique<OpenWorldSession>();
        ASSERT_TRUE(session->Start(context));
        PlayMOD360Session(*session);
        ASSERT_TRUE(StartsWith(session->engine.SaveGame("corrupt"), "Saved OpenWorld game"));
    }
    const std::string intactSidecar = ReadMOD360File(sidecarPath);
    ASSERT_FALSE(intactSidecar.empty());

    World liveWorld;
    liveWorld.AddComponent<Transform>(liveWorld.CreateEntity("mod360-live-a"));
    liveWorld.AddComponent<Transform>(liveWorld.CreateEntity("mod360-live-b"));
    OpenWorldPersistenceContext context(&saves.System(), &liveWorld);
    auto live = std::make_unique<OpenWorldSession>();
    ASSERT_TRUE(live->Start(context));
    live->player.SetPosition(-50.0f, 4.0f, 75.0f);
    live->gathering.AddResource(ResourceType::Crystal, 2);
    const std::string liveSnapshot = live->Snapshot();

    // Tampered: one body byte flipped inside the player record breaks the checksum.
    std::string tampered = intactSidecar;
    const size_t playerRecord = tampered.find("PLAYER ");
    ASSERT_TRUE(playerRecord != std::string::npos);
    const size_t digit = tampered.find_first_of("0123456789", playerRecord);
    ASSERT_TRUE(digit != std::string::npos);
    tampered[digit] = tampered[digit] == '1' ? '2' : '1';
    ASSERT_TRUE(WriteMOD360File(sidecarPath, tampered));
    std::string result = live->engine.LoadGame("corrupt");
    EXPECT_TRUE(result.find("checksum") != std::string::npos);
    EXPECT_EQ(live->Snapshot(), liveSnapshot);
    EXPECT_EQ(liveWorld.GetEntityCount(), 2u);

    // Truncated: half the file is gone.
    ASSERT_TRUE(WriteMOD360File(sidecarPath, intactSidecar.substr(0, intactSidecar.size() / 2)));
    result = live->engine.LoadGame("corrupt");
    EXPECT_TRUE(StartsWith(result, "OpenWorld save is invalid"));
    EXPECT_EQ(live->Snapshot(), liveSnapshot);
    EXPECT_EQ(liveWorld.GetEntityCount(), 2u);

    // Missing sidecar with a present engine slot.
    std::filesystem::remove(sidecarPath);
    result = live->engine.LoadGame("corrupt");
    EXPECT_TRUE(result.find("missing") != std::string::npos);
    EXPECT_EQ(live->Snapshot(), liveSnapshot);
    EXPECT_EQ(liveWorld.GetEntityCount(), 2u);

    // Missing engine slot with an intact sidecar.
    ASSERT_TRUE(WriteMOD360File(sidecarPath, intactSidecar));
    ASSERT_TRUE(saves.System().DeleteSave("corrupt"));
    ASSERT_FALSE(saves.System().SaveExists("corrupt"));
    result = live->engine.LoadGame("corrupt");
    EXPECT_TRUE(StartsWith(result, "No save found"));
    EXPECT_EQ(live->Snapshot(), liveSnapshot);
    EXPECT_EQ(liveWorld.GetEntityCount(), 2u);

    // Unsafe slot names never reach the filesystem.
    EXPECT_TRUE(StartsWith(live->engine.LoadGame("../corrupt"), "Invalid save slot name"));
    EXPECT_EQ(live->Snapshot(), liveSnapshot);
    live->engine.Shutdown();
}

namespace
{
    /// Engine context exposing only the host music and area-streaming services.
    class OpenWorldAssetContext final : public Spark::IEngineContext
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
        ::AudioEngine* GetAudio() override { return nullptr; }
        const ::AudioEngine* GetAudio() const override { return nullptr; }
        PhysicsSystem* GetPhysics() override { return nullptr; }
        const PhysicsSystem* GetPhysics() const override { return nullptr; }
        ::World* GetWorld() override { return nullptr; }
        const ::World* GetWorld() const override { return nullptr; }
        Spark::Streaming::SeamlessAreaManager* GetAreaStreaming() override
        {
            return &Spark::Streaming::SeamlessAreaManager::GetInstance();
        }
        const Spark::Streaming::SeamlessAreaManager* GetAreaStreaming() const override
        {
            return &Spark::Streaming::SeamlessAreaManager::GetInstance();
        }
        Spark::Audio::MusicManager* GetMusic() override { return &Spark::Audio::MusicManager::GetInstance(); }
        const Spark::Audio::MusicManager* GetMusic() const override
        {
            return &Spark::Audio::MusicManager::GetInstance();
        }
        uint32_t GetEngineVersion() const override { return 0; }
        uint32_t GetSDKVersion() const override { return 0; }
    };

    /// Track names from MusicManager's console listing ("  <name> [<bpm> BPM...]" per track).
    std::vector<std::string> RegisteredMusicTrackNames(const Spark::Audio::MusicManager& music)
    {
        std::vector<std::string> names;
        std::istringstream listing(music.Console_ListTracks());
        std::string line;
        while (std::getline(listing, line))
        {
            const size_t bracket = line.find(" [");
            if (line.rfind("  ", 0) == 0 && bracket != std::string::npos)
                names.push_back(line.substr(2, bracket - 2));
        }
        return names;
    }

    /// True when the repository-relative asset path names an existing regular file.
    bool OpenWorldAssetExists(const std::string& relativePath)
    {
        const std::filesystem::path path = std::filesystem::path(SPARK_TEST_SOURCE_DIR) / relativePath;
        std::error_code error;
        return !relativePath.empty() && std::filesystem::is_regular_file(path, error);
    }
} // namespace

TEST(OpenWorldAssets_AllRegisteredAssetsExist)
{
    auto& music = Spark::Audio::MusicManager::GetInstance();
    auto& streaming = Spark::Streaming::SeamlessAreaManager::GetInstance();

    // Start from an empty track registry so every listed track is one this module registered.
    for (const auto& name : RegisteredMusicTrackNames(music))
        music.UnregisterTrack(name);

    OpenWorldAssetContext context;
    OWEngineSystems engine;
    ASSERT_TRUE(engine.Initialize(&context));
    OWWorldSetup worldSetup;
    ASSERT_TRUE(worldSetup.Initialize(&context));

    // Every music track the real OWEngineSystems registers names a file that exists.
    const std::vector<std::string> trackNames = RegisteredMusicTrackNames(music);
    EXPECT_EQ(trackNames.size(), static_cast<size_t>(9));
    for (const auto& name : trackNames)
    {
        const Spark::Audio::MusicTrack* track = music.GetTrack(name);
        ASSERT_TRUE(track != nullptr);
        EXPECT_TRUE(track->filepath.rfind("Assets/", 0) == 0);
        EXPECT_TRUE(OpenWorldAssetExists(track->filepath));
    }

    // Every region the real OWWorldSetup streams carries a manifest whose paths all exist.
    const auto& loader = streaming.GetAssetLoader();
    EXPECT_EQ(worldSetup.GetRegions().size(), static_cast<size_t>(8));
    for (const auto& region : worldSetup.GetRegions())
    {
        const Spark::Streaming::SceneManifest* manifest = loader.GetManifest(region.regionId);
        ASSERT_TRUE(manifest != nullptr);
        EXPECT_EQ(manifest->name, region.name);
        EXPECT_FALSE(manifest->meshPaths.empty());
        EXPECT_FALSE(manifest->texturePaths.empty());
        EXPECT_FALSE(manifest->audioPaths.empty());
        for (const auto& path : manifest->AllPaths())
        {
            EXPECT_TRUE(path.rfind("Assets/", 0) == 0);
            EXPECT_TRUE(OpenWorldAssetExists(path));
        }
    }

    // OWWorldSetup::Shutdown unregisters its areas; the module never removes its tracks.
    std::vector<Spark::Streaming::AreaID> regionIds;
    for (const auto& region : worldSetup.GetRegions())
        regionIds.push_back(region.regionId);
    worldSetup.Shutdown();
    for (const auto regionId : regionIds)
        EXPECT_FALSE(loader.HasManifest(regionId));
    for (const auto& name : trackNames)
        music.UnregisterTrack(name);
    engine.Shutdown();
}

#endif // SPARK_TEST_HAS_IMGUI

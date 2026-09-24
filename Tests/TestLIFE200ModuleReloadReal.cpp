/**
 * @file TestLIFE200ModuleReloadReal.cpp
 * @brief LIFE-200: a module hot reload hands host registrations to the live image.
 *
 * ModuleManager::ReloadModule initializes the replacement before the outgoing
 * instance's OnUnload runs. Modules tear down by shared names (console command
 * names, rule categories, serializer type names), so every host registry must
 * scope that teardown to the calling module image. These tests drive the real
 * SparkGame module and the registry lifecycle fixture through successful and
 * failed reloads and then call the surviving callbacks, so a callback left
 * pointing into an unloaded image faults (or trips ASan) instead of passing.
 */

#include "TestFramework.h"

#include "Core/ModuleManager.h"
#include "Engine/ECS/Components.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Utils/InvalidStateDetector.h"
#include "Utils/SparkConsole.h"
#include <Spark/Version.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace
{
    constexpr const char* kShowcaseModule = "Spark Default - Engine Showcase";
    constexpr const char* kFixtureModule = "Spark Registry Lifecycle Fixture";
    constexpr const char* kShowcaseCommands[] = {"showcase_status", "showcase_weather", "showcase_save",
                                                 "showcase_load", "showcase_spawn"};

    class Life200EngineContext final : public Spark::IEngineContext
    {
      public:
        explicit Life200EngineContext(Spark::SaveSystem* saveSystem = nullptr) : m_saveSystem(saveSystem) {}

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
        Spark::SaveSystem* GetSaveSystem() override { return m_saveSystem; }
        const Spark::SaveSystem* GetSaveSystem() const override { return m_saveSystem; }
        uint32_t GetEngineVersion() const override { return SPARK_ENGINE_VERSION_PACKED; }
        uint32_t GetSDKVersion() const override { return SPARK_SDK_VERSION; }

      private:
        Spark::SaveSystem* m_saveSystem = nullptr;
    };

    void SetFixtureThrowOnLoad(bool enabled)
    {
#ifdef _WIN32
        _putenv_s("SPARK_REGISTRY_FIXTURE_THROW_ON_LOAD", enabled ? "1" : "");
#else
        if (enabled)
            setenv("SPARK_REGISTRY_FIXTURE_THROW_ON_LOAD", "1", 1);
        else
            unsetenv("SPARK_REGISTRY_FIXTURE_THROW_ON_LOAD");
#endif
    }

    /// Initializes the host console for the test and restores its prior state.
    struct ConsoleScope final
    {
        Spark::SimpleConsole& console = Spark::SimpleConsole::GetInstance();
        bool restoreUninitialized = !console.IsInitialized();
        ConsoleScope()
        {
            if (restoreUninitialized)
                console.Initialize();
        }
        ~ConsoleScope()
        {
            if (restoreUninitialized)
                console.Shutdown();
        }
    };

    /// Shuts down and unloads every module even when an assertion returns early.
    struct ManagerScope final
    {
        ModuleManager& manager;
        ~ManagerScope()
        {
            manager.ShutdownAllAfterPreflight();
            manager.UnloadAll();
        }
    };

    /// Calls the TagComponent serializer so a callback owned by an unloaded image faults.
    std::string SerializeTagThroughRegistry()
    {
        TagComponent tag;
        tag.tags = {"life200", "reload"};
        const Spark::SerializedComponent serialized =
            Spark::ComponentSerializerRegistry::GetInstance().Serialize("TagComponent", &tag);
        const auto tags = serialized.properties.find("tags");
        return serialized.typeName + ":" + (tags == serialized.properties.end() ? std::string{} : tags->second);
    }

    bool AllShowcaseCommandsRegistered(const Spark::SimpleConsole& console)
    {
        for (const char* command : kShowcaseCommands)
        {
            if (!console.HasCommand(command))
                return false;
        }
        return true;
    }

    bool AnyShowcaseCommandRegistered(const Spark::SimpleConsole& console)
    {
        for (const char* command : kShowcaseCommands)
        {
            if (console.HasCommand(command))
                return true;
        }
        return false;
    }
} // namespace

#if !defined(_WIN32) && defined(SPARK_TEST_SPARK_GAME_MODULE_PATH)
namespace
{
    /// Removes the host's TagComponent serializer for the test and restores it afterwards.
    struct SerializerScope final
    {
        Spark::ComponentSerializerRegistry& serializers = Spark::ComponentSerializerRegistry::GetInstance();
        Spark::ComponentSerializerRegistry::RegistrationHandle prior = serializers.TakeRegistration("TagComponent");
        ~SerializerScope()
        {
            serializers.Unregister("TagComponent");
            (void)serializers.RestoreRegistration(std::move(prior));
        }
    };

    std::filesystem::path SidecarOf(const std::filesystem::path& modulePath)
    {
        std::filesystem::path sidecar = modulePath;
        sidecar += ".sparkabi";
        return sidecar;
    }
} // namespace

TEST(LIFE200_SparkGameSuccessfulReloadKeepsReplacementRegistrations)
{
    ConsoleScope consoleScope;
    SerializerScope serializerScope;
    auto& console = consoleScope.console;
    auto& detector = Spark::InvalidStateDetector::GetInstance();
    ASSERT_FALSE(AnyShowcaseCommandRegistered(console));
    ASSERT_FALSE(detector.HasRule("Base.HealthInvariant"));
    const uint32_t initialRuleCount = detector.GetRuleCount();

    Life200EngineContext context(&Spark::SaveSystem::GetInstance());
    ModuleManager manager;
    ManagerScope managerScope{manager};
    ASSERT_TRUE(manager.LoadModule(SPARK_TEST_SPARK_GAME_MODULE_PATH));
    manager.InitializeAll(&context);
    ASSERT_TRUE(manager.GetModule(kShowcaseModule) != nullptr);
    ASSERT_TRUE(AllShowcaseCommandsRegistered(console));
    ASSERT_TRUE(serializerScope.serializers.HasSerializer("TagComponent"));

    // Two consecutive swaps: each outgoing image's name-based teardown runs
    // after its replacement registered the same names.
    for (int reload = 0; reload < 2; ++reload)
    {
        Spark::IModule* const outgoing = manager.GetModule(kShowcaseModule);
        ASSERT_TRUE(manager.ReloadModule(kShowcaseModule, &context));
        EXPECT_TRUE(manager.GetModule(kShowcaseModule) != outgoing);

        EXPECT_TRUE(AllShowcaseCommandsRegistered(console));
        EXPECT_TRUE(detector.HasRule("Base.HealthInvariant"));
        EXPECT_EQ(detector.GetRuleCount(), initialRuleCount + 1);
        ASSERT_TRUE(serializerScope.serializers.HasSerializer("TagComponent"));
        // The surviving serializer must belong to the live image; the outgoing
        // image has already been unmapped.
        EXPECT_EQ(SerializeTagThroughRegistry(), std::string("TagComponent:life200,reload"));
    }

    ASSERT_TRUE(manager.ShutdownAll());
    EXPECT_FALSE(AnyShowcaseCommandRegistered(console));
    EXPECT_FALSE(detector.HasRule("Base.HealthInvariant"));
    EXPECT_EQ(detector.GetRuleCount(), initialRuleCount);
    EXPECT_FALSE(serializerScope.serializers.HasSerializer("TagComponent"));
}

TEST(LIFE200_SparkGameFailedReloadKeepsWorkingRegistrations)
{
    ConsoleScope consoleScope;
    SerializerScope serializerScope;
    auto& console = consoleScope.console;
    auto& detector = Spark::InvalidStateDetector::GetInstance();
    ASSERT_FALSE(AnyShowcaseCommandRegistered(console));
    const uint32_t initialRuleCount = detector.GetRuleCount();

    // Load a private copy so the replacement metadata can be broken without
    // touching the build output other tests use.
    const std::filesystem::path sourcePath = SPARK_TEST_SPARK_GAME_MODULE_PATH;
    const std::filesystem::path copyDirectory = std::filesystem::temp_directory_path() / "SparkLIFE200FailedReload";
    const std::filesystem::path modulePath = copyDirectory / sourcePath.filename();
    std::error_code ec;
    std::filesystem::remove_all(copyDirectory, ec);
    std::filesystem::create_directories(copyDirectory);
    std::filesystem::copy_file(sourcePath, modulePath);
    std::filesystem::copy_file(SidecarOf(sourcePath), SidecarOf(modulePath));
    struct CopyScope final
    {
        std::filesystem::path directory;
        ~CopyScope()
        {
            std::error_code cleanup;
            std::filesystem::remove_all(directory, cleanup);
        }
    } copyScope{copyDirectory};

    Life200EngineContext context(&Spark::SaveSystem::GetInstance());
    ModuleManager manager;
    ManagerScope managerScope{manager};
    ASSERT_TRUE(manager.LoadModule(modulePath.string()));
    manager.InitializeAll(&context);
    Spark::IModule* const workingInstance = manager.GetModule(kShowcaseModule);
    ASSERT_TRUE(workingInstance != nullptr);
    ASSERT_TRUE(AllShowcaseCommandsRegistered(console));

    std::string sidecar;
    {
        std::ifstream input(SidecarOf(modulePath));
        sidecar.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    const std::string expectedSdk = "sdk_version=" + std::to_string(SPARK_SDK_VERSION);
    const size_t sdkField = sidecar.find(expectedSdk);
    ASSERT_TRUE(sdkField != std::string::npos);
    sidecar.replace(sdkField, expectedSdk.size(), "sdk_version=" + std::to_string(SPARK_SDK_VERSION + 1));
    {
        std::ofstream output(SidecarOf(modulePath), std::ios::trunc);
        output << sidecar;
    }

    EXPECT_FALSE(manager.ReloadModule(kShowcaseModule, &context));
    EXPECT_TRUE(manager.GetModule(kShowcaseModule) == workingInstance);
    EXPECT_TRUE(AllShowcaseCommandsRegistered(console));
    EXPECT_TRUE(detector.HasRule("Base.HealthInvariant"));
    EXPECT_EQ(detector.GetRuleCount(), initialRuleCount + 1);
    EXPECT_EQ(SerializeTagThroughRegistry(), std::string("TagComponent:life200,reload"));

    ASSERT_TRUE(manager.ShutdownAll());
    EXPECT_FALSE(AnyShowcaseCommandRegistered(console));
    EXPECT_EQ(detector.GetRuleCount(), initialRuleCount);
    EXPECT_FALSE(serializerScope.serializers.HasSerializer("TagComponent"));
}
#endif

TEST(LIFE200_ThrowingReplacementKeepsWorkingModuleRegistrations)
{
    ConsoleScope consoleScope;
    auto& console = consoleScope.console;
    auto& detector = Spark::InvalidStateDetector::GetInstance();
    ASSERT_FALSE(console.HasCommand("registry_fixture_status"));
    ASSERT_FALSE(detector.HasRule("RegistryFixture.Rule"));
    const uint32_t initialRuleCount = detector.GetRuleCount();

    Life200EngineContext context;
    ModuleManager manager;
    ManagerScope managerScope{manager};
    ASSERT_TRUE(manager.LoadModule(SPARK_TEST_REGISTRY_LIFECYCLE_MODULE_PATH));
    ASSERT_TRUE(manager.InitializeAll(&context));
    Spark::IModule* const workingInstance = manager.GetModule(kFixtureModule);
    ASSERT_TRUE(workingInstance != nullptr);

    // The replacement registers the same command and rule names, throws, and
    // its partial teardown removes them by name. Only its own entries may go.
    SetFixtureThrowOnLoad(true);
    const bool reloaded = manager.ReloadModule(kFixtureModule, &context);
    SetFixtureThrowOnLoad(false);

    EXPECT_FALSE(reloaded);
    EXPECT_TRUE(manager.GetModule(kFixtureModule) == workingInstance);
    EXPECT_TRUE(console.HasCommand("registry_fixture_status"));
    EXPECT_TRUE(detector.HasRule("RegistryFixture.Rule"));
    EXPECT_EQ(detector.GetRuleCount(), initialRuleCount + 1);
    EXPECT_TRUE(console.ExecuteCommand("registry_fixture_status"));

    ASSERT_TRUE(manager.ShutdownAll());
    EXPECT_FALSE(console.HasCommand("registry_fixture_status"));
    EXPECT_EQ(detector.GetRuleCount(), initialRuleCount);
}

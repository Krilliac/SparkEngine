#include "TestFramework.h"

#include "Core/ModuleHotReload.h"
#include "Core/ModuleManager.h"
#include <Spark/ModuleABI.h>
#include <Spark/Version.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#ifndef SPARK_TEST_MISMATCHED_MODULE_PATH
#error SPARK_TEST_MISMATCHED_MODULE_PATH must name the mismatched module fixture
#endif

#ifndef SPARK_TEST_COMPATIBLE_MODULE_PATH
#error SPARK_TEST_COMPATIBLE_MODULE_PATH must name the compatible module fixture
#endif

namespace
{
    class NullEngineContext final : public Spark::IEngineContext
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
        uint32_t GetEngineVersion() const override { return SPARK_ENGINE_VERSION_PACKED; }
        uint32_t GetSDKVersion() const override { return SPARK_SDK_VERSION; }
    };

    std::filesystem::path CopyCompatibleFixtureToTemp(const std::string& stem)
    {
        const std::filesystem::path sourcePath = SPARK_TEST_COMPATIBLE_MODULE_PATH;
        const std::filesystem::path destination =
            std::filesystem::temp_directory_path() / (stem + sourcePath.extension().string());
        std::error_code ec;
        std::filesystem::remove(destination, ec);
        std::filesystem::remove(destination.string() + ".sparkabi", ec);
        std::filesystem::copy_file(sourcePath, destination, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy_file(sourcePath.string() + ".sparkabi", destination.string() + ".sparkabi",
                                   std::filesystem::copy_options::overwrite_existing);
        return destination;
    }

    void RemoveModuleCopy(const std::filesystem::path& modulePath)
    {
        std::error_code ec;
        std::filesystem::remove(modulePath.string() + ".sparkabi", ec);
        std::filesystem::remove(modulePath, ec);
    }

    void SetSentinelEnvironment(const std::string& path)
    {
#ifdef _WIN32
        _putenv_s("SPARK_MODULE_ABI_SENTINEL", path.c_str());
#else
        setenv("SPARK_MODULE_ABI_SENTINEL", path.c_str(), 1);
#endif
    }

    void ClearSentinelEnvironment()
    {
#ifdef _WIN32
        _putenv_s("SPARK_MODULE_ABI_SENTINEL", "");
#else
        unsetenv("SPARK_MODULE_ABI_SENTINEL");
#endif
    }

    void SetFailOnLoadEnvironment(bool enabled)
    {
#ifdef _WIN32
        _putenv_s("SPARK_MODULE_ABI_FAIL_ON_LOAD", enabled ? "1" : "");
#else
        if (enabled)
            setenv("SPARK_MODULE_ABI_FAIL_ON_LOAD", "1", 1);
        else
            unsetenv("SPARK_MODULE_ABI_FAIL_ON_LOAD");
#endif
    }
} // namespace

TEST(ModuleABI_ExpectedDescriptorIsCompatible)
{
    EXPECT_TRUE(Spark::CheckModuleCompatibility(&Spark::kExpectedModuleCompatibility) ==
                Spark::ModuleCompatibilityStatus::Compatible);

    auto mismatch = Spark::kExpectedModuleCompatibility;
    ++mismatch.sdkVersion;
    EXPECT_TRUE(Spark::CheckModuleCompatibility(&mismatch) == Spark::ModuleCompatibilityStatus::SDKVersionMismatch);
}

TEST(ModuleABI_DiscoveryDoesNotExecuteCandidate)
{
    const std::filesystem::path fixturePath = SPARK_TEST_MISMATCHED_MODULE_PATH;
    const std::filesystem::path sentinelPath =
        std::filesystem::temp_directory_path() / "spark-module-abi-discovery-sentinel.txt";
    std::error_code ec;
    std::filesystem::remove(sentinelPath, ec);
    SetSentinelEnvironment(sentinelPath.string());

    ModuleManager manager;
    const auto discovered = manager.DiscoverModules(fixturePath.parent_path().string());
    const bool found = std::any_of(discovered.begin(), discovered.end(), [&](const DiscoveredModule& module)
                                   { return std::filesystem::path(module.path).filename() == fixturePath.filename(); });

    ClearSentinelEnvironment();
    EXPECT_TRUE(found);
    EXPECT_FALSE(std::filesystem::exists(sentinelPath));
}

TEST(ModuleABI_MismatchRejectedBeforeStaticConstructorInjectionOrFactory)
{
    const std::filesystem::path fixturePath = SPARK_TEST_MISMATCHED_MODULE_PATH;
    const std::filesystem::path sentinelPath =
        std::filesystem::temp_directory_path() / "spark-module-abi-load-sentinel.txt";
    std::error_code ec;
    std::filesystem::remove(sentinelPath, ec);
    SetSentinelEnvironment(sentinelPath.string());

    ModuleManager manager;
    const bool loaded = manager.LoadModule(fixturePath.string());

    ClearSentinelEnvironment();
    EXPECT_FALSE(loaded);
    EXPECT_FALSE(std::filesystem::exists(sentinelPath));
    EXPECT_TRUE(manager.GetLoadedModuleInfo().empty());
}

TEST(ModuleABI_ModifiedBinaryRejectedByHashBeforeDllMainOrStaticConstructor)
{
    const std::filesystem::path fixturePath = SPARK_TEST_COMPATIBLE_MODULE_PATH;
    const std::filesystem::path copiedPath =
        std::filesystem::temp_directory_path() / ("SparkCompatibleHashTampered" + fixturePath.extension().string());
    const std::filesystem::path copiedSidecar = copiedPath.string() + ".sparkabi";
    const std::filesystem::path sentinelPath =
        std::filesystem::temp_directory_path() / "spark-module-abi-hash-sentinel.txt";
    std::error_code ec;
    std::filesystem::remove(copiedPath, ec);
    std::filesystem::remove(copiedSidecar, ec);
    std::filesystem::remove(sentinelPath, ec);
    std::filesystem::copy_file(fixturePath, copiedPath, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(fixturePath.string() + ".sparkabi", copiedSidecar,
                               std::filesystem::copy_options::overwrite_existing);
    {
        std::ofstream tamper(copiedPath, std::ios::binary | std::ios::app);
        tamper.put('\0');
    }

    SetSentinelEnvironment(sentinelPath.string());
    ModuleManager manager;
    const bool loaded = manager.LoadModule(copiedPath.string());
    ClearSentinelEnvironment();

    EXPECT_FALSE(loaded);
    EXPECT_FALSE(std::filesystem::exists(sentinelPath));
    EXPECT_TRUE(manager.GetLoadedModuleInfo().empty());
    std::filesystem::remove(copiedPath, ec);
    std::filesystem::remove(copiedSidecar, ec);
}

TEST(ModuleABI_CompatibleMacroModuleStillLoads)
{
    ModuleManager manager;
    EXPECT_TRUE(manager.LoadModule(SPARK_TEST_COMPATIBLE_MODULE_PATH));

    const auto loaded = manager.GetLoadedModuleInfo();
    EXPECT_EQ(loaded.size(), size_t{1});
    if (!loaded.empty())
    {
        EXPECT_EQ(loaded[0].name, std::string("Spark Compatible ABI Fixture"));
        EXPECT_EQ(loaded[0].version, std::string("1.0.0"));
        EXPECT_TRUE(loaded[0].kind == Spark::ModuleKind::Addon);
        EXPECT_TRUE(loaded[0].kindKnown);
    }

    manager.UnloadAll();
}

TEST(ModuleABI_FailedTransactionalReloadPreservesWorkingModule)
{
    const std::filesystem::path modulePath = CopyCompatibleFixtureToTemp("SparkReloadPreservationModule");
    NullEngineContext context;
    ModuleManager manager;
    EXPECT_TRUE(manager.LoadModule(modulePath.string()));
    manager.InitializeAll(&context);

    Spark::IModule* const workingInstance = manager.GetModule("Spark Compatible ABI Fixture");
    EXPECT_TRUE(workingInstance != nullptr);

    // Make only the replacement metadata incompatible. The already loaded
    // working image remains valid and must not be unloaded on this failure.
    std::string sidecar;
    {
        std::ifstream input(modulePath.string() + ".sparkabi");
        sidecar.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    const size_t sdkField = sidecar.find("sdk_version=2");
    EXPECT_TRUE(sdkField != std::string::npos);
    if (sdkField != std::string::npos)
        sidecar.replace(sdkField, std::string("sdk_version=2").size(), "sdk_version=3");
    {
        std::ofstream output(modulePath.string() + ".sparkabi", std::ios::trunc);
        output << sidecar;
    }

    EXPECT_FALSE(manager.ReloadModule("Spark Compatible ABI Fixture", &context));
    EXPECT_TRUE(manager.GetModule("Spark Compatible ABI Fixture") == workingInstance);
    EXPECT_EQ(std::string(workingInstance->GetModuleInfo().name), std::string("Spark Compatible ABI Fixture"));

    manager.ShutdownAll();
    manager.UnloadAll();
    RemoveModuleCopy(modulePath);
}

TEST(ModuleABI_FailedReplacementInitializationPreservesWorkingModule)
{
    const std::filesystem::path modulePath = CopyCompatibleFixtureToTemp("SparkReloadInitFailureModule");
    NullEngineContext context;
    ModuleManager manager;
    EXPECT_TRUE(manager.LoadModule(modulePath.string()));
    manager.InitializeAll(&context);

    Spark::IModule* const workingInstance = manager.GetModule("Spark Compatible ABI Fixture");
    EXPECT_TRUE(workingInstance != nullptr);

    SetFailOnLoadEnvironment(true);
    const bool reloadSucceeded = manager.ReloadModule("Spark Compatible ABI Fixture", &context);
    SetFailOnLoadEnvironment(false);

    EXPECT_FALSE(reloadSucceeded);
    EXPECT_TRUE(manager.GetModule("Spark Compatible ABI Fixture") == workingInstance);
    EXPECT_EQ(std::string(workingInstance->GetModuleInfo().name), std::string("Spark Compatible ABI Fixture"));

    manager.ShutdownAll();
    manager.UnloadAll();
    RemoveModuleCopy(modulePath);
}

TEST(ModuleABI_HotReloadCallbackCanReenterManagerWithoutDeadlock)
{
    const std::filesystem::path modulePath = CopyCompatibleFixtureToTemp("SparkReentrantReloadModule");
    NullEngineContext context;
    ModuleManager manager;
    EXPECT_TRUE(manager.LoadModule(modulePath.string()));
    manager.InitializeAll(&context);

    Spark::ModuleHotReloadManager hotReload;
    hotReload.Initialize(&manager, &context);
    hotReload.WatchModule("Spark Compatible ABI Fixture", modulePath.string());

    bool callbackRan = false;
    std::string statusFromCallback;
    hotReload.SetReloadCallback(
        [&](const std::string&, bool success)
        {
            callbackRan = success;
            statusFromCallback = hotReload.GetStatus();
            hotReload.SetReloadCallback(nullptr);
        });

    EXPECT_TRUE(hotReload.ForceReload("Spark Compatible ABI Fixture"));
    EXPECT_TRUE(callbackRan);
    EXPECT_TRUE(statusFromCallback.find("Reloads:  1") != std::string::npos);
    EXPECT_EQ(hotReload.GetReloadCount(), 1);

    manager.ShutdownAll();
    manager.UnloadAll();
    RemoveModuleCopy(modulePath);
}

#include "TestFramework.h"

#include "Core/ModuleHotReload.h"
#include "Core/ModuleManager.h"
#include "Utils/LocalFileCache.h"
#include <Spark/ModuleABI.h>
#include <Spark/Version.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef SPARK_TEST_MISMATCHED_MODULE_PATH
#error SPARK_TEST_MISMATCHED_MODULE_PATH must name the mismatched module fixture
#endif

#ifndef SPARK_TEST_COMPATIBLE_MODULE_PATH
#error SPARK_TEST_COMPATIBLE_MODULE_PATH must name the compatible module fixture
#endif

#ifndef SPARK_TEST_SIBLING_DEPENDENT_MODULE_PATH
#error SPARK_TEST_SIBLING_DEPENDENT_MODULE_PATH must name the sibling-dependent module fixture
#endif

namespace
{
    std::filesystem::path PathFromUtf8(std::string_view path)
    {
        return std::filesystem::u8path(path.begin(), path.end());
    }

    std::string PathToUtf8(const std::filesystem::path& path)
    {
        const std::u8string utf8 = path.generic_u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }

    std::filesystem::path SidecarPath(const std::filesystem::path& modulePath)
    {
        std::filesystem::path sidecar = modulePath;
        sidecar += ".sparkabi";
        return sidecar;
    }

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

    std::filesystem::path CopyCompatibleFixtureToTemp(const std::filesystem::path& stem)
    {
        const std::filesystem::path sourcePath = PathFromUtf8(SPARK_TEST_COMPATIBLE_MODULE_PATH);
        std::filesystem::path destination = std::filesystem::temp_directory_path() / stem;
        destination += sourcePath.extension();
        std::error_code ec;
        std::filesystem::create_directories(destination.parent_path(), ec);
        std::filesystem::remove(destination, ec);
        std::filesystem::remove(SidecarPath(destination), ec);
        std::filesystem::copy_file(sourcePath, destination, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy_file(SidecarPath(sourcePath), SidecarPath(destination),
                                   std::filesystem::copy_options::overwrite_existing);
        return destination;
    }

    void RemoveModuleCopy(const std::filesystem::path& modulePath)
    {
        std::error_code ec;
        std::filesystem::remove(SidecarPath(modulePath), ec);
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

    void SetGameKindEnvironment(bool enabled)
    {
#ifdef _WIN32
        _putenv_s("SPARK_MODULE_ABI_KIND_GAME", enabled ? "1" : "");
#else
        if (enabled)
            setenv("SPARK_MODULE_ABI_KIND_GAME", "1", 1);
        else
            unsetenv("SPARK_MODULE_ABI_KIND_GAME");
#endif
    }

    void SetVetoUnloadEnvironment(bool enabled)
    {
#ifdef _WIN32
        _putenv_s("SPARK_MODULE_ABI_VETO_UNLOAD", enabled ? "1" : "");
#else
        if (enabled)
            setenv("SPARK_MODULE_ABI_VETO_UNLOAD", "1", 1);
        else
            unsetenv("SPARK_MODULE_ABI_VETO_UNLOAD");
#endif
    }

#ifndef _WIN32
    size_t CountStagedModuleImages(const std::filesystem::path& source)
    {
        const std::string prefix = "spark-module-stage-" + std::to_string(static_cast<uint64_t>(::getpid())) + "-";
        size_t count = 0;
        std::error_code ec;
        const std::filesystem::path stagingRoot = std::filesystem::temp_directory_path();
        for (std::filesystem::directory_iterator it(stagingRoot, ec), end; !ec && it != end; it.increment(ec))
        {
            const std::string filename = it->path().filename().string();
            if (filename.starts_with(prefix) && std::filesystem::exists(it->path() / source.filename()))
                ++count;
        }
        return count;
    }
#endif
} // namespace

TEST(ModuleABI_ExpectedDescriptorIsCompatible)
{
    EXPECT_TRUE(Spark::CheckModuleCompatibility(&Spark::kExpectedModuleCompatibility) ==
                Spark::ModuleCompatibilityStatus::Compatible);

    auto mismatch = Spark::kExpectedModuleCompatibility;
    ++mismatch.sdkVersion;
    EXPECT_TRUE(Spark::CheckModuleCompatibility(&mismatch) == Spark::ModuleCompatibilityStatus::SDKVersionMismatch);
}

TEST(ModuleABI_LoadErrorIncludesRequestedPathAndLoaderStage)
{
    const std::filesystem::path missingPath =
        std::filesystem::temp_directory_path() / "spark-module-that-does-not-exist.invalid";
    std::error_code ec;
    std::filesystem::remove(missingPath, ec);

    ModuleManager manager;
    EXPECT_FALSE(manager.LoadModule(PathToUtf8(missingPath)));
    EXPECT_STR_CONTAINS(manager.GetLastLoadError(), PathToUtf8(missingPath));
#ifdef _WIN32
    EXPECT_STR_CONTAINS(manager.GetLastLoadError(), "validation");
#else
    EXPECT_STR_CONTAINS(manager.GetLastLoadError(), "stage");
#endif
}

TEST(ModuleABI_LoadErrorTracksDirectoryFailureAndClearsAfterSuccess)
{
    const std::filesystem::path missingDirectory =
        std::filesystem::temp_directory_path() / "spark-module-directory-that-does-not-exist";
    std::error_code ec;
    std::filesystem::remove_all(missingDirectory, ec);

    ModuleManager manager;
    EXPECT_FALSE(manager.LoadModulesFromDirectory(PathToUtf8(missingDirectory)));
    EXPECT_STR_CONTAINS(manager.GetLastLoadError(), PathToUtf8(missingDirectory));

    EXPECT_TRUE(manager.LoadModule(SPARK_TEST_COMPATIBLE_MODULE_PATH));
    EXPECT_TRUE(manager.GetLastLoadError().empty());
}

TEST(ModuleABI_FailedGameInitializationIsNotReportedAsUsable)
{
    SetGameKindEnvironment(true);
    SetFailOnLoadEnvironment(true);

    NullEngineContext context;
    ModuleManager manager;
    EXPECT_TRUE(manager.LoadModule(SPARK_TEST_COMPATIBLE_MODULE_PATH));
    EXPECT_FALSE(manager.GetGameModuleName().empty());
    manager.InitializeAll(&context);

    SetFailOnLoadEnvironment(false);
    SetGameKindEnvironment(false);
    EXPECT_TRUE(manager.GetInitializedGameModuleName().empty());
    manager.UnloadAll();
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

TEST(ModuleABI_ProjectDiscoveryAcceptsCompatibleSidecarWithoutLegacyNameHint)
{
    const std::filesystem::path modulePath = CopyCompatibleFixtureToTemp("FPSStarter");
    const std::string directory = PathToUtf8(modulePath.parent_path());

    const auto conservative = ModuleManager::DiscoverModuleCandidates(directory);
    EXPECT_FALSE(std::any_of(conservative.begin(), conservative.end(), [&](const std::string& candidate)
                             { return PathFromUtf8(candidate).filename() == modulePath.filename(); }));

    const auto projectCandidates =
        ModuleManager::DiscoverModuleCandidates(directory, ModuleManager::DiscoveryMode::CompatibleSidecars);
    EXPECT_TRUE(std::any_of(projectCandidates.begin(), projectCandidates.end(), [&](const std::string& candidate)
                            { return PathFromUtf8(candidate).filename() == modulePath.filename(); }));
    RemoveModuleCopy(modulePath);
}

TEST(ModuleABI_ManifestIgnoresPathKeysOutsideModulesArray)
{
    const std::filesystem::path modulePath = CopyCompatibleFixtureToTemp("SparkManifestMetadataPath");
    const std::filesystem::path manifestPath = modulePath.parent_path() / "spark.metadata-only.modules.json";
    {
        std::ofstream manifest(manifestPath, std::ios::trunc);
        manifest << "{ \"path\": \"" << PathToUtf8(modulePath.filename()) << "\", \"metadata\": { \"path\": \""
                 << PathToUtf8(modulePath.filename()) << "\" } }\n";
    }

    ModuleManager manager;
    EXPECT_FALSE(manager.LoadModulesFromManifest(PathToUtf8(manifestPath)));
    EXPECT_TRUE(manager.GetLoadedModuleInfo().empty());

    RemoveModuleCopy(modulePath);
    std::error_code ec;
    std::filesystem::remove(manifestPath, ec);
}

#ifdef _WIN32
TEST(ModuleABI_UnicodeManifestPathResolvesAndLoadsWithWideWindowsLoader)
{
    const std::filesystem::path unicodeDirectory = std::filesystem::path(L"Spark-ABI-Caf\u00e9-\u6e2c\u8a66");
    const std::filesystem::path modulePath = CopyCompatibleFixtureToTemp(unicodeDirectory / "FPSStarter");
    const std::filesystem::path manifestPath = modulePath.parent_path() / "spark.modules.json";
    {
        std::ofstream manifest(manifestPath, std::ios::trunc);
        manifest << "{ \"modules\": [{ \"path\": \"" << PathToUtf8(modulePath.filename()) << "\" }] }\n";
    }

    {
        Spark::LocalFileCache fileCache;
        ModuleManager manager;
        manager.SetFileCache(&fileCache);
        EXPECT_TRUE(manager.LoadModulesFromManifest(PathToUtf8(manifestPath)));
        EXPECT_EQ(manager.GetLoadedModuleInfo().size(), size_t{1});
        manager.UnloadAll();
    }

    RemoveModuleCopy(modulePath);
    std::error_code ec;
    std::filesystem::remove(manifestPath, ec);
    std::filesystem::remove(modulePath.parent_path(), ec);
}
#endif

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
    ASSERT_TRUE(manager.LoadModule(SPARK_TEST_COMPATIBLE_MODULE_PATH));

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

#ifndef _WIN32
TEST(ModuleABI_PosixLoadsVerifiedShadowAndCleansItAfterUnload)
{
    const std::filesystem::path modulePath = CopyCompatibleFixtureToTemp("SparkPosixStagedModule");
    EXPECT_EQ(CountStagedModuleImages(modulePath), size_t{0});

    ModuleManager manager;
    ASSERT_TRUE(manager.LoadModule(PathToUtf8(modulePath)));
    EXPECT_EQ(CountStagedModuleImages(modulePath), size_t{1});

    manager.UnloadAll();
    EXPECT_EQ(CountStagedModuleImages(modulePath), size_t{0});
    RemoveModuleCopy(modulePath);
}

TEST(ModuleABI_PosixLoadsFromReadOnlyInstallDirectory)
{
    const std::filesystem::path sourcePath = PathFromUtf8(SPARK_TEST_COMPATIBLE_MODULE_PATH);
    const std::filesystem::path installDirectory =
        std::filesystem::temp_directory_path() /
        ("spark-readonly-module-" + std::to_string(static_cast<uint64_t>(::getpid())));
    std::error_code ec;
    std::filesystem::remove_all(installDirectory, ec);
    std::filesystem::create_directory(installDirectory);
    const std::filesystem::path installedModule = installDirectory / sourcePath.filename();
    std::filesystem::copy_file(sourcePath, installedModule);
    std::filesystem::copy_file(SidecarPath(sourcePath), SidecarPath(installedModule));
    ASSERT_TRUE(::chmod(installDirectory.c_str(), S_IRUSR | S_IXUSR) == 0);

    ModuleManager manager;
    EXPECT_TRUE(manager.LoadModule(PathToUtf8(installedModule)));
    manager.UnloadAll();
    EXPECT_EQ(CountStagedModuleImages(installedModule), size_t{0});

    EXPECT_TRUE(::chmod(installDirectory.c_str(), S_IRWXU) == 0);
    std::filesystem::remove_all(installDirectory, ec);
}

TEST(ModuleABI_PosixPrivateStagePreservesSiblingDependencyResolution)
{
    ModuleManager manager;
    ASSERT_TRUE(manager.LoadModule(SPARK_TEST_SIBLING_DEPENDENT_MODULE_PATH));

    NullEngineContext context;
    manager.InitializeAll(&context);
    EXPECT_EQ(manager.GetInitializedModuleCount(), size_t{1});

    manager.ShutdownAll();
    manager.UnloadAll();
}
#endif

TEST(ModuleABI_FailedTransactionalReloadPreservesWorkingModule)
{
    const std::filesystem::path modulePath = CopyCompatibleFixtureToTemp("SparkReloadPreservationModule");
    NullEngineContext context;
    ModuleManager manager;
    ASSERT_TRUE(manager.LoadModule(modulePath.string()));
    manager.InitializeAll(&context);
#ifndef _WIN32
    EXPECT_EQ(CountStagedModuleImages(modulePath), size_t{1});
#endif

    Spark::IModule* const workingInstance = manager.GetModule("Spark Compatible ABI Fixture");
    EXPECT_TRUE(workingInstance != nullptr);

    // Make only the replacement metadata incompatible. The already loaded
    // working image remains valid and must not be unloaded on this failure.
    std::string sidecar;
    {
        std::ifstream input(modulePath.string() + ".sparkabi");
        sidecar.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    const std::string expectedSdk = "sdk_version=" + std::to_string(SPARK_SDK_VERSION);
    const size_t sdkField = sidecar.find(expectedSdk);
    EXPECT_TRUE(sdkField != std::string::npos);
    if (sdkField != std::string::npos)
        sidecar.replace(sdkField, expectedSdk.size(), "sdk_version=" + std::to_string(SPARK_SDK_VERSION + 1));
    {
        std::ofstream output(modulePath.string() + ".sparkabi", std::ios::trunc);
        output << sidecar;
    }

    EXPECT_FALSE(manager.ReloadModule("Spark Compatible ABI Fixture", &context));
    EXPECT_STR_CONTAINS(manager.GetLastLoadError(), "Spark Compatible ABI Fixture");
    EXPECT_STR_CONTAINS(manager.GetLastLoadError(), "staged replacement");
    EXPECT_TRUE(manager.GetModule("Spark Compatible ABI Fixture") == workingInstance);
    EXPECT_EQ(std::string(workingInstance->GetModuleInfo().name), std::string("Spark Compatible ABI Fixture"));

    manager.ShutdownAll();
    manager.UnloadAll();
    RemoveModuleCopy(modulePath);
}

TEST(ModuleABI_UnloadVetoPreservesInitializedWorkingModule)
{
    const std::filesystem::path modulePath = CopyCompatibleFixtureToTemp("SparkUnloadVetoModule");
    NullEngineContext context;
    ModuleManager manager;
    ASSERT_TRUE(manager.LoadModule(modulePath.string()));
    manager.InitializeAll(&context);

    Spark::IModule* const workingInstance = manager.GetModule("Spark Compatible ABI Fixture");
    SetVetoUnloadEnvironment(true);
    EXPECT_FALSE(manager.ShutdownAll());
    EXPECT_TRUE(manager.GetModule("Spark Compatible ABI Fixture") == workingInstance);
    EXPECT_FALSE(manager.ReloadModule("Spark Compatible ABI Fixture", &context));
    EXPECT_STR_CONTAINS(manager.GetLastLoadError(), "refused hot reload");
    EXPECT_TRUE(manager.GetModule("Spark Compatible ABI Fixture") == workingInstance);

    SetVetoUnloadEnvironment(false);
    EXPECT_TRUE(manager.ShutdownAll());
    manager.UnloadAll();
    RemoveModuleCopy(modulePath);
}

TEST(ModuleABI_CommittedShutdownDoesNotRepeatFalliblePreflight)
{
    const std::filesystem::path modulePath = CopyCompatibleFixtureToTemp("SparkCommittedShutdownModule");
    NullEngineContext context;
    ModuleManager manager;
    ASSERT_TRUE(manager.LoadModule(modulePath.string()));
    manager.InitializeAll(&context);

    SetVetoUnloadEnvironment(false);
    ASSERT_TRUE(manager.CanShutdownAll());
    // Once the owner commits shutdown, a later environmental change must not
    // strand a partially torn-down dependency graph behind a second gate.
    SetVetoUnloadEnvironment(true);
    manager.ShutdownAllAfterPreflight();
    manager.UnloadAll();
    SetVetoUnloadEnvironment(false);
    RemoveModuleCopy(modulePath);
}

TEST(ModuleABI_StartupRollbackTearsDownVetoingUncommittedModule)
{
    const std::filesystem::path modulePath = CopyCompatibleFixtureToTemp("SparkStartupRollbackModule");
    NullEngineContext context;
    ModuleManager manager;
    ASSERT_TRUE(manager.LoadModule(modulePath.string()));
    manager.InitializeAll(&context);

    SetVetoUnloadEnvironment(true);
    EXPECT_FALSE(manager.CanShutdownAll());
    manager.RollbackStartup();
    SetVetoUnloadEnvironment(false);

    manager.UnloadAll();
    EXPECT_EQ(manager.GetModuleCount(), size_t{0});
    RemoveModuleCopy(modulePath);
}

TEST(ModuleABI_FailedReplacementInitializationPreservesWorkingModule)
{
    const std::filesystem::path modulePath = CopyCompatibleFixtureToTemp("SparkReloadInitFailureModule");
    NullEngineContext context;
    ModuleManager manager;
    ASSERT_TRUE(manager.LoadModule(modulePath.string()));
    manager.InitializeAll(&context);

    Spark::IModule* const workingInstance = manager.GetModule("Spark Compatible ABI Fixture");
    EXPECT_TRUE(workingInstance != nullptr);

    SetFailOnLoadEnvironment(true);
    const bool reloadSucceeded = manager.ReloadModule("Spark Compatible ABI Fixture", &context);
    SetFailOnLoadEnvironment(false);

    EXPECT_FALSE(reloadSucceeded);
    EXPECT_STR_CONTAINS(manager.GetLastLoadError(), "initialization failed");
    EXPECT_TRUE(manager.GetModule("Spark Compatible ABI Fixture") == workingInstance);
    EXPECT_EQ(std::string(workingInstance->GetModuleInfo().name), std::string("Spark Compatible ABI Fixture"));

    manager.ShutdownAll();
    manager.UnloadAll();
    RemoveModuleCopy(modulePath);
}

#ifdef _WIN32
TEST(ModuleABI_WindowsReloadCleanupPreservesModuleParentDirectory)
{
    const std::filesystem::path sourcePath = PathFromUtf8(SPARK_TEST_COMPATIBLE_MODULE_PATH);
    const std::filesystem::path moduleDirectory =
        std::filesystem::temp_directory_path() / "SparkWindowsReloadParentPreservation";
    const std::filesystem::path modulePath = moduleDirectory / sourcePath.filename();
    const std::filesystem::path sentinelPath = moduleDirectory / "parent-directory-sentinel.txt";

    std::error_code ec;
    std::filesystem::remove_all(moduleDirectory, ec);
    ASSERT_TRUE(std::filesystem::create_directories(moduleDirectory));
    std::filesystem::copy_file(sourcePath, modulePath, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(SidecarPath(sourcePath), SidecarPath(modulePath),
                               std::filesystem::copy_options::overwrite_existing);
    {
        std::ofstream sentinel(sentinelPath, std::ios::trunc);
        sentinel << "must survive module shadow cleanup";
    }

    NullEngineContext context;
    ModuleManager manager;
    ASSERT_TRUE(manager.LoadModule(PathToUtf8(modulePath)));
    manager.InitializeAll(&context);
    ASSERT_TRUE(manager.ReloadModule("Spark Compatible ABI Fixture", &context));
    EXPECT_TRUE(manager.ShutdownAll());
    manager.UnloadAll();

    EXPECT_TRUE(std::filesystem::is_directory(moduleDirectory));
    EXPECT_TRUE(std::filesystem::is_regular_file(sentinelPath));
    std::filesystem::remove_all(moduleDirectory, ec);
}
#endif

TEST(ModuleABI_HotReloadCallbackCanReenterManagerWithoutDeadlock)
{
    const std::filesystem::path modulePath = CopyCompatibleFixtureToTemp("SparkReentrantReloadModule");
    NullEngineContext context;
    ModuleManager manager;
    ASSERT_TRUE(manager.LoadModule(modulePath.string()));
    manager.InitializeAll(&context);

    EXPECT_FALSE(manager.ReloadModule("Missing Module", &context));
    EXPECT_STR_CONTAINS(manager.GetLastLoadError(), "Missing Module");

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
    EXPECT_TRUE(manager.GetLastLoadError().empty());
    EXPECT_TRUE(callbackRan);
    EXPECT_TRUE(statusFromCallback.find("Reloads:  1") != std::string::npos);
    EXPECT_EQ(hotReload.GetReloadCount(), 1);
#ifndef _WIN32
    EXPECT_EQ(CountStagedModuleImages(modulePath), size_t{1});
#endif

    manager.ShutdownAll();
    manager.UnloadAll();
#ifndef _WIN32
    EXPECT_EQ(CountStagedModuleImages(modulePath), size_t{0});
#endif
    RemoveModuleCopy(modulePath);
}

#include "TestFramework.h"

#include "Fixtures/ScopedUnboundedFileSize.h"

#include "Core/ModuleHotReload.h"
#include "Core/ModuleManager.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Utils/InvalidStateDetector.h"
#include "Utils/SparkConsole.h"
#include <Spark/ModuleABI.h>
#include <Spark/Version.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef SPARK_TEST_MISMATCHED_MODULE_PATH
#error SPARK_TEST_MISMATCHED_MODULE_PATH must name the mismatched module fixture
#endif

#ifndef SPARK_TEST_COMPATIBLE_MODULE_PATH
#error SPARK_TEST_COMPATIBLE_MODULE_PATH must name the compatible module fixture
#endif

#ifndef SPARK_TEST_REGISTRY_LIFECYCLE_MODULE_PATH
#error SPARK_TEST_REGISTRY_LIFECYCLE_MODULE_PATH must name the registry lifecycle module fixture
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
        explicit NullEngineContext(Spark::SaveSystem* saveSystem = nullptr, Spark::WeatherSystem* weather = nullptr,
                                   Spark::UI::UISystem* ui = nullptr, Spark::DialogueSystem* dialogue = nullptr,
                                   Spark::ModSystem* mods = nullptr)
            : m_saveSystem(saveSystem), m_weather(weather), m_ui(ui), m_dialogue(dialogue), m_mods(mods)
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
        AudioEngine* GetAudio() override { return nullptr; }
        const AudioEngine* GetAudio() const override { return nullptr; }
        PhysicsSystem* GetPhysics() override { return nullptr; }
        const PhysicsSystem* GetPhysics() const override { return nullptr; }
        Spark::SaveSystem* GetSaveSystem() override { return m_saveSystem; }
        const Spark::SaveSystem* GetSaveSystem() const override { return m_saveSystem; }
        // The save capability comes with the registry the host SaveSystem reads.
        Spark::ComponentSerializerRegistry* GetComponentSerializers() override
        {
            return m_saveSystem ? &Spark::ComponentSerializerRegistry::GetInstance() : nullptr;
        }
        const Spark::ComponentSerializerRegistry* GetComponentSerializers() const override
        {
            return m_saveSystem ? &Spark::ComponentSerializerRegistry::GetInstance() : nullptr;
        }
        Spark::WeatherSystem* GetWeather() override { return m_weather; }
        const Spark::WeatherSystem* GetWeather() const override { return m_weather; }
        Spark::UI::UISystem* GetUI() override { return m_ui; }
        const Spark::UI::UISystem* GetUI() const override { return m_ui; }
        Spark::DialogueSystem* GetDialogue() override { return m_dialogue; }
        const Spark::DialogueSystem* GetDialogue() const override { return m_dialogue; }
        Spark::ModSystem* GetModSystem() override { return m_mods; }
        const Spark::ModSystem* GetModSystem() const override { return m_mods; }
        uint32_t GetEngineVersion() const override { return SPARK_ENGINE_VERSION_PACKED; }
        uint32_t GetSDKVersion() const override { return SPARK_SDK_VERSION; }

      private:
        Spark::SaveSystem* m_saveSystem = nullptr;
        Spark::WeatherSystem* m_weather = nullptr;
        Spark::UI::UISystem* m_ui = nullptr;
        Spark::DialogueSystem* m_dialogue = nullptr;
        Spark::ModSystem* m_mods = nullptr;
    };

    /**
     * @brief This process's scratch root under the system temp directory.
     *
     * Fixed %TEMP% names collided with any other SparkTests process: a module
     * whose OnLoad fails stays mapped until its process exits, so a concurrent or
     * hung run held the shared copy open and this run's copy_file failed. File
     * names below the root are unchanged (module names derive from them).
     */
    std::filesystem::path ProcessScratchRoot()
    {
#ifdef _WIN32
        const unsigned long processId = static_cast<unsigned long>(GetCurrentProcessId());
#else
        const unsigned long processId = static_cast<unsigned long>(::getpid());
#endif
        const std::filesystem::path root =
            std::filesystem::temp_directory_path() / ("SparkModuleABI-" + std::to_string(processId));
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        return root;
    }

    std::filesystem::path CopyCompatibleFixtureToTemp(const std::filesystem::path& stem,
                                                      std::string_view fixture = SPARK_TEST_COMPATIBLE_MODULE_PATH)
    {
        const std::filesystem::path sourcePath = PathFromUtf8(fixture);
        std::filesystem::path destination = ProcessScratchRoot() / stem;
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

    void SetTestEnvironment(const char* name, const std::string& value)
    {
#ifdef _WIN32
        _putenv_s(name, value.c_str());
#else
        if (value.empty())
            unsetenv(name);
        else
            setenv(name, value.c_str(), 1);
#endif
    }

    /**
     * @brief Sets one fixture switch for a scope and restores its previous value on exit.
     *
     * The fixtures read process-global switches, so a raw set/reset pair leaked
     * the value into every later test (and --shuffle reorders them) whenever an
     * ASSERT return or an exception skipped the reset. Set() retoggles within
     * the scope; an empty value unsets the variable.
     */
    class ScopedTestEnvironment final
    {
      public:
        ScopedTestEnvironment(const char* variable, const std::string& value) : m_name(variable)
        {
            if (const char* existing = std::getenv(variable))
            {
                m_hadValue = true;
                m_previous = existing;
            }
            Set(value);
        }
        ~ScopedTestEnvironment() { Set(m_hadValue ? m_previous : std::string{}); }
        ScopedTestEnvironment(const ScopedTestEnvironment&) = delete;
        ScopedTestEnvironment& operator=(const ScopedTestEnvironment&) = delete;

        void Set(const std::string& value) const { SetTestEnvironment(m_name, value); }

      private:
        const char* m_name;
        bool m_hadValue = false;
        std::string m_previous;
    };

    std::string ReadBinaryFile(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    void WriteBinaryFile(const std::filesystem::path& path, const std::string& content)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << content;
    }

    /// Initializes the host console for fixtures that register commands and removes their entries on exit.
    struct RegistryFixtureHostGuard final
    {
        Spark::SimpleConsole& console = Spark::SimpleConsole::GetInstance();
        Spark::InvalidStateDetector& detector = Spark::InvalidStateDetector::GetInstance();
        bool restoreUninitialized = !console.IsInitialized();

        RegistryFixtureHostGuard()
        {
            if (restoreUninitialized)
                console.Initialize();
            Clear();
        }
        ~RegistryFixtureHostGuard()
        {
            Clear();
            if (restoreUninitialized)
                console.Shutdown();
        }
        RegistryFixtureHostGuard(const RegistryFixtureHostGuard&) = delete;
        RegistryFixtureHostGuard& operator=(const RegistryFixtureHostGuard&) = delete;

        void Clear()
        {
            detector.RemoveRulesByCategory("RegistryFixture");
            console.UnregisterCommand("registry_fixture_status");
        }
    };

    /// True when @p address lies inside a module image that is still mapped. Executes nothing there.
    bool IsAddressInMappedImage(std::uintptr_t address)
    {
#ifdef _WIN32
        HMODULE module = nullptr;
        return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCWSTR>(address), &module) != FALSE &&
               module != nullptr;
#else
        Dl_info info{};
        return dladdr(reinterpret_cast<const void*>(address), &info) != 0 && info.dli_fname != nullptr;
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
    const std::filesystem::path missingPath = ProcessScratchRoot() / "spark-module-that-does-not-exist.invalid";
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
    const std::filesystem::path missingDirectory = ProcessScratchRoot() / "spark-module-directory-that-does-not-exist";
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
    const ScopedTestEnvironment gameKind("SPARK_MODULE_ABI_KIND_GAME", "1");
    const ScopedTestEnvironment failOnLoad("SPARK_MODULE_ABI_FAIL_ON_LOAD", "1");

    NullEngineContext context;
    ModuleManager manager;
    EXPECT_TRUE(manager.LoadModule(SPARK_TEST_COMPATIBLE_MODULE_PATH));
    EXPECT_FALSE(manager.GetGameModuleName().empty());
    manager.InitializeAll(&context);

    failOnLoad.Set("");
    gameKind.Set("");
    EXPECT_TRUE(manager.GetInitializedGameModuleName().empty());

    // The failed entry survives only to keep its DLL mapped. It must not be
    // reported as the process's game module (that refused every replacement)
    // and must not make the windowed loop take the module branch, which skips
    // both the module's rendering and the engine-only present path.
    EXPECT_TRUE(manager.GetGameModuleName().empty());
    EXPECT_FALSE(manager.HasInitializedModules());
    EXPECT_TRUE(manager.HasModules());

    manager.UnloadAll();
}

TEST(ModuleABI_DiscoveryDoesNotExecuteCandidate)
{
    const std::filesystem::path fixturePath = SPARK_TEST_MISMATCHED_MODULE_PATH;
    const std::filesystem::path sentinelPath = ProcessScratchRoot() / "spark-module-abi-discovery-sentinel.txt";
    std::error_code ec;
    std::filesystem::remove(sentinelPath, ec);
    const ScopedTestEnvironment sentinel("SPARK_MODULE_ABI_SENTINEL", sentinelPath.string());

    ModuleManager manager;
    const auto discovered = manager.DiscoverModules(fixturePath.parent_path().string());
    const bool found = std::any_of(discovered.begin(), discovered.end(), [&](const DiscoveredModule& module)
                                   { return std::filesystem::path(module.path).filename() == fixturePath.filename(); });

    sentinel.Set("");
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
        ModuleManager manager;
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
    const std::filesystem::path sentinelPath = ProcessScratchRoot() / "spark-module-abi-load-sentinel.txt";
    std::error_code ec;
    std::filesystem::remove(sentinelPath, ec);
    const ScopedTestEnvironment sentinel("SPARK_MODULE_ABI_SENTINEL", sentinelPath.string());

    ModuleManager manager;
    const bool loaded = manager.LoadModule(fixturePath.string());

    sentinel.Set("");
    EXPECT_FALSE(loaded);
    EXPECT_FALSE(std::filesystem::exists(sentinelPath));
    EXPECT_TRUE(manager.GetLoadedModuleInfo().empty());
}

TEST(ModuleABI_ModifiedBinaryRejectedByHashBeforeDllMainOrStaticConstructor)
{
    const std::filesystem::path fixturePath = SPARK_TEST_COMPATIBLE_MODULE_PATH;
    const std::filesystem::path copiedPath =
        ProcessScratchRoot() / ("SparkCompatibleHashTampered" + fixturePath.extension().string());
    const std::filesystem::path copiedSidecar = copiedPath.string() + ".sparkabi";
    const std::filesystem::path sentinelPath = ProcessScratchRoot() / "spark-module-abi-hash-sentinel.txt";
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

    const ScopedTestEnvironment sentinel("SPARK_MODULE_ABI_SENTINEL", sentinelPath.string());
    ModuleManager manager;
    const bool loaded = manager.LoadModule(copiedPath.string());
    sentinel.Set("");

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
    const ScopedTestEnvironment vetoUnload("SPARK_MODULE_ABI_VETO_UNLOAD", "1");
    EXPECT_FALSE(manager.ShutdownAll());
    EXPECT_TRUE(manager.GetModule("Spark Compatible ABI Fixture") == workingInstance);
    EXPECT_FALSE(manager.ReloadModule("Spark Compatible ABI Fixture", &context));
    EXPECT_STR_CONTAINS(manager.GetLastLoadError(), "refused hot reload");
    EXPECT_TRUE(manager.GetModule("Spark Compatible ABI Fixture") == workingInstance);

    vetoUnload.Set("");
    EXPECT_TRUE(manager.ShutdownAll());
    manager.UnloadAll();
    RemoveModuleCopy(modulePath);
}

TEST(ModuleABI_ReplacementHotReloadVetoPreservesInitializedWorkingModule)
{
    const std::filesystem::path modulePath = CopyCompatibleFixtureToTemp("SparkReplacementHotReloadVetoModule");
    NullEngineContext context;
    ModuleManager manager;
    ASSERT_TRUE(manager.LoadModule(modulePath.string()));
    manager.InitializeAll(&context);

    Spark::IModule* const workingInstance = manager.GetModule("Spark Compatible ABI Fixture");
    ASSERT_TRUE(workingInstance != nullptr);

    // The already-loaded image captured the allow decision at construction.
    // Only the staged replacement sees this veto, so the test covers the
    // replacement-side contract rather than the existing-image preflight.
    bool reloadSucceeded = false;
    {
        const ScopedTestEnvironment vetoHotReload("SPARK_MODULE_ABI_VETO_HOT_RELOAD", "1");
        reloadSucceeded = manager.ReloadModule("Spark Compatible ABI Fixture", &context);
    }

    EXPECT_FALSE(reloadSucceeded);
    EXPECT_STR_CONTAINS(manager.GetLastLoadError(), "replacement");
    EXPECT_STR_CONTAINS(manager.GetLastLoadError(), "hot reload");
    EXPECT_TRUE(manager.GetModule("Spark Compatible ABI Fixture") == workingInstance);
    EXPECT_TRUE(manager.HasInitializedModules());

    manager.ShutdownAll();
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

    const ScopedTestEnvironment vetoUnload("SPARK_MODULE_ABI_VETO_UNLOAD", "");
    ASSERT_TRUE(manager.CanShutdownAll());
    // Once the owner commits shutdown, a later environmental change must not
    // strand a partially torn-down dependency graph behind a second gate.
    vetoUnload.Set("1");
    manager.ShutdownAllAfterPreflight();
    manager.UnloadAll();
    vetoUnload.Set("");
    RemoveModuleCopy(modulePath);
}

TEST(ModuleABI_StartupRollbackTearsDownVetoingUncommittedModule)
{
    const std::filesystem::path modulePath = CopyCompatibleFixtureToTemp("SparkStartupRollbackModule");
    NullEngineContext context;
    ModuleManager manager;
    ASSERT_TRUE(manager.LoadModule(modulePath.string()));
    manager.InitializeAll(&context);

    const ScopedTestEnvironment vetoUnload("SPARK_MODULE_ABI_VETO_UNLOAD", "1");
    EXPECT_FALSE(manager.CanShutdownAll());
    manager.RollbackStartup();
    vetoUnload.Set("");

    manager.UnloadAll();
    EXPECT_EQ(manager.GetModuleCount(), size_t{0});
    RemoveModuleCopy(modulePath);
}

TEST(ModuleABI_AllValidationRuleOwnersReleaseCallbacksBeforeUnload)
{
    struct ModuleContract
    {
        const char* directory;
        const char* category;
    };
    constexpr ModuleContract contracts[] = {{"SparkGame", "Base"},
                                            {"SparkGameARPG", "ARPG"},
                                            {"SparkGameFPS", "FPS"},
                                            {"SparkGameMMO", "MMO"},
                                            {"SparkGameOpenWorld", "OpenWorld"},
                                            {"SparkGamePlatformer", "Platformer"},
                                            {"SparkGameRacing", "Racing"},
                                            {"SparkGameRPG", "RPG"},
                                            {"SparkGameRTS", "RTS"},
                                            {"SparkGameVisualScript", "VisualScript"}};

    const auto sourceRoot = std::filesystem::path(SPARK_TEST_SOURCE_DIR) / "GameModules";
    for (const auto& contract : contracts)
    {
        const auto moduleSource = sourceRoot / contract.directory / "Source" / "Core" / "Main.cpp";
        std::ifstream stream(moduleSource, std::ios::binary);
        EXPECT_TRUE(stream.is_open());
        if (!stream)
            continue;

        const std::string source{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
        const std::string cleanup = "RemoveRulesByCategory(\"" + std::string(contract.category) + "\")";
        const auto unloadStart = source.find("::OnUnload()");
        ASSERT_TRUE(unloadStart != std::string::npos);
        const auto unloadBodyStart = source.find('{', unloadStart);
        ASSERT_TRUE(unloadBodyStart != std::string::npos);
        const auto findBodyEnd = [&source](size_t bodyStart)
        {
            size_t braceDepth = 0;
            for (size_t cursor = bodyStart; cursor < source.size(); ++cursor)
            {
                if (source[cursor] == '{')
                    ++braceDepth;
                else if (source[cursor] == '}' && --braceDepth == 0)
                    return cursor;
            }
            return std::string::npos;
        };
        const auto unloadBodyEnd = findBodyEnd(unloadBodyStart);
        ASSERT_TRUE(unloadBodyEnd != std::string::npos);
        const auto cleanupPosition = source.find(cleanup, unloadBodyStart);
        if (cleanupPosition < unloadBodyEnd)
            continue;

        // A module may centralize teardown in Shutdown(), provided OnUnload()
        // delegates to that exact owner method before its image is unmapped.
        const auto shutdownCall = source.find("Shutdown()", unloadBodyStart);
        ASSERT_TRUE(shutdownCall < unloadBodyEnd);
        const auto ownerStart = source.rfind("void ", unloadStart);
        ASSERT_TRUE(ownerStart != std::string::npos);
        const auto ownerNameStart = ownerStart + std::string_view("void ").size();
        const auto ownerName = source.substr(ownerNameStart, unloadStart - ownerNameStart);
        const auto shutdownStart = source.find(ownerName + "::Shutdown()", unloadBodyEnd);
        ASSERT_TRUE(shutdownStart != std::string::npos);
        const auto shutdownBodyStart = source.find('{', shutdownStart);
        ASSERT_TRUE(shutdownBodyStart != std::string::npos);
        const auto shutdownBodyEnd = findBodyEnd(shutdownBodyStart);
        ASSERT_TRUE(shutdownBodyEnd != std::string::npos);
        const auto delegatedCleanup = source.find(cleanup, shutdownBodyStart);
        EXPECT_TRUE(delegatedCleanup < shutdownBodyEnd);
    }
}

#if !defined(_WIN32) && defined(SPARK_TEST_SPARK_GAME_MODULE_PATH)
TEST(ModuleABI_SparkGameShutdownReleasesHostRegistryCallbacksBeforeUnload)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    const std::string hostSentinel = "__spark_module_abi_unrelated_host_command__";
    const bool consoleWasInitialized = console.IsInitialized();
    struct ConsoleStateGuard final
    {
        Spark::SimpleConsole& console;
        const std::string& sentinel;
        bool restoreUninitialized;
        bool sentinelRegistered = false;
        ~ConsoleStateGuard()
        {
            if (sentinelRegistered)
                console.UnregisterCommand(sentinel);
            if (restoreUninitialized)
                console.Shutdown();
        }
    } consoleState{console, hostSentinel, !consoleWasInitialized};
    if (!consoleWasInitialized)
        ASSERT_TRUE(console.Initialize());
    ASSERT_FALSE(console.HasCommand(hostSentinel));
    ASSERT_FALSE(console.HasCommand("showcase_status"));
    ASSERT_FALSE(console.HasCommand("showcase_weather"));
    ASSERT_FALSE(console.HasCommand("showcase_save"));
    ASSERT_FALSE(console.HasCommand("showcase_load"));
    ASSERT_FALSE(console.HasCommand("showcase_spawn"));
    console.RegisterCommand(hostSentinel, [](const std::vector<std::string>&) { return std::string("host"); });
    consoleState.sentinelRegistered = true;
    ASSERT_TRUE(console.HasCommand(hostSentinel));

    auto& serializers = Spark::ComponentSerializerRegistry::GetInstance();
    auto& detector = Spark::InvalidStateDetector::GetInstance();
    auto priorTagRegistration = serializers.TakeRegistration("TagComponent");
    struct SerializerStateGuard final
    {
        Spark::ComponentSerializerRegistry& serializers;
        Spark::ComponentSerializerRegistry::RegistrationHandle prior;
        ~SerializerStateGuard()
        {
            serializers.Unregister("TagComponent");
            (void)serializers.RestoreRegistration(std::move(prior));
        }
    } serializerState{serializers, std::move(priorTagRegistration)};
    ASSERT_FALSE(detector.HasRule("Base.HealthInvariant"));
    const uint32_t initialRuleCount = detector.GetRuleCount();

    // GameplayShowcase intentionally registers serializers only when the host
    // exposes a SaveSystem. Supply that real capability so this test exercises
    // registration and, critically, removal before the module image unloads.
    auto& saveSystem = Spark::SaveSystem::GetInstance();
    NullEngineContext context(&saveSystem);
    // ModuleManager stages a copy of the image, and the ASan Debug libSparkGame exceeds the
    // sanitizer wrapper's 16 MiB file-size cap (Fixtures/ScopedUnboundedFileSize.h).
    const SparkTestFixtures::ScopedUnboundedFileSize fileSizeLimit;
    ModuleManager manager;
    struct ModuleManagerGuard final
    {
        ModuleManager& manager;
        ~ModuleManagerGuard()
        {
            manager.ShutdownAllAfterPreflight();
            manager.UnloadAll();
        }
    } managerGuard{manager};
    ASSERT_TRUE(manager.LoadModule(SPARK_TEST_SPARK_GAME_MODULE_PATH));
    manager.InitializeAll(&context);

    ASSERT_TRUE(manager.GetModule("Spark Default - Engine Showcase") != nullptr);
    EXPECT_TRUE(serializers.HasSerializer("TagComponent"));
    EXPECT_EQ(detector.GetRuleCount(), initialRuleCount + 1);
    EXPECT_TRUE(console.HasCommand("showcase_status"));
    EXPECT_TRUE(console.HasCommand("showcase_weather"));
    EXPECT_TRUE(console.HasCommand("showcase_save"));
    EXPECT_TRUE(console.HasCommand("showcase_load"));
    EXPECT_TRUE(console.HasCommand("showcase_spawn"));

    manager.ShutdownAllAfterPreflight();
    EXPECT_FALSE(serializers.HasSerializer("TagComponent"));
    EXPECT_EQ(detector.GetRuleCount(), initialRuleCount);
    EXPECT_FALSE(console.HasCommand("showcase_status"));
    EXPECT_FALSE(console.HasCommand("showcase_weather"));
    EXPECT_FALSE(console.HasCommand("showcase_save"));
    EXPECT_FALSE(console.HasCommand("showcase_load"));
    EXPECT_FALSE(console.HasCommand("showcase_spawn"));
    EXPECT_TRUE(console.HasCommand(hostSentinel));

    manager.UnloadAll();
    EXPECT_EQ(manager.GetModuleCount(), size_t{0});
}
#endif

TEST(ModuleABI_ReloadPreservesHostRegistryCallbacks)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    auto& detector = Spark::InvalidStateDetector::GetInstance();
    const std::string commandName = "registry_fixture_status";
    const std::string ruleCategory = "RegistryFixture";
    const bool consoleWasInitialized = console.IsInitialized();
    struct RegistryStateGuard final
    {
        Spark::SimpleConsole& console;
        Spark::InvalidStateDetector& detector;
        bool restoreUninitialized;
        ~RegistryStateGuard()
        {
            detector.RemoveRulesByCategory("RegistryFixture");
            console.UnregisterCommand("registry_fixture_status");
            if (restoreUninitialized)
                console.Shutdown();
        }
    } registryState{console, detector, !consoleWasInitialized};

    if (!consoleWasInitialized)
        ASSERT_TRUE(console.Initialize());
    detector.RemoveRulesByCategory(ruleCategory);
    ASSERT_FALSE(console.HasCommand(commandName));

    const uint32_t initialRuleCount = detector.GetRuleCount();
    NullEngineContext context;
    ModuleManager manager;
    ASSERT_TRUE(manager.LoadModule(SPARK_TEST_REGISTRY_LIFECYCLE_MODULE_PATH));
    ASSERT_TRUE(manager.InitializeAll(&context));
    ASSERT_TRUE(console.HasCommand(commandName));
    EXPECT_EQ(detector.GetRuleCount(), initialRuleCount + 1);

    // The replacement is initialized before the outgoing image's OnUnload.
    // Its command and rule must survive that handoff and remain owned by the
    // live replacement rather than being removed by the outgoing instance.
    ASSERT_TRUE(manager.ReloadModule("Spark Registry Lifecycle Fixture", &context));
    EXPECT_TRUE(console.HasCommand(commandName));
    EXPECT_EQ(detector.GetRuleCount(), initialRuleCount + 1);

    ASSERT_TRUE(manager.ShutdownAll());
    EXPECT_FALSE(console.HasCommand(commandName));
    EXPECT_EQ(detector.GetRuleCount(), initialRuleCount);
    manager.UnloadAll();
}

TEST(ModuleABI_OnUnloadSeesEngineServicesBeforeImageTeardown)
{
    const std::filesystem::path sentinel = ProcessScratchRoot() / "SparkRegistryLifecycleServicesAlive.txt";
    std::error_code cleanupError;
    std::filesystem::remove(sentinel, cleanupError);
    const ScopedTestEnvironment sentinelSwitch("SPARK_REGISTRY_FIXTURE_LIFECYCLE_SENTINEL", sentinel.string());

    int serviceStorage = 0;
    NullEngineContext context(nullptr, reinterpret_cast<Spark::WeatherSystem*>(&serviceStorage),
                              reinterpret_cast<Spark::UI::UISystem*>(&serviceStorage),
                              reinterpret_cast<Spark::DialogueSystem*>(&serviceStorage),
                              reinterpret_cast<Spark::ModSystem*>(&serviceStorage));
    ModuleManager manager;
    ASSERT_TRUE(manager.LoadModule(SPARK_TEST_REGISTRY_LIFECYCLE_MODULE_PATH));
    ASSERT_TRUE(manager.InitializeAll(&context));
    ASSERT_TRUE(manager.ShutdownAll());

    std::ifstream marker(sentinel);
    ASSERT_TRUE(marker.good());
    std::string result;
    std::getline(marker, result);
    EXPECT_EQ(result, "services_alive");

    // Exercise a second complete cycle on the same process. UnloadEntry must
    // clear the image-local injected EngineContext before FreeLibrary/dlclose.
    manager.UnloadAll();
    ASSERT_TRUE(manager.LoadModule(SPARK_TEST_REGISTRY_LIFECYCLE_MODULE_PATH));
    ASSERT_TRUE(manager.InitializeAll(&context));
    ASSERT_TRUE(manager.ShutdownAll());
    manager.UnloadAll();
    std::filesystem::remove(sentinel, cleanupError);
}

TEST(ModuleABI_ThrownOnLoadCleansPartialHostRegistryState)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    auto& detector = Spark::InvalidStateDetector::GetInstance();
    const std::string commandName = "registry_fixture_status";
    const std::string ruleCategory = "RegistryFixture";
    const bool consoleWasInitialized = console.IsInitialized();
    struct RegistryStateGuard final
    {
        Spark::SimpleConsole& console;
        Spark::InvalidStateDetector& detector;
        bool restoreUninitialized;
        ~RegistryStateGuard()
        {
            detector.RemoveRulesByCategory("RegistryFixture");
            console.UnregisterCommand("registry_fixture_status");
            if (restoreUninitialized)
                console.Shutdown();
        }
    } registryState{console, detector, !consoleWasInitialized};

    if (!consoleWasInitialized)
        ASSERT_TRUE(console.Initialize());
    detector.RemoveRulesByCategory(ruleCategory);
    console.UnregisterCommand(commandName);
    ASSERT_FALSE(console.HasCommand(commandName));

    const uint32_t initialRuleCount = detector.GetRuleCount();
    NullEngineContext context;
    ModuleManager manager;
    ASSERT_TRUE(manager.LoadModule(SPARK_TEST_REGISTRY_LIFECYCLE_MODULE_PATH));

    bool initializeResult = true;
    {
        const ScopedTestEnvironment throwOnLoad("SPARK_REGISTRY_FIXTURE_THROW_ON_LOAD", "1");
        EXPECT_NO_THROW(initializeResult = manager.InitializeAll(&context));
    }

    EXPECT_FALSE(initializeResult);
    EXPECT_FALSE(manager.HasInitializedModules());
    EXPECT_TRUE(manager.GetModule("Spark Registry Lifecycle Fixture") == nullptr);
    EXPECT_FALSE(console.HasCommand(commandName));
    EXPECT_EQ(detector.GetRuleCount(), initialRuleCount);

    manager.UnloadAll();
    EXPECT_EQ(manager.GetModuleCount(), size_t{0});
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

    bool reloadSucceeded = false;
    {
        const ScopedTestEnvironment failOnLoad("SPARK_MODULE_ABI_FAIL_ON_LOAD", "1");
        reloadSucceeded = manager.ReloadModule("Spark Compatible ABI Fixture", &context);
    }

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
    const std::filesystem::path moduleDirectory = ProcessScratchRoot() / "SparkWindowsReloadParentPreservation";
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

TEST(ModuleABI_OversizedSidecarRejectedBeforeOSLoad)
{
    const std::filesystem::path modulePath = CopyCompatibleFixtureToTemp("SparkOversizedSidecarModule");
    const std::filesystem::path sidecarPath = SidecarPath(modulePath);
    const std::string sidecar = ReadBinaryFile(sidecarPath);
    const std::string expectedSdk = "sdk_version=" + std::to_string(SPARK_SDK_VERSION);
    const size_t sdkField = sidecar.find(expectedSdk);
    ASSERT_TRUE(sdkField != std::string::npos);

    // Zero padding parses to the same integer, so only the per-field budget
    // rejects this value; the image itself still matches binary_sha256.
    std::string paddedValue = sidecar;
    paddedValue.replace(sdkField, expectedSdk.size(),
                        "sdk_version=" + std::string(100, '0') + std::to_string(SPARK_SDK_VERSION));
    WriteBinaryFile(sidecarPath, paddedValue);
    {
        ModuleManager manager;
        EXPECT_FALSE(manager.LoadModule(PathToUtf8(modulePath)));
        EXPECT_STR_CONTAINS(manager.GetLastLoadError(), "oversized ABI sidecar field");
        EXPECT_TRUE(manager.GetLoadedModuleInfo().empty());
    }

    // A file past the byte budget is refused before any line is parsed.
    WriteBinaryFile(sidecarPath, sidecar + "padding=" + std::string(8192, 'x') + "\n");
    {
        ModuleManager manager;
        EXPECT_FALSE(manager.LoadModule(PathToUtf8(modulePath)));
        EXPECT_STR_CONTAINS(manager.GetLastLoadError(), "exceeds 4096 bytes");
        EXPECT_TRUE(manager.GetLoadedModuleInfo().empty());
    }

    // The real sidecar fits both budgets.
    WriteBinaryFile(sidecarPath, sidecar);
    {
        ModuleManager manager;
        EXPECT_TRUE(manager.LoadModule(PathToUtf8(modulePath)));
        manager.UnloadAll();
    }
    RemoveModuleCopy(modulePath);
}

TEST(ModuleABI_OversizedManifestRejectedBeforeParse)
{
    const std::filesystem::path modulePath = CopyCompatibleFixtureToTemp("SparkOversizedManifestModule");
    const std::filesystem::path manifestPath = modulePath.parent_path() / "spark.oversized.modules.json";
    {
        // Valid JSON naming a loadable module; only the whitespace padding
        // pushes it past the manifest budget.
        std::ofstream manifest(manifestPath, std::ios::binary | std::ios::trunc);
        manifest << "{ \"modules\": [{ \"path\": \"" << PathToUtf8(modulePath.filename()) << "\" }] }"
                 << std::string(std::size_t{1024} * 1024, ' ') << "\n";
    }

    {
        ModuleManager manager;
        EXPECT_FALSE(manager.LoadModulesFromManifest(PathToUtf8(manifestPath)));
        EXPECT_STR_CONTAINS(manager.GetLastLoadError(), "exceeds");
        EXPECT_TRUE(manager.GetLoadedModuleInfo().empty());
        manager.UnloadAll();
    }

    RemoveModuleCopy(modulePath);
    std::error_code ec;
    std::filesystem::remove(manifestPath, ec);
}

TEST(ModuleABI_NonRegularManifestRejectedBeforeRead)
{
    // A manifest that is not a regular file has no size to budget. Reading it
    // anyway meant an unbounded read (a symlink to /dev/zero) or an open that
    // blocks forever (a FIFO with no writer). It must be refused before open.
    const std::filesystem::path scratch = ProcessScratchRoot() / "SparkNonRegularManifest";
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(scratch / "spark.directory.modules.json", ec);
    ASSERT_FALSE(ec);

    std::vector<std::filesystem::path> manifests{scratch / "spark.directory.modules.json"};
#ifndef _WIN32
    const std::filesystem::path zeroLink = scratch / "spark.zero.modules.json";
    std::filesystem::create_symlink("/dev/zero", zeroLink, ec);
    ASSERT_FALSE(ec);
    manifests.push_back(zeroLink);

    const std::filesystem::path fifo = scratch / "spark.fifo.modules.json";
    ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
    manifests.push_back(fifo);
#endif

    for (const std::filesystem::path& manifestPath : manifests)
    {
        ModuleManager manager;
        EXPECT_FALSE(manager.LoadModulesFromManifest(PathToUtf8(manifestPath)));
        EXPECT_STR_CONTAINS(manager.GetLastLoadError(), "regular file");
        EXPECT_EQ(manager.GetModuleCount(), size_t{0});
    }

    std::filesystem::remove_all(scratch, ec);
}

TEST(ModuleABI_ManifestWithUnusableEntryLoadsNothing)
{
    const std::filesystem::path modulePath = CopyCompatibleFixtureToTemp("SparkManifestAllOrNothingModule");
    const std::filesystem::path manifestPath = modulePath.parent_path() / "spark.all-or-nothing.modules.json";
    const std::string goodEntry = "{ \"path\": \"" + PathToUtf8(modulePath.filename()) + "\" }";

    struct Case
    {
        const char* label;
        std::string secondEntry;
        const char* expectedError;
    };
    const Case cases[] = {
        // Listed after a module that loads, so a partial set would already exist.
        {"ABI-rejected", "{ \"path\": \"" + PathToUtf8(PathFromUtf8(SPARK_TEST_MISMATCHED_MODULE_PATH)) + "\" }",
         "rejected"},
        {"malformed", "{ \"name\": \"NoPath\" }", "no string path"},
        {"missing", "{ \"path\": \"SparkNoSuchManifestModule.dll\" }", "Module not found"},
    };

    for (const Case& manifestCase : cases)
    {
        {
            std::ofstream manifest(manifestPath, std::ios::trunc);
            manifest << "{ \"modules\": [" << goodEntry << ", " << manifestCase.secondEntry << "] }\n";
        }

        ModuleManager manager;
        const bool loaded = manager.LoadModulesFromManifest(PathToUtf8(manifestPath));
        if (loaded)
            std::cerr << "  manifest case '" << manifestCase.label << "' loaded a partial module set\n";
        EXPECT_FALSE(loaded);
        EXPECT_STR_CONTAINS(manager.GetLastLoadError(), manifestCase.expectedError);
        EXPECT_EQ(manager.GetModuleCount(), size_t{0});
        manager.UnloadAll();
    }

    RemoveModuleCopy(modulePath);
    std::error_code ec;
    std::filesystem::remove(manifestPath, ec);
}

TEST(ModuleABI_UnloadedDependencyBlocksInitialization)
{
    NullEngineContext context;
    ModuleManager manager;
    {
        ScopedTestEnvironment dependsOn("SPARK_MODULE_ABI_DEPENDS_ON", "Spark Module That Is Not Loaded");
        ASSERT_TRUE(manager.LoadModule(SPARK_TEST_COMPATIBLE_MODULE_PATH));
        EXPECT_FALSE(manager.InitializeAll(&context));
        EXPECT_FALSE(manager.HasInitializedModules());
        EXPECT_EQ(manager.GetInitializedModuleCount(), size_t{0});
    }

    // Without the declaration the same module initializes, so the refusal
    // above came from the dependency graph and not from OnLoad.
    EXPECT_TRUE(manager.InitializeAll(&context));
    EXPECT_EQ(manager.GetInitializedModuleCount(), size_t{1});
    EXPECT_TRUE(manager.ShutdownAll());
    manager.UnloadAll();
}

TEST(ModuleABI_DependencyCycleBlocksInitialization)
{
    NullEngineContext context;
    ModuleManager manager;
    {
        // A module that depends on itself is the smallest cycle.
        ScopedTestEnvironment dependsOn("SPARK_MODULE_ABI_DEPENDS_ON", "Spark Compatible ABI Fixture");
        ASSERT_TRUE(manager.LoadModule(SPARK_TEST_COMPATIBLE_MODULE_PATH));
        EXPECT_FALSE(manager.InitializeAll(&context));
        EXPECT_FALSE(manager.HasInitializedModules());
    }

    EXPECT_TRUE(manager.InitializeAll(&context));
    EXPECT_TRUE(manager.ShutdownAll());
    manager.UnloadAll();
}

TEST(ModuleABI_FailedDependencyInitializationSkipsDependent)
{
    // The dependency is loaded, so the up-front graph check passes. Its OnLoad
    // then throws in the same pass; the dependent, sorted after it, must not
    // start without it.
    RegistryFixtureHostGuard host;
    const std::string dependencyName = "Spark Registry Lifecycle Fixture";
    const std::string dependentName = "Spark Compatible ABI Fixture";
    ScopedTestEnvironment dependsOn("SPARK_MODULE_ABI_DEPENDS_ON", dependencyName);

    NullEngineContext context;
    ModuleManager manager;
    ASSERT_TRUE(manager.LoadModule(SPARK_TEST_COMPATIBLE_MODULE_PATH));
    ASSERT_TRUE(manager.LoadModule(SPARK_TEST_REGISTRY_LIFECYCLE_MODULE_PATH));

    bool initialized = true;
    {
        ScopedTestEnvironment throwOnLoad("SPARK_REGISTRY_FIXTURE_THROW_ON_LOAD", "1");
        EXPECT_NO_THROW(initialized = manager.InitializeAll(&context));
    }
    EXPECT_FALSE(initialized);
    EXPECT_TRUE(manager.GetModule(dependencyName) == nullptr);
    EXPECT_EQ(manager.GetInitializedModuleCount(), size_t{0});

    // The dependent's OnLoad always succeeds, so a recorded OnLoad would mean
    // it ran. It keeps its (never started) instance.
    const auto evidence = manager.GetLifecycleEvidence();
    const auto* dependent = evidence.FindModule(dependentName);
    EXPECT_TRUE(dependent == nullptr || dependent->onLoad == uint64_t{0});
    EXPECT_TRUE(manager.GetModule(dependentName) != nullptr);

    manager.UnloadAll();
    EXPECT_EQ(manager.GetModuleCount(), size_t{0});
}

TEST(ModuleABI_ReloadRefusesDependentOfUninitializedModule)
{
    // A staged replacement's OnLoad runs during the reload, so its dependency
    // must already be running, not merely loaded.
    RegistryFixtureHostGuard host;
    const std::string dependencyName = "Spark Registry Lifecycle Fixture";
    const std::string dependentName = "Spark Compatible ABI Fixture";
    ScopedTestEnvironment dependsOn("SPARK_MODULE_ABI_DEPENDS_ON", dependencyName);
    const std::filesystem::path modulePath = CopyCompatibleFixtureToTemp("SparkReloadUninitializedDependency");

    NullEngineContext context;
    {
        ModuleManager manager;
        ASSERT_TRUE(manager.LoadModule(PathToUtf8(modulePath)));
        ASSERT_TRUE(manager.LoadModule(SPARK_TEST_REGISTRY_LIFECYCLE_MODULE_PATH));

        EXPECT_FALSE(manager.ReloadModule(dependentName, &context));
        EXPECT_STR_CONTAINS(manager.GetLastLoadError(), "dependency graph");
        EXPECT_EQ(manager.GetInitializedModuleCount(), size_t{0});
        manager.UnloadAll();
    }
    RemoveModuleCopy(modulePath);
}

TEST(ModuleABI_FailedReplacementImageStaysMapped)
{
    RegistryFixtureHostGuard host;
    const std::filesystem::path modulePath =
        CopyCompatibleFixtureToTemp("SparkRetainedReplacementModule", SPARK_TEST_REGISTRY_LIFECYCLE_MODULE_PATH);
    const std::filesystem::path addressFile = ProcessScratchRoot() / "SparkRetainedReplacementCodeAddress.txt";
    std::error_code ec;
    std::filesystem::remove(addressFile, ec);

    NullEngineContext context;
    ModuleManager manager;
    ASSERT_TRUE(manager.LoadModule(PathToUtf8(modulePath)));
    ASSERT_TRUE(manager.InitializeAll(&context));

    {
        // Only the staged replacement sees these switches: it publishes an
        // address inside its own image, then fails OnLoad.
        ScopedTestEnvironment address("SPARK_REGISTRY_FIXTURE_CODE_ADDRESS_FILE", addressFile.string());
        ScopedTestEnvironment throwOnLoad("SPARK_REGISTRY_FIXTURE_THROW_ON_LOAD", "1");
        EXPECT_FALSE(manager.ReloadModule("Spark Registry Lifecycle Fixture", &context));
    }
    EXPECT_STR_CONTAINS(manager.GetLastLoadError(), "initialization failed");

    unsigned long long replacementCode = 0;
    {
        std::ifstream input(addressFile);
        input >> replacementCode;
    }
    ASSERT_TRUE(replacementCode != 0);
    // Callbacks the failed OnLoad left outside the owner-scoped registries can
    // still point into the replacement image, so it must not be unmapped.
    EXPECT_TRUE(IsAddressInMappedImage(static_cast<std::uintptr_t>(replacementCode)));

    EXPECT_TRUE(manager.ShutdownAll());
    manager.UnloadAll();
    RemoveModuleCopy(modulePath);
    std::filesystem::remove(addressFile, ec);
}

TEST(ModuleABI_ThrowingPartialUnloadQuarantinesInstance)
{
    RegistryFixtureHostGuard host;
    const std::string moduleName = "Spark Registry Lifecycle Fixture";
    NullEngineContext context;
    ModuleManager manager;
    ASSERT_TRUE(manager.LoadModule(SPARK_TEST_REGISTRY_LIFECYCLE_MODULE_PATH));

    bool initialized = true;
    {
        ScopedTestEnvironment throwOnLoad("SPARK_REGISTRY_FIXTURE_THROW_ON_LOAD", "1");
        ScopedTestEnvironment throwOnUnload("SPARK_REGISTRY_FIXTURE_THROW_ON_UNLOAD", "1");
        EXPECT_NO_THROW(initialized = manager.InitializeAll(&context));
    }
    EXPECT_FALSE(initialized);
    EXPECT_TRUE(manager.GetModule(moduleName) == nullptr);

    // The partial OnUnload threw before removing its callbacks, so the host
    // must not destroy the instance they may capture. The owner-scoped
    // registrations are still removed by the host itself.
    const auto evidence = manager.GetLifecycleEvidence();
    const auto* record = evidence.FindModule(moduleName);
    ASSERT_TRUE(record != nullptr);
    EXPECT_EQ(record->onUnload, uint64_t{0});
    EXPECT_EQ(record->destroyModule, uint64_t{0});
    EXPECT_FALSE(host.console.HasCommand("registry_fixture_status"));
    EXPECT_FALSE(host.detector.HasRule("RegistryFixture.Rule"));

    manager.UnloadAll();
    EXPECT_EQ(manager.GetModuleCount(), size_t{0});
    const auto evidenceAfterUnload = manager.GetLifecycleEvidence();
    const auto* afterUnload = evidenceAfterUnload.FindModule(moduleName);
    ASSERT_TRUE(afterUnload != nullptr);
    EXPECT_EQ(afterUnload->destroyModule, uint64_t{0});
}

TEST(ModuleABI_ModulesOwningIdKeyedStateRefuseHotReload)
{
    // Transactional reload runs the replacement's OnLoad before the outgoing
    // OnUnload. Streaming areas and the shared network server are keyed by ID,
    // not by owner, so a module that registers them would have its outgoing
    // teardown remove what the replacement just registered. It must refuse
    // hot reload and require a restart instead.
    const std::filesystem::path gameModules = std::filesystem::path(SPARK_TEST_SOURCE_DIR) / "GameModules";
    size_t owners = 0;
    std::string missingOptOut;
    for (const auto& moduleDirectory : std::filesystem::directory_iterator(gameModules))
    {
        const std::filesystem::path sourceDirectory = moduleDirectory.path() / "Source";
        if (!moduleDirectory.is_directory() || !std::filesystem::is_directory(sourceDirectory))
            continue;

        bool ownsIdKeyedState = false;
        bool refusesHotReload = false;
        for (const auto& file : std::filesystem::recursive_directory_iterator(sourceDirectory))
        {
            const std::string extension = file.path().extension().string();
            if (!file.is_regular_file() || (extension != ".cpp" && extension != ".h"))
                continue;
            const std::string source = ReadBinaryFile(file.path());
            ownsIdKeyedState =
                ownsIdKeyedState || source.contains("RegisterArea(") || source.contains("StartNetworkServer(");
            refusesHotReload =
                refusesHotReload || source.contains("SupportsHotReload() const override { return false; }");
        }

        if (!ownsIdKeyedState)
            continue;
        ++owners;
        if (!refusesHotReload)
            missingOptOut += PathToUtf8(moduleDirectory.path().filename()) + " ";
    }

    // SparkGameMMO, SparkGameOpenWorld and SparkGameRPG: a scan that finds
    // nothing proves nothing.
    EXPECT_GE(owners, size_t{3});
    EXPECT_EQ(missingOptOut, std::string{});
}

#ifdef _WIN32
TEST(ModuleABI_WindowsModuleDependencyResolvesFromModuleDirectory)
{
    const std::filesystem::path moduleSource = PathFromUtf8(SPARK_TEST_SIBLING_DEPENDENT_MODULE_PATH);
    const std::filesystem::path dependencySource = PathFromUtf8(SPARK_TEST_SIBLING_DEPENDENCY_PATH);
    const std::wstring dependencyName = dependencySource.filename().wstring();
    // An already-mapped copy would satisfy the import by name and hide the search order.
    ASSERT_TRUE(GetModuleHandleW(dependencyName.c_str()) == nullptr);

    const std::filesystem::path moduleDirectory = ProcessScratchRoot() / "SparkModuleDependencySearch";
    std::error_code ec;
    std::filesystem::remove_all(moduleDirectory, ec);
    ASSERT_TRUE(std::filesystem::create_directories(moduleDirectory));
    const std::filesystem::path modulePath = moduleDirectory / moduleSource.filename();
    std::filesystem::copy_file(moduleSource, modulePath);
    std::filesystem::copy_file(SidecarPath(moduleSource), SidecarPath(modulePath));
    std::filesystem::copy_file(dependencySource, moduleDirectory / dependencySource.filename());

    ModuleManager manager;
    ASSERT_TRUE(manager.LoadModule(PathToUtf8(modulePath)));
    const HMODULE dependency = GetModuleHandleW(dependencyName.c_str());
    ASSERT_TRUE(dependency != nullptr);
    std::wstring loadedPath(32768, L'\0');
    const DWORD length = GetModuleFileNameW(dependency, loadedPath.data(), static_cast<DWORD>(loadedPath.size()));
    ASSERT_TRUE(length > 0 && length < loadedPath.size());
    loadedPath.resize(length);

    // The module's own directory is searched first: ahead of the application
    // directory (where the build also placed this dependency), and the loader
    // never reaches the current directory or PATH for it.
    EXPECT_TRUE(std::filesystem::equivalent(std::filesystem::path(loadedPath).parent_path(), moduleDirectory, ec));

    manager.UnloadAll();
    std::filesystem::remove_all(moduleDirectory, ec);
}
#endif

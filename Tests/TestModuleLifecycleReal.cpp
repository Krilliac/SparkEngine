/**
 * @file TestModuleLifecycleReal.cpp
 * @brief Real-ModuleManager regressions for failed-load recovery and manifest
 *        module-path resolution.
 *
 * Exercises the production ModuleManager against the real compatible ABI
 * fixture image — no mirror implementation.
 */

#include "TestFramework.h"

#include "Core/ModuleManager.h"
#include <Spark/Version.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <bcrypt.h>
#endif

#ifndef SPARK_TEST_COMPATIBLE_MODULE_PATH
#error SPARK_TEST_COMPATIBLE_MODULE_PATH must name the compatible module fixture
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
        Spark::SaveSystem* GetSaveSystem() override { return nullptr; }
        const Spark::SaveSystem* GetSaveSystem() const override { return nullptr; }
        uint32_t GetEngineVersion() const override { return SPARK_ENGINE_VERSION_PACKED; }
        uint32_t GetSDKVersion() const override { return SPARK_SDK_VERSION; }
    };

    /**
     * @brief A SPARK_MODULE_ABI_* switch scoped to the test that sets it.
     *
     * The fixture image reads these variables, and so do TestModuleABI.cpp's
     * cases later in the same process. Setting them by hand meant any ASSERT_*
     * between the set and the reset returned from the test body first and left
     * the fixture reconfigured for everything that ran after it.
     */
    class ScopedModuleEnvironment
    {
      public:
        ScopedModuleEnvironment(const char* name, bool enabled) : m_name(name)
        {
            if (const char* existing = std::getenv(name))
            {
                m_hadValue = true;
                m_previous = existing;
            }
            Set(enabled);
        }

        ~ScopedModuleEnvironment() { Apply(m_hadValue ? m_previous : std::string{}); }

        ScopedModuleEnvironment(const ScopedModuleEnvironment&) = delete;
        ScopedModuleEnvironment& operator=(const ScopedModuleEnvironment&) = delete;

        /// Retoggle within the test; the destructor still restores the original.
        void Set(bool enabled) const { Apply(enabled ? std::string("1") : std::string{}); }

      private:
        void Apply(const std::string& value) const
        {
#ifdef _WIN32
            _putenv_s(m_name, value.c_str());
#else
            if (value.empty())
                unsetenv(m_name);
            else
                setenv(m_name, value.c_str(), 1);
#endif
        }

        const char* m_name;
        bool m_hadValue = false;
        std::string m_previous;
    };

    /** @brief Fresh, empty scratch directory removed and recreated per test. */
    std::filesystem::path MakeScratchDirectory(std::string_view name)
    {
        const std::filesystem::path directory = std::filesystem::temp_directory_path() / PathFromUtf8(name);
        std::error_code ec;
        std::filesystem::remove_all(directory, ec);
        std::filesystem::create_directories(directory, ec);
        return directory;
    }

    /** @brief Copy the compatible ABI fixture (and its mandatory sidecar) into @p destination. */
    bool CopyFixtureImage(const std::filesystem::path& destination)
    {
        const std::filesystem::path source = PathFromUtf8(SPARK_TEST_COMPATIBLE_MODULE_PATH);
        std::error_code ec;
        std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
            return false;
        std::filesystem::copy_file(SidecarPath(source), SidecarPath(destination),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        return !ec;
    }

#ifdef _WIN32
    /** @brief Real dual-ABI FPS image, used as a legacy-only adapter fixture after scratch-copy export masking. */
    std::filesystem::path LegacyAdapterFixturePath()
    {
        const std::filesystem::path compatibleFixture = PathFromUtf8(SPARK_TEST_COMPATIBLE_MODULE_PATH);
        std::filesystem::path legacyFixture = compatibleFixture.parent_path() / "SparkGameFPS";
        legacyFixture += compatibleFixture.extension();
        return legacyFixture;
    }

    bool CopyModuleImage(const std::filesystem::path& source, const std::filesystem::path& destination)
    {
        std::error_code ec;
        std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
            return false;
        std::filesystem::copy_file(SidecarPath(source), SidecarPath(destination),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        return !ec;
    }

    /** @brief Make a scratch copy select the real CreateGameModule exports without changing the source image. */
    bool HideNewStyleCreateExport(const std::filesystem::path& modulePath)
    {
        std::fstream image(modulePath, std::ios::in | std::ios::out | std::ios::binary);
        if (!image)
            return false;

        const std::string exportName = "CreateModule";
        std::string bytes((std::istreambuf_iterator<char>(image)), std::istreambuf_iterator<char>());
        const size_t offset = bytes.find(exportName);
        if (offset == std::string::npos)
            return false;

        image.clear();
        // Keep the PE export-name table sorted: CreateGameModule < CreateZodule < DestroyModule.
        // Changing its first letter would make GetProcAddress binary search miss later exports.
        image.seekp(static_cast<std::streamoff>(offset + std::string_view("Create").size()));
        image.put('Z');
        if (!image)
            return false;

        image.close();
        std::ifstream module(modulePath, std::ios::binary);
        const std::string moduleBytes((std::istreambuf_iterator<char>(module)), std::istreambuf_iterator<char>());
        if (!module)
            return false;

        BCRYPT_ALG_HANDLE algorithm = nullptr;
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            return false;
        std::array<UCHAR, 32> hash{};
        const NTSTATUS hashStatus = BCryptHash(algorithm, nullptr, 0,
                                               reinterpret_cast<PUCHAR>(const_cast<char*>(moduleBytes.data())),
                                               static_cast<ULONG>(moduleBytes.size()), hash.data(),
                                               static_cast<ULONG>(hash.size()));
        BCryptCloseAlgorithmProvider(algorithm, 0);
        if (hashStatus < 0)
            return false;

        static constexpr char hexDigits[] = "0123456789abcdef";
        std::string hashText;
        hashText.reserve(hash.size() * 2);
        for (const UCHAR byte : hash)
        {
            hashText.push_back(hexDigits[byte >> 4]);
            hashText.push_back(hexDigits[byte & 0x0f]);
        }

        const std::filesystem::path sidecarPath = SidecarPath(modulePath);
        std::ifstream sidecarIn(sidecarPath, std::ios::binary);
        std::string sidecar((std::istreambuf_iterator<char>(sidecarIn)), std::istreambuf_iterator<char>());
        const size_t hashOffset = sidecar.find("binary_sha256=");
        if (!sidecarIn || hashOffset == std::string::npos)
            return false;
        sidecar.replace(hashOffset + std::string_view("binary_sha256=").size(), hashText.size(), hashText);

        std::ofstream sidecarOut(sidecarPath, std::ios::binary | std::ios::trunc);
        sidecarOut.write(sidecar.data(), static_cast<std::streamsize>(sidecar.size()));
        return static_cast<bool>(sidecarOut);
    }
#endif

    /** @brief The module filename this host does NOT build, for the same fixture stem. */
    std::string ForeignPlatformModuleFilename()
    {
        const std::filesystem::path source = PathFromUtf8(SPARK_TEST_COMPATIBLE_MODULE_PATH);
        std::string stem = PathToUtf8(source.stem());
#ifdef _WIN32
        return "lib" + stem + ".so";
#else
        if (stem.starts_with("lib"))
            stem.erase(0, 3);
        return stem + ".dll";
#endif
    }

    void WriteManifest(const std::filesystem::path& manifestPath, std::string_view modulePath)
    {
        std::ofstream manifest(manifestPath, std::ios::trunc);
        manifest << "{\n  \"modules\": [\n    { \"name\": \"Fixture\", \"path\": \"" << modulePath << "\" }\n  ]\n}\n";
    }
} // namespace

TEST(ModuleLifecycle_FailedGameLoadDoesNotBlockAReplacementGameModule)
{
    const ScopedModuleEnvironment kindGame("SPARK_MODULE_ABI_KIND_GAME", true);
    const ScopedModuleEnvironment failOnLoad("SPARK_MODULE_ABI_FAIL_ON_LOAD", true);

    const std::filesystem::path directory = MakeScratchDirectory("SparkModuleLifecycleGhostEntry");
    const std::filesystem::path source = PathFromUtf8(SPARK_TEST_COMPATIBLE_MODULE_PATH);
    std::filesystem::path firstPath = directory / "FirstGame";
    firstPath += source.extension();
    std::filesystem::path secondPath = directory / "SecondGame";
    secondPath += source.extension();
    ASSERT_TRUE(CopyFixtureImage(firstPath));
    ASSERT_TRUE(CopyFixtureImage(secondPath));

    NullEngineContext context;
    ModuleManager manager;
    ASSERT_TRUE(manager.LoadModule(PathToUtf8(firstPath)));
    manager.InitializeAll(&context);

    // The failed module's instance is destroyed but its entry (and mapped DLL)
    // stays. Before the fix GetGameModuleName() still named it, so the
    // single-game-module policy REFUSED every subsequent Game-kind module for
    // the rest of the process lifetime.
    failOnLoad.Set(false);
    EXPECT_TRUE(manager.GetGameModuleName().empty());
    EXPECT_FALSE(manager.HasInitializedModules());

    EXPECT_TRUE(manager.LoadModule(PathToUtf8(secondPath)));
    manager.InitializeAll(&context);
    EXPECT_FALSE(manager.GetInitializedGameModuleName().empty());
    EXPECT_TRUE(manager.HasInitializedModules());

    kindGame.Set(false);
    manager.ShutdownAll();
    manager.UnloadAll();

    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
}

TEST(ModuleLifecycle_ManifestResolvesAForeignPlatformModulePath)
{
    const std::filesystem::path directory = MakeScratchDirectory("SparkModuleLifecycleManifestRemap");
    const std::filesystem::path source = PathFromUtf8(SPARK_TEST_COMPATIBLE_MODULE_PATH);
    const std::filesystem::path hostImage = directory / source.filename();
    ASSERT_TRUE(CopyFixtureImage(hostImage));

    // Every shipped spark.modules.json (and every generated project) writes the
    // Windows ".dll" form, so a manifest launch resolved nothing on Linux/macOS
    // where the artifact is "lib<Name>.so".
    const std::filesystem::path manifestPath = directory / "spark.modules.json";
    WriteManifest(manifestPath, ForeignPlatformModuleFilename());

    ModuleManager manager;
    EXPECT_TRUE(manager.LoadModulesFromManifest(PathToUtf8(manifestPath)));
    EXPECT_TRUE(manager.GetLastLoadError().empty());
    EXPECT_EQ(manager.GetModuleCount(), size_t{1});

    manager.UnloadAll();

    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
}

TEST(ModuleLifecycle_ManifestStillReportsATrulyMissingModule)
{
    const std::filesystem::path directory = MakeScratchDirectory("SparkModuleLifecycleManifestMissing");
    const std::filesystem::path manifestPath = directory / "spark.modules.json";
    WriteManifest(manifestPath, "NoSuchModule.dll");

    ModuleManager manager;
    EXPECT_FALSE(manager.LoadModulesFromManifest(PathToUtf8(manifestPath)));
    EXPECT_STR_CONTAINS(manager.GetLastLoadError(), "NoSuchModule");
    EXPECT_EQ(manager.GetModuleCount(), size_t{0});

    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
}

TEST(ModuleLifecycle_RecordsSuccessfulNewStyleModuleCallbacks)
{
    const ScopedModuleEnvironment failOnLoad("SPARK_MODULE_ABI_FAIL_ON_LOAD", false);

    NullEngineContext context;
    ModuleManager manager;
    ASSERT_TRUE(manager.LoadModule(SPARK_TEST_COMPATIBLE_MODULE_PATH));
    manager.InitializeAll(&context);
    manager.UpdateAll(1.0F / 60.0F);
    manager.FixedUpdateAll(1.0F / 60.0F);
    manager.RenderAll();
    ASSERT_TRUE(manager.ShutdownAll());
    manager.UnloadAll();

    const auto evidence = manager.GetLifecycleEvidence();
    const auto* record = evidence.FindModule("Spark Compatible ABI Fixture");
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->createModule, 1u);
    EXPECT_EQ(record->onLoad, 1u);
    EXPECT_GE(record->onUpdate, 1u);
    EXPECT_GE(record->onFixedUpdate, 1u);
    EXPECT_GE(record->onRender, 1u);
    EXPECT_EQ(record->onUnload, 1u);
    EXPECT_EQ(record->destroyModule, 1u);
    EXPECT_EQ(record->faults, 0u);
}

TEST(ModuleLifecycle_RecordsFailedNewStyleModuleInitialization)
{
    const ScopedModuleEnvironment failOnLoad("SPARK_MODULE_ABI_FAIL_ON_LOAD", true);

    NullEngineContext context;
    ModuleManager manager;
    ASSERT_TRUE(manager.LoadModule(SPARK_TEST_COMPATIBLE_MODULE_PATH));
    manager.InitializeAll(&context);

    const auto evidence = manager.GetLifecycleEvidence();
    const auto* record = evidence.FindModule("Spark Compatible ABI Fixture");
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->createModule, 1u);
    EXPECT_EQ(record->onLoad, 0u);
    EXPECT_EQ(record->onUnload, 1u);
    EXPECT_EQ(record->destroyModule, 1u);

    manager.UnloadAll();
}

#ifdef _WIN32
TEST(ModuleLifecycle_LegacyAdapterNeverCreatesLifecycleRecord)
{
    const std::filesystem::path directory = MakeScratchDirectory("SparkModuleLifecycleLegacyAdapter");
    const std::filesystem::path source = LegacyAdapterFixturePath();
    std::filesystem::path legacyPath = directory / source.filename();
    ASSERT_TRUE(CopyModuleImage(source, legacyPath));
    ASSERT_TRUE(HideNewStyleCreateExport(legacyPath));

    NullEngineContext context;
    ModuleManager manager;
    ASSERT_TRUE(manager.LoadModule(PathToUtf8(legacyPath)));
    manager.InitializeAll(&context);
    manager.UpdateAll(1.0F / 60.0F);
    manager.FixedUpdateAll(1.0F / 60.0F);
    manager.RenderAll();
    manager.UnloadAll();

    const auto evidence = manager.GetLifecycleEvidence();
    EXPECT_EQ(evidence.FindModule("Spark Arena"), nullptr);
    EXPECT_TRUE(evidence.modules.empty());

    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
}
#endif

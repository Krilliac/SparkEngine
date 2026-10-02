/**
 * @file TestModuleVersion.cpp
 * @brief OD-02 N-1 / N / N+1 module version behavior through the production loader (SDK-240)
 *
 * stable-v1 module ABI is exact-match only: a module built against the
 * previous (N-1) or next (N+1) SDK is rejected, never loaded or migrated, and
 * the rejection names the field plus the host's and the module's values.
 * These tests use real shared-library fixtures compiled against N-1 and N+1
 * (Tests/CMakeLists.txt builds Fixtures/ModuleABI/MismatchedModule.cpp twice
 * with a declared SDK offset and a matching build-generated .sparkabi
 * sidecar) plus the current-SDK fixture as the positive control. Sentinels
 * written by the fixtures prove no module code runs before rejection.
 */

#include "TestFramework.h"

#include "Core/FileIntegrity.h"
#include "Core/ModuleManager.h"
#include <Spark/ModuleABI.h>
#include <Spark/Version.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>

#ifndef SPARK_TEST_MISMATCHED_MODULE_PATH
#error SPARK_TEST_MISMATCHED_MODULE_PATH must name the N+1 SDK module fixture
#endif

#ifndef SPARK_TEST_PREVIOUS_SDK_MODULE_PATH
#error SPARK_TEST_PREVIOUS_SDK_MODULE_PATH must name the N-1 SDK module fixture
#endif

#ifndef SPARK_TEST_COMPATIBLE_MODULE_PATH
#error SPARK_TEST_COMPATIBLE_MODULE_PATH must name the compatible module fixture
#endif

namespace
{
    constexpr uint32_t kHostSdkVersion = SPARK_SDK_VERSION;
    constexpr uint32_t kPreviousSdkVersion = SPARK_SDK_VERSION - 1;
    constexpr uint32_t kNextSdkVersion = SPARK_SDK_VERSION + 1;

    static_assert(SPARK_SDK_VERSION >= 1, "an N-1 SDK version must exist for these tests");

    // Written independently of ModuleManager.cpp so a changed policy sentence
    // or field wording in production fails here.
    constexpr std::string_view kExactMatchPolicy =
        "stable-v1 module ABI is exact-match only (N-1 modules are not loaded); rebuild the module against this "
        "host's Spark SDK and toolchain";

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

    std::map<std::string, std::string> ReadSidecar(const std::filesystem::path& modulePath)
    {
        std::map<std::string, std::string> values;
        std::ifstream sidecar(SidecarPath(modulePath), std::ios::binary);
        std::string line;
        while (std::getline(sidecar, line))
        {
            // CMake's file(WRITE) emits CRLF on Windows; ModuleManager accepts
            // CRLF sidecars, so read them the same way it does.
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            const size_t separator = line.find('=');
            if (separator != std::string::npos)
                values[line.substr(0, separator)] = line.substr(separator + 1);
        }
        return values;
    }

    std::string SdkMismatchDiagnostic(uint32_t declared)
    {
        return std::format("SDK ABI version mismatch: field 'sdk_version' host expects {}, module declares {}; {}",
                           kHostSdkVersion, declared, kExactMatchPolicy);
    }

    std::string RuntimeAbiMismatchDiagnostic(uint32_t declared)
    {
        return std::format("module runtime ABI version mismatch: field 'runtime_abi_version' host expects {}, module "
                           "declares {}; {}",
                           SPARK_MODULE_RUNTIME_ABI_VERSION, declared, kExactMatchPolicy);
    }

    /** Write a complete sidecar in the layout spark_configure_module_abi generates. */
    void WriteSidecar(const std::filesystem::path& modulePath, const SparkModuleCompatibilityDescriptor& descriptor,
                      std::string_view binarySha256)
    {
        std::ofstream sidecar(SidecarPath(modulePath), std::ios::binary | std::ios::trunc);
        sidecar << "struct_size=" << descriptor.structSize << '\n'
                << "magic=" << descriptor.magic << '\n'
                << "format=" << descriptor.descriptorVersion << '\n'
                << "sdk_version=" << descriptor.sdkVersion << '\n'
                << "runtime_abi_version=" << descriptor.runtimeABIVersion << '\n'
                << "compiler_family=" << descriptor.compilerFamily << '\n'
                << "compiler_abi_version=" << descriptor.compilerABIVersion << '\n'
                << "cxx_language_level=" << descriptor.cxxLanguageLevel << '\n'
                << "runtime_library=" << descriptor.runtimeLibrary << '\n'
                << "iterator_debug_level=" << descriptor.iteratorDebugLevel << '\n'
                << "pointer_size=" << descriptor.pointerSize << '\n'
                << "binary_sha256=" << binarySha256 << '\n';
    }

    void RemoveModuleFiles(const std::filesystem::path& modulePath)
    {
        std::error_code ignored;
        std::filesystem::remove(SidecarPath(modulePath), ignored);
        std::filesystem::remove(modulePath, ignored);
    }

    /** Points the fixtures' execution sentinel at a fresh file for one scope. */
    class ExecutionSentinel final
    {
      public:
        explicit ExecutionSentinel(std::string_view name)
            : m_path(std::filesystem::temp_directory_path() / std::string(name))
        {
            std::error_code ignored;
            std::filesystem::remove(m_path, ignored);
            const std::string path = m_path.string();
#ifdef _WIN32
            _putenv_s("SPARK_MODULE_ABI_SENTINEL", path.c_str());
#else
            setenv("SPARK_MODULE_ABI_SENTINEL", path.c_str(), 1);
#endif
        }

        ~ExecutionSentinel()
        {
#ifdef _WIN32
            _putenv_s("SPARK_MODULE_ABI_SENTINEL", "");
#else
            unsetenv("SPARK_MODULE_ABI_SENTINEL");
#endif
            std::error_code ignored;
            std::filesystem::remove(m_path, ignored);
        }

        ExecutionSentinel(const ExecutionSentinel&) = delete;
        ExecutionSentinel& operator=(const ExecutionSentinel&) = delete;

        bool Written() const { return std::filesystem::exists(m_path); }

        /** Phases recorded by the fixture, one per line, in execution order. */
        std::string Contents() const
        {
            std::ifstream sentinel(m_path, std::ios::binary);
            std::ostringstream contents;
            contents << sentinel.rdbuf();
            return contents.str();
        }

      private:
        std::filesystem::path m_path;
    };

    /** A module-named file that must never reach the OS loader: it is not an image at all. */
    std::filesystem::path WriteUnloadableModule(std::string_view stem)
    {
        std::filesystem::path modulePath = std::filesystem::temp_directory_path() / std::string(stem);
        modulePath += PathFromUtf8(SPARK_TEST_COMPATIBLE_MODULE_PATH).extension();
        std::ofstream image(modulePath, std::ios::binary | std::ios::trunc);
        image << "not a native module image";
        return modulePath;
    }
} // namespace

TEST(ModuleVersion_FixtureSidecarsDeclarePreviousCurrentAndNextSdk)
{
    // Guards the fixtures themselves: if the build stopped generating a real
    // N-1 or N+1 image, the rejection tests below would prove nothing.
    EXPECT_EQ(ReadSidecar(PathFromUtf8(SPARK_TEST_PREVIOUS_SDK_MODULE_PATH))["sdk_version"],
              std::to_string(kPreviousSdkVersion));
    EXPECT_EQ(ReadSidecar(PathFromUtf8(SPARK_TEST_COMPATIBLE_MODULE_PATH))["sdk_version"],
              std::to_string(kHostSdkVersion));
    EXPECT_EQ(ReadSidecar(PathFromUtf8(SPARK_TEST_MISMATCHED_MODULE_PATH))["sdk_version"],
              std::to_string(kNextSdkVersion));

    EXPECT_FALSE(Spark::IsSDKCompatible(kPreviousSdkVersion));
    EXPECT_TRUE(Spark::IsSDKCompatible(kHostSdkVersion));
    EXPECT_FALSE(Spark::IsSDKCompatible(kNextSdkVersion));
}

TEST(ModuleVersion_CurrentSdkModuleLoads)
{
    ModuleManager manager;
    ASSERT_TRUE(manager.LoadModule(SPARK_TEST_COMPATIBLE_MODULE_PATH));
    EXPECT_TRUE(manager.GetLastLoadError().empty());
    const auto loaded = manager.GetLoadedModuleInfo();
    ASSERT_EQ(loaded.size(), size_t{1});
    EXPECT_EQ(loaded[0].name, std::string("Spark Compatible ABI Fixture"));
    manager.UnloadAll();
}

TEST(ModuleVersion_PreviousSdkModuleRejectedBeforeOsLoadNamingBothVersions)
{
    ExecutionSentinel sentinel("spark-module-version-previous-sentinel.txt");

    ModuleManager manager;
    EXPECT_FALSE(manager.LoadModule(SPARK_TEST_PREVIOUS_SDK_MODULE_PATH));
    EXPECT_EQ(manager.GetLastLoadError(),
              std::format("Module '{}' rejected before OS load: {}", SPARK_TEST_PREVIOUS_SDK_MODULE_PATH,
                          SdkMismatchDiagnostic(kPreviousSdkVersion)));
    // No static constructor, DllMain, injection hook, or factory ran.
    EXPECT_FALSE(sentinel.Written());
    EXPECT_TRUE(manager.GetLoadedModuleInfo().empty());

    // The rejection leaves no partial state: the same manager still loads a
    // current-SDK module afterwards.
    ASSERT_TRUE(manager.LoadModule(SPARK_TEST_COMPATIBLE_MODULE_PATH));
    EXPECT_EQ(manager.GetLoadedModuleInfo().size(), size_t{1});
    manager.UnloadAll();
}

TEST(ModuleVersion_NextSdkModuleRejectedBeforeOsLoadNamingBothVersions)
{
    ExecutionSentinel sentinel("spark-module-version-next-sentinel.txt");

    ModuleManager manager;
    EXPECT_FALSE(manager.LoadModule(SPARK_TEST_MISMATCHED_MODULE_PATH));
    EXPECT_EQ(manager.GetLastLoadError(),
              std::format("Module '{}' rejected before OS load: {}", SPARK_TEST_MISMATCHED_MODULE_PATH,
                          SdkMismatchDiagnostic(kNextSdkVersion)));
    EXPECT_FALSE(sentinel.Written());
    EXPECT_TRUE(manager.GetLoadedModuleInfo().empty());
}

TEST(ModuleVersion_PreviousSdkImageBehindForgedSidecarRejectedBeforeInjection)
{
    // A sidecar forged to claim the host's values (with the image's real hash)
    // passes the pre-load gate; the N-1 image's own descriptor must still be
    // rejected before any injection hook or the factory runs.
    const std::filesystem::path source = PathFromUtf8(SPARK_TEST_PREVIOUS_SDK_MODULE_PATH);
    std::filesystem::path modulePath = std::filesystem::temp_directory_path() / "SparkModuleVersionPreviousInImage";
    modulePath += source.extension();
    RemoveModuleFiles(modulePath);
    std::filesystem::copy_file(source, modulePath, std::filesystem::copy_options::overwrite_existing);

    std::string digest;
    std::string hashError;
    ASSERT_TRUE(Spark::FileIntegrity::ComputeSha256(modulePath, digest, hashError));
    WriteSidecar(modulePath, Spark::kExpectedModuleCompatibility, digest);

    const std::string moduleArg = PathToUtf8(modulePath);
    std::string sentinelContents;
    std::string error;
    bool loaded = false;
    bool anyLoaded = false;
    {
        ExecutionSentinel sentinel("spark-module-version-in-image-sentinel.txt");
        ModuleManager manager;
        loaded = manager.LoadModule(moduleArg);
        error = manager.GetLastLoadError();
        anyLoaded = !manager.GetLoadedModuleInfo().empty();
        sentinelContents = sentinel.Contents();
    }
    RemoveModuleFiles(modulePath);

    EXPECT_FALSE(loaded);
    EXPECT_FALSE(anyLoaded);
    EXPECT_EQ(error, std::format("Module '{}' in-image compatibility descriptor rejected before injection/factory: {}",
                                 moduleArg, SdkMismatchDiagnostic(kPreviousSdkVersion)));
    // The OS loader necessarily ran image initialization; nothing after it did.
    for (const std::string_view phase : {"SparkModuleInjectConsole", "SparkModuleInjectEngineContext",
                                         "SparkModuleInjectImGui", "CreateModule", "DestroyModule"})
        EXPECT_EQ(sentinelContents.find(phase), std::string::npos);
#ifndef _WIN32
    EXPECT_EQ(sentinelContents, std::string("StaticConstructor\n"));
#endif
}

TEST(ModuleVersion_NeighbouringRuntimeAbiVersionsRejectedBeforeOsLoad)
{
    const std::filesystem::path modulePath = WriteUnloadableModule("SparkModuleVersionRuntimeAbi");
    const std::string moduleArg = PathToUtf8(modulePath);
    const std::string placeholderHash(64, '0');

    for (const uint32_t declared : {SPARK_MODULE_RUNTIME_ABI_VERSION - 1u, SPARK_MODULE_RUNTIME_ABI_VERSION + 1u})
    {
        SparkModuleCompatibilityDescriptor neighbour = Spark::kExpectedModuleCompatibility;
        neighbour.runtimeABIVersion = declared;
        WriteSidecar(modulePath, neighbour, placeholderHash);

        ModuleManager manager;
        EXPECT_FALSE(manager.LoadModule(moduleArg));
        // Reaching the OS loader would report a dlopen/LoadLibraryW failure
        // for this non-image file instead of the sidecar diagnostic.
        EXPECT_EQ(manager.GetLastLoadError(), std::format("Module '{}' rejected before OS load: {}", moduleArg,
                                                          RuntimeAbiMismatchDiagnostic(declared)));
        EXPECT_TRUE(manager.GetLoadedModuleInfo().empty());
    }

    RemoveModuleFiles(modulePath);
}

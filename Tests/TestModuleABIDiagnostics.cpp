/**
 * @file TestModuleABIDiagnostics.cpp
 * @brief OD-02 exact-match module ABI rejection diagnostics (SDK-240)
 *
 * stable-v1 loads only modules whose compatibility descriptor matches the
 * host exactly; N-1 modules are rejected. Every rejection must name the
 * mismatched field, the host's expected value, and the module's declared
 * value. These tests drive the production ModuleManager load path with
 * hand-written .sparkabi sidecars (rejected before the OS loader maps the
 * image), the real build-generated mismatched fixture, and the in-image and
 * ModuleInfo re-checks that run after the sidecar gate.
 */

#include "TestFramework.h"

#include "Core/FileIntegrity.h"
#include "Core/ModuleManager.h"
#include <Spark/ModuleABI.h>
#include <Spark/Version.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>

#ifndef SPARK_TEST_MISMATCHED_MODULE_PATH
#error SPARK_TEST_MISMATCHED_MODULE_PATH must name the mismatched module fixture
#endif

#ifndef SPARK_TEST_COMPATIBLE_MODULE_PATH
#error SPARK_TEST_COMPATIBLE_MODULE_PATH must name the compatible module fixture
#endif

namespace
{
    constexpr std::string_view kExactMatchPolicy =
        "stable-v1 module ABI is exact-match only (N-1 modules are not loaded); rebuild the module against this "
        "host's Spark SDK and toolchain";

    struct ExactMatchField
    {
        std::string_view sidecarKey;
        std::string_view reason;
        uint32_t SparkModuleCompatibilityDescriptor::*member;
    };

    // Written independently of ModuleManager.cpp so a renamed key, reason, or
    // swapped member in production fails here.
    constexpr std::array<ExactMatchField, 10> kExactMatchFields = {{
        {"magic", "compatibility descriptor magic mismatch", &SparkModuleCompatibilityDescriptor::magic},
        {"format", "compatibility descriptor version mismatch", &SparkModuleCompatibilityDescriptor::descriptorVersion},
        {"sdk_version", "SDK ABI version mismatch", &SparkModuleCompatibilityDescriptor::sdkVersion},
        {"runtime_abi_version", "module runtime ABI version mismatch",
         &SparkModuleCompatibilityDescriptor::runtimeABIVersion},
        {"compiler_family", "compiler ABI family mismatch", &SparkModuleCompatibilityDescriptor::compilerFamily},
        {"compiler_abi_version", "compiler ABI version mismatch",
         &SparkModuleCompatibilityDescriptor::compilerABIVersion},
        {"cxx_language_level", "C++ language level mismatch", &SparkModuleCompatibilityDescriptor::cxxLanguageLevel},
        {"runtime_library", "runtime library mismatch", &SparkModuleCompatibilityDescriptor::runtimeLibrary},
        {"iterator_debug_level", "iterator debug level mismatch",
         &SparkModuleCompatibilityDescriptor::iteratorDebugLevel},
        {"pointer_size", "pointer size mismatch", &SparkModuleCompatibilityDescriptor::pointerSize},
    }};

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

    std::string ExpectedFieldDiagnostic(const ExactMatchField& field, uint32_t declared)
    {
        return std::format("{}: field '{}' host expects {}, module declares {}; {}", field.reason, field.sidecarKey,
                           Spark::kExpectedModuleCompatibility.*field.member, declared, kExactMatchPolicy);
    }

    /** Write a complete 12-field sidecar in the layout spark_configure_module_abi generates. */
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

    /** A module-named file that must never reach the OS loader: it is not an image at all. */
    std::filesystem::path WriteUnloadableModule(std::string_view stem)
    {
        std::filesystem::path modulePath = std::filesystem::temp_directory_path() / std::string(stem);
        modulePath += PathFromUtf8(SPARK_TEST_COMPATIBLE_MODULE_PATH).extension();
        std::ofstream image(modulePath, std::ios::binary | std::ios::trunc);
        image << "not a native module image";
        return modulePath;
    }

    void RemoveModuleFiles(const std::filesystem::path& modulePath)
    {
        std::error_code ignored;
        std::filesystem::remove(SidecarPath(modulePath), ignored);
        std::filesystem::remove(modulePath, ignored);
    }

    void SetContradictInfoSdkEnvironment(bool enabled)
    {
#ifdef _WIN32
        _putenv_s("SPARK_MODULE_ABI_CONTRADICT_INFO_SDK", enabled ? "1" : "");
#else
        if (enabled)
            setenv("SPARK_MODULE_ABI_CONTRADICT_INFO_SDK", "1", 1);
        else
            unsetenv("SPARK_MODULE_ABI_CONTRADICT_INFO_SDK");
#endif
    }

    const std::string kPlaceholderHash(64, '0');
} // namespace

TEST(ModuleABI_RejectionDiagnosticNamesFieldAndBothValues)
{
    EXPECT_TRUE(DescribeModuleCompatibilityRejection(&Spark::kExpectedModuleCompatibility).empty());

    EXPECT_EQ(DescribeModuleCompatibilityRejection(nullptr),
              std::format("missing compatibility descriptor: host expects descriptor format {}, module declares "
                          "none; {}",
                          SPARK_MODULE_ABI_DESCRIPTOR_VERSION, kExactMatchPolicy));

    SparkModuleCompatibilityDescriptor truncated = Spark::kExpectedModuleCompatibility;
    truncated.structSize = 32;
    EXPECT_EQ(DescribeModuleCompatibilityRejection(&truncated),
              std::format("compatibility descriptor is too small: field 'struct_size' host expects at least {}, "
                          "module declares 32; {}",
                          SPARK_MODULE_ABI_DESCRIPTOR_SIZE, kExactMatchPolicy));

    for (const ExactMatchField& field : kExactMatchFields)
    {
        SparkModuleCompatibilityDescriptor mismatched = Spark::kExpectedModuleCompatibility;
        mismatched.*field.member += 1;
        EXPECT_EQ(DescribeModuleCompatibilityRejection(&mismatched),
                  ExpectedFieldDiagnostic(field, mismatched.*field.member));
    }
}

TEST(ModuleABI_PreviousSdkVersionIsRejectedNotMigrated)
{
    // OD-02: an N-1 module is rejected exactly like any other SDK mismatch.
    SparkModuleCompatibilityDescriptor previous = Spark::kExpectedModuleCompatibility;
    previous.sdkVersion = SPARK_SDK_VERSION - 1;
    EXPECT_EQ(DescribeModuleCompatibilityRejection(&previous),
              std::format("SDK ABI version mismatch: field 'sdk_version' host expects {}, module declares {}; {}",
                          SPARK_SDK_VERSION, SPARK_SDK_VERSION - 1, kExactMatchPolicy));
}

TEST(ModuleABI_SidecarMismatchRejectedBeforeOsLoadNamingBothValues)
{
    const std::filesystem::path modulePath = WriteUnloadableModule("SparkABIDiagnosticsSidecar");
    const std::string moduleArg = PathToUtf8(modulePath);

    for (const ExactMatchField& field : kExactMatchFields)
    {
        SparkModuleCompatibilityDescriptor mismatched = Spark::kExpectedModuleCompatibility;
        mismatched.*field.member += 1;
        WriteSidecar(modulePath, mismatched, kPlaceholderHash);

        ModuleManager manager;
        EXPECT_FALSE(manager.LoadModule(moduleArg));
        // Reaching the OS loader would report a dlopen/LoadLibraryW failure
        // for this non-image file instead of the sidecar diagnostic.
        EXPECT_EQ(manager.GetLastLoadError(), std::format("Module '{}' rejected before OS load: {}", moduleArg,
                                                          ExpectedFieldDiagnostic(field, mismatched.*field.member)));
        EXPECT_TRUE(manager.GetLoadedModuleInfo().empty());
    }

    SparkModuleCompatibilityDescriptor truncated = Spark::kExpectedModuleCompatibility;
    truncated.structSize = 32;
    WriteSidecar(modulePath, truncated, kPlaceholderHash);
    ModuleManager manager;
    EXPECT_FALSE(manager.LoadModule(moduleArg));
    EXPECT_EQ(manager.GetLastLoadError(),
              std::format("Module '{}' rejected before OS load: compatibility descriptor is too small: field "
                          "'struct_size' host expects at least {}, module declares 32; {}",
                          moduleArg, SPARK_MODULE_ABI_DESCRIPTOR_SIZE, kExactMatchPolicy));

    RemoveModuleFiles(modulePath);
}

TEST(ModuleABI_BuildGeneratedMismatchedSidecarNamesBothSdkVersions)
{
    ModuleManager manager;
    EXPECT_FALSE(manager.LoadModule(SPARK_TEST_MISMATCHED_MODULE_PATH));
    EXPECT_EQ(manager.GetLastLoadError(),
              std::format("Module '{}' rejected before OS load: SDK ABI version mismatch: field 'sdk_version' host "
                          "expects {}, module declares {}; {}",
                          SPARK_TEST_MISMATCHED_MODULE_PATH, SPARK_SDK_VERSION, SPARK_SDK_VERSION + 1,
                          kExactMatchPolicy));
}

TEST(ModuleABI_InImageDescriptorMismatchNamesBothSdkVersions)
{
    // A sidecar that lies about the image (claims the host's values with the
    // image's real hash) passes the pre-load gate; the in-image descriptor
    // re-check must still reject before any injection hook or factory runs.
    const std::filesystem::path source = PathFromUtf8(SPARK_TEST_MISMATCHED_MODULE_PATH);
    std::filesystem::path modulePath = std::filesystem::temp_directory_path() / "SparkABIDiagnosticsInImage";
    modulePath += source.extension();
    RemoveModuleFiles(modulePath);
    std::filesystem::copy_file(source, modulePath, std::filesystem::copy_options::overwrite_existing);

    std::string digest;
    std::string hashError;
    ASSERT_TRUE(Spark::FileIntegrity::ComputeSha256(modulePath, digest, hashError));
    WriteSidecar(modulePath, Spark::kExpectedModuleCompatibility, digest);

    const std::string moduleArg = PathToUtf8(modulePath);
    ModuleManager manager;
    EXPECT_FALSE(manager.LoadModule(moduleArg));
    EXPECT_EQ(manager.GetLastLoadError(),
              std::format("Module '{}' in-image compatibility descriptor rejected before injection/factory: SDK ABI "
                          "version mismatch: field 'sdk_version' host expects {}, module declares {}; {}",
                          moduleArg, SPARK_SDK_VERSION, SPARK_SDK_VERSION + 1, kExactMatchPolicy));
    EXPECT_TRUE(manager.GetLoadedModuleInfo().empty());

    RemoveModuleFiles(modulePath);
}

TEST(ModuleABI_ModuleInfoSdkContradictionNamesBothVersions)
{
    SetContradictInfoSdkEnvironment(true);
    ModuleManager manager;
    const bool loaded = manager.LoadModule(SPARK_TEST_COMPATIBLE_MODULE_PATH);
    const std::string error = manager.GetLastLoadError();
    const bool anyLoaded = !manager.GetLoadedModuleInfo().empty();
    SetContradictInfoSdkEnvironment(false);

    EXPECT_FALSE(loaded);
    EXPECT_FALSE(anyLoaded);
    EXPECT_EQ(error, std::format("Module 'Spark Compatible ABI Fixture' ('{}') rejected: ModuleInfo field "
                                 "'sdkVersion' host expects {}, module declares {}; stable-v1 module ABI is "
                                 "exact-match only (N-1 modules are not loaded)",
                                 SPARK_TEST_COMPATIBLE_MODULE_PATH, SPARK_SDK_VERSION, SPARK_SDK_VERSION + 1));
}

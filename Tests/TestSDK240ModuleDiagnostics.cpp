// TestSDK240ModuleDiagnostics.cpp - SDK-240: an incompatible module is rejected
// with a diagnostic that names the module's and the host's value of the field
// that failed, and the rejection reaches the unified Logger (not only the
// SimpleConsole history), so it lands in the engine log file.

#include "TestFramework.h"
#include "ScopedLoggerBaseline.h"

#include "Core/ModuleManager.h"
#include "Utils/Logger.h"
#include <Spark/ModuleABI.h>
#include <Spark/Version.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#ifndef SPARK_TEST_MISMATCHED_MODULE_PATH
#error SPARK_TEST_MISMATCHED_MODULE_PATH must name the mismatched module fixture
#endif

#ifndef SPARK_TEST_COMPATIBLE_MODULE_PATH
#error SPARK_TEST_COMPATIBLE_MODULE_PATH must name the compatible module fixture
#endif

namespace
{
    /// Records every Error-level Logger message while alive.
    class LoggedErrors
    {
      public:
        LoggedErrors()
        {
            Spark::Logger::Get().AddSink(std::make_unique<Spark::CallbackSink>(
                [this](const Spark::LogMessage& message)
                {
                    if (message.level >= Spark::LogLevel::Error)
                        m_messages.push_back(message.message);
                }));
        }

        bool Contains(const std::string& needle) const
        {
            for (const std::string& message : m_messages)
            {
                if (message.find(needle) != std::string::npos)
                    return true;
            }
            return false;
        }

      private:
        std::vector<std::string> m_messages;
    };

    std::filesystem::path SidecarOf(const std::filesystem::path& modulePath)
    {
        std::filesystem::path sidecar = modulePath;
        sidecar += ".sparkabi";
        return sidecar;
    }

    /// Copies the compatible fixture and rewrites one `key=value` sidecar line.
    std::filesystem::path CopyFixtureWithSidecarField(const std::string& stem, const std::string& key,
                                                      const std::string& value)
    {
        const std::filesystem::path source = SPARK_TEST_COMPATIBLE_MODULE_PATH;
        const std::filesystem::path directory = std::filesystem::temp_directory_path() / ("spark-sdk240-" + stem);
        std::error_code ec;
        std::filesystem::remove_all(directory, ec);
        std::filesystem::create_directories(directory);
        const std::filesystem::path destination = directory / source.filename();
        std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);

        std::string sidecar;
        {
            std::ifstream input(SidecarOf(source));
            sidecar.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        }
        const std::string prefix = "\n" + key + "=";
        const size_t start = ("\n" + sidecar).find(prefix);
        if (start != std::string::npos)
        {
            const size_t valueStart = start + key.size() + 1;
            const size_t valueEnd = sidecar.find('\n', valueStart);
            sidecar.replace(valueStart, valueEnd - valueStart, value);
        }
        std::ofstream output(SidecarOf(destination), std::ios::trunc | std::ios::binary);
        output << sidecar;
        return destination;
    }
} // namespace

TEST(SDK240_SidecarSDKMismatchNamesModuleAndHostVersions)
{
    ScopedLoggerBaseline loggerBaseline;
    LoggedErrors logged;

    ModuleManager manager;
    EXPECT_FALSE(manager.LoadModule(SPARK_TEST_MISMATCHED_MODULE_PATH));

    // The fixture's sidecar advertises SPARK_SDK_VERSION + 1 (Tests/CMakeLists.txt).
    const std::string moduleVersion = "module sdk_version=" + std::to_string(SPARK_SDK_VERSION + 1);
    const std::string hostVersion = "host sdk_version=" + std::to_string(SPARK_SDK_VERSION);
    const std::string error = manager.GetLastLoadError();
    EXPECT_STR_CONTAINS(error, "rejected before OS load");
    EXPECT_STR_CONTAINS(error, "SDK ABI version mismatch");
    EXPECT_STR_CONTAINS(error, moduleVersion);
    EXPECT_STR_CONTAINS(error, hostVersion);
    EXPECT_TRUE(logged.Contains(moduleVersion));
    EXPECT_TRUE(logged.Contains(hostVersion));
    EXPECT_TRUE(manager.GetLoadedModuleInfo().empty());
}

TEST(SDK240_SidecarRuntimeABIMismatchNamesBothValuesAndSDKVersions)
{
    ScopedLoggerBaseline loggerBaseline;
    LoggedErrors logged;

    const std::string moduleRuntime = std::to_string(SPARK_MODULE_RUNTIME_ABI_VERSION + 98u);
    const std::filesystem::path modulePath =
        CopyFixtureWithSidecarField("runtime-abi", "runtime_abi_version", moduleRuntime);

    ModuleManager manager;
    EXPECT_FALSE(manager.LoadModule(modulePath.string()));

    const std::string error = manager.GetLastLoadError();
    EXPECT_STR_CONTAINS(error, "module runtime ABI version mismatch");
    EXPECT_STR_CONTAINS(error, "module runtime_abi_version=" + moduleRuntime);
    EXPECT_STR_CONTAINS(error, "host runtime_abi_version=" + std::to_string(SPARK_MODULE_RUNTIME_ABI_VERSION));
    // Whatever field failed, the SDK versions of both sides are named too.
    EXPECT_STR_CONTAINS(error, "module sdk_version=" + std::to_string(SPARK_SDK_VERSION));
    EXPECT_STR_CONTAINS(error, "host sdk_version=" + std::to_string(SPARK_SDK_VERSION));
    EXPECT_TRUE(logged.Contains("module runtime_abi_version=" + moduleRuntime));
    EXPECT_TRUE(manager.GetLoadedModuleInfo().empty());

    std::error_code ec;
    std::filesystem::remove_all(modulePath.parent_path(), ec);
}

TEST(SDK240_EveryDescriptorMismatchNamesItsFieldAndBothValues)
{
    struct Case
    {
        uint32_t SparkModuleCompatibilityDescriptor::*member;
        Spark::ModuleCompatibilityStatus status;
        const char* field;
    };
    using Status = Spark::ModuleCompatibilityStatus;
    const Case cases[] = {
        {&SparkModuleCompatibilityDescriptor::magic, Status::BadMagic, "magic"},
        {&SparkModuleCompatibilityDescriptor::descriptorVersion, Status::DescriptorVersionMismatch, "format"},
        {&SparkModuleCompatibilityDescriptor::sdkVersion, Status::SDKVersionMismatch, "sdk_version"},
        {&SparkModuleCompatibilityDescriptor::runtimeABIVersion, Status::RuntimeABIVersionMismatch,
         "runtime_abi_version"},
        {&SparkModuleCompatibilityDescriptor::compilerFamily, Status::CompilerFamilyMismatch, "compiler_family"},
        {&SparkModuleCompatibilityDescriptor::compilerABIVersion, Status::CompilerABIVersionMismatch,
         "compiler_abi_version"},
        {&SparkModuleCompatibilityDescriptor::cxxLanguageLevel, Status::CxxLanguageLevelMismatch, "cxx_language_level"},
        {&SparkModuleCompatibilityDescriptor::runtimeLibrary, Status::RuntimeLibraryMismatch, "runtime_library"},
        {&SparkModuleCompatibilityDescriptor::iteratorDebugLevel, Status::IteratorDebugLevelMismatch,
         "iterator_debug_level"},
        {&SparkModuleCompatibilityDescriptor::pointerSize, Status::PointerSizeMismatch, "pointer_size"},
    };
    for (const Case& testCase : cases)
    {
        SparkModuleCompatibilityDescriptor descriptor = Spark::kExpectedModuleCompatibility;
        const uint32_t hostValue = descriptor.*testCase.member;
        descriptor.*testCase.member = hostValue + 17u;

        EXPECT_TRUE(Spark::CheckModuleCompatibility(&descriptor) == testCase.status);
        const Spark::ModuleCompatibilityMismatch mismatch =
            Spark::GetModuleCompatibilityMismatch(&descriptor, testCase.status);
        EXPECT_TRUE(mismatch.field != nullptr);
        if (mismatch.field)
            EXPECT_EQ(std::string(mismatch.field), std::string(testCase.field));
        EXPECT_EQ(mismatch.moduleValue, hostValue + 17u);
        EXPECT_EQ(mismatch.hostValue, hostValue);
    }

    SparkModuleCompatibilityDescriptor tooSmall = Spark::kExpectedModuleCompatibility;
    tooSmall.structSize = 32u;
    const Spark::ModuleCompatibilityMismatch sizeMismatch =
        Spark::GetModuleCompatibilityMismatch(&tooSmall, Spark::CheckModuleCompatibility(&tooSmall));
    EXPECT_TRUE(sizeMismatch.field != nullptr);
    EXPECT_EQ(sizeMismatch.moduleValue, 32u);
    EXPECT_EQ(sizeMismatch.hostValue, static_cast<uint32_t>(SPARK_MODULE_ABI_DESCRIPTOR_SIZE));

    EXPECT_TRUE(Spark::GetModuleCompatibilityMismatch(nullptr, Status::MissingDescriptor).field == nullptr);
    EXPECT_TRUE(Spark::GetModuleCompatibilityMismatch(&Spark::kExpectedModuleCompatibility, Status::Compatible).field ==
                nullptr);
}

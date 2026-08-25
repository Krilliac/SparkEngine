// TestRuntimePackage.cpp - Packaged runtime root-selection regression tests.

#include "TestFramework.h"
#include "Core/RuntimePackage.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    namespace fs = std::filesystem;

    class ScopedCurrentPath
    {
      public:
        explicit ScopedCurrentPath(const fs::path& path) : m_previous(fs::current_path()) { fs::current_path(path); }
        ~ScopedCurrentPath()
        {
            std::error_code error;
            fs::current_path(m_previous, error);
        }

      private:
        fs::path m_previous;
    };

    fs::path UniqueTestRoot()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        return fs::temp_directory_path() / ("spark-runtime-package-" + std::to_string(stamp));
    }
} // namespace

TEST(RuntimePackage_AnchorsForeignWorkingDirectory)
{
    const fs::path root = UniqueTestRoot();
    const fs::path caller = root / "caller";
    const fs::path package = root / "package";
    fs::create_directories(caller);
    fs::create_directories(package);
    std::ofstream(package / "manifest.json") << "{}";
    std::ofstream(package / "spark.modules.json") << "{}";

    {
        ScopedCurrentPath restore(caller);
        std::error_code error;
        EXPECT_TRUE(Spark::RuntimePackage::AnchorWorkingDirectory(package, error) ==
                    Spark::RuntimePackage::WorkingDirectoryResult::Anchored);
        EXPECT_FALSE(error);
        EXPECT_TRUE(fs::equivalent(fs::current_path(), package));
    }

    std::error_code cleanupError;
    fs::remove_all(root, cleanupError);
}

TEST(RuntimePackage_DoesNotAnchorUnpackagedBuild)
{
    const fs::path root = UniqueTestRoot();
    const fs::path caller = root / "caller";
    const fs::path executableDirectory = root / "build";
    fs::create_directories(caller);
    fs::create_directories(executableDirectory);
    std::ofstream(executableDirectory / "spark.modules.json") << "{}";

    {
        ScopedCurrentPath restore(caller);
        std::error_code error;
        EXPECT_TRUE(Spark::RuntimePackage::AnchorWorkingDirectory(executableDirectory, error) ==
                    Spark::RuntimePackage::WorkingDirectoryResult::NotPackaged);
        EXPECT_FALSE(error);
        EXPECT_TRUE(fs::equivalent(fs::current_path(), caller));
    }

    std::error_code cleanupError;
    fs::remove_all(root, cleanupError);
}

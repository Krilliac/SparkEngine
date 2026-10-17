/**
 * @file TestRuntimePackageContentRoots.cpp
 * @brief Spark::RuntimePackage::ResolveContentRoots() search order and input rejection.
 *
 * Game modules resolve content staged beside the engine executable (the
 * SparkGameVisualScript scripts) through this helper, so a launch from a
 * directory other than the executable's still finds it. Pure path arithmetic:
 * nothing here touches the filesystem.
 */

#include "TestFramework.h"
#include "Core/RuntimePackage.h"

#include <filesystem>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    using Spark::RuntimePackage::ResolveContentRoots;

    /// An absolute path on every platform ("<root>/<leaf>").
    fs::path Absolute(const char* leaf)
    {
        return (fs::temp_directory_path() / "spark-content-roots" / leaf).lexically_normal();
    }
} // namespace

TEST(RuntimePackage_ContentRoots_ExeDirFirstThenCwd)
{
    const fs::path exe = Absolute("install/bin");
    const fs::path cwd = Absolute("repo");

    const std::vector<fs::path> roots = ResolveContentRoots("Assets/Scripts/Generated", exe, cwd);
    ASSERT_EQ(roots.size(), static_cast<size_t>(2));
    EXPECT_TRUE(roots[0] == (exe / "Assets/Scripts/Generated").lexically_normal());
    EXPECT_TRUE(roots[1] == (cwd / "Assets/Scripts/Generated").lexically_normal());

    // Harmless dot segments are normalized away rather than rejected.
    const std::vector<fs::path> dotted = ResolveContentRoots("Assets/./Scripts/../Scripts/Generated", exe, cwd);
    ASSERT_EQ(dotted.size(), static_cast<size_t>(2));
    EXPECT_TRUE(dotted[0] == roots[0]);
}

TEST(RuntimePackage_ContentRoots_DeduplicatesWhenCwdIsExeDir)
{
    const fs::path exe = Absolute("install/bin");

    const std::vector<fs::path> same = ResolveContentRoots("Assets/Scripts/Generated", exe, exe);
    ASSERT_EQ(same.size(), static_cast<size_t>(1));
    EXPECT_TRUE(same[0] == (exe / "Assets/Scripts/Generated").lexically_normal());

    // The same directory spelled differently is still one candidate.
    const std::vector<fs::path> spelled = ResolveContentRoots("Assets", exe, exe / "sub" / "..");
    EXPECT_EQ(spelled.size(), static_cast<size_t>(1));
}

TEST(RuntimePackage_ContentRoots_EmptyExeDirFallsBackToCwd)
{
    const fs::path cwd = Absolute("repo");

    const std::vector<fs::path> roots = ResolveContentRoots("Assets/Scripts/Generated", fs::path(), cwd);
    ASSERT_EQ(roots.size(), static_cast<size_t>(1));
    EXPECT_TRUE(roots[0] == (cwd / "Assets/Scripts/Generated").lexically_normal());

    EXPECT_TRUE(ResolveContentRoots("Assets", fs::path(), fs::path()).empty());
}

TEST(RuntimePackage_ContentRoots_RejectsAbsoluteAndEscapingRelative)
{
    const fs::path exe = Absolute("install/bin");
    const fs::path cwd = Absolute("repo");

    EXPECT_TRUE(ResolveContentRoots(Absolute("elsewhere"), exe, cwd).empty());
    EXPECT_TRUE(ResolveContentRoots("/Assets", exe, cwd).empty());
    EXPECT_TRUE(ResolveContentRoots("../Assets", exe, cwd).empty());
    EXPECT_TRUE(ResolveContentRoots("Assets/../../secrets", exe, cwd).empty());
    EXPECT_TRUE(ResolveContentRoots("", exe, cwd).empty());
}

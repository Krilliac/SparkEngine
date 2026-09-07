/** @file assets.cpp
 * @brief Run the production FPS asset resolver with no private engine headers or linkage.
 */
#include "Game/FPSAssetPaths.h"

#include <fstream>
#include <iostream>
#include <string_view>

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    const std::string_view scenario(argv[1]);
    const auto working = std::filesystem::current_path();
    const auto fixture = working.parent_path().parent_path();
    auto expected = working / "Assets";
    if (scenario == "executable")
        expected = fixture / "bin" / "Assets";
    else if (scenario == "parent")
        expected = fixture / "Assets";
    else if (scenario != "working" && scenario != "fallback")
        return 2;

    int failures = 0;
    auto check = [&failures](bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << message << '\n';
            ++failures;
        }
    };
    check(Spark::FPSAssets::Root() == expected, "Asset discovery selected the wrong root");
    check(Spark::FPSAssets::RootExists() == (scenario != "fallback"), "Root existence report is incorrect");

    // Non-ASCII installation directories and filenames must survive UTF-8 APIs.
    const std::u8string relative = u8"Models/pistol-\u00e9-\u4e16\u754c.obj";
    const std::string relativeUtf8(reinterpret_cast<const char*>(relative.data()), relative.size());
    auto full = expected / std::filesystem::path(relative);
    full.make_preferred();
    const auto expectedUtf8 = full.u8string();
    check(Spark::FPSAssets::ResolveUtf8(relativeUtf8) ==
              std::string(reinterpret_cast<const char*>(expectedUtf8.data()), expectedUtf8.size()),
          "UTF-8 asset path changed encoding or root");
#ifdef _WIN32
    check(Spark::FPSAssets::Resolve(std::filesystem::path(relative).wstring()) == full.wstring(),
          "Wide asset path disagrees with UTF-8 resolution");
#endif

    // A directory alone is not enough: the marker must be a Models directory.
    const auto invalid = working / "invalid";
    const auto first = working / "first";
    const auto second = working / "second";
    std::filesystem::create_directories(invalid / "Assets");
    std::ofstream(invalid / "Assets" / "Models") << "not a directory";
    std::filesystem::create_directories(first / "Assets" / "Models");
    std::filesystem::create_directories(second / "Assets" / "Models");
    check(Spark::FPSAssets::FindAssetRoot({{}, working / "missing", invalid}).empty(),
          "Missing roots or a file marker were accepted");
    check(Spark::FPSAssets::FindAssetRoot({invalid, first, second}) == first / "Assets",
          "Resolver did not skip an invalid marker or preserve search priority");
    check(Spark::FPSAssets::FindAssetRoot({second, first}) == second / "Assets",
          "Resolver ignored reversed search priority");
    std::filesystem::current_path(second);
    check(Spark::FPSAssets::Root() == expected, "Root changed after its first resolution");
    return failures == 0 ? 0 : 1;
}

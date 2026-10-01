/**
 * @file FuzzVirtualFileSystemProduction.cpp
 * @brief libc++-compiled production adapter for the virtual-filesystem libFuzzer harness.
 *
 * Mods, scene manifests and asset references name content by virtual path, and every read
 * goes through Spark::VirtualFileSystem::ReadFile / ReadTextFile, which apply
 * IsVirtualPathSafe and then LocalFileProvider's lexical and link-resolved containment
 * check against the mount root. The adapter mounts a real directory tree once:
 *
 *   <base>/mod/a.txt, <base>/mod/sub/b.bin, <base>/mod/empty.txt   (the mount, priority MOD)
 *   <base>/mod/escape -> ../outside, <base>/mod/secret-link -> ../outside/secret.txt
 *   <base>/game/a.txt                                             (a lower-priority mount)
 *   <base>/outside/secret.txt                                     (a canary outside both)
 *
 * and resolves the fuzz bytes as a virtual path. A violated contract aborts so libFuzzer
 * records a crash:
 *  - nothing outside the mount roots is ever returned (the canary never appears),
 *  - a read returns either nothing or exactly the bytes of one regular fixture file;
 *    a directory, device or other non-regular path reads as nothing,
 *  - the higher-priority mount wins for a path both mounts hold,
 *  - a path IsVirtualPathSafe rejects reads as nothing, and Exists never claims it,
 *  - ReadFile and ReadTextFile agree.
 */

#include "FuzzVirtualFileSystemProduction.h"

#include "Engine/Modding/VirtualFileSystem.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 4096;
    constexpr std::string_view kCanary = "SPARK-VFS-FUZZ-CANARY-OUTSIDE-THE-MOUNT";
    constexpr std::string_view kModAlpha = "mod alpha fixture";
    constexpr std::string_view kModBeta = "mod beta fixture\n\x01\x02";
    constexpr std::string_view kGameAlpha = "game alpha fixture (lower priority)";

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzVirtualFileSystem: VirtualFileSystem violated: %s\n", what);
        std::abort();
    }

    void WriteFixture(const std::filesystem::path& path, std::string_view bytes)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out)
            InvariantFailure("could not write a fixture file");
    }

    /// Build the fixture tree once and mount it; the VFS is a process singleton.
    bool MountFixtures()
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "spark-fuzz-vfs-XXXXXX").string();
        if (::mkdtemp(pattern.data()) == nullptr)
            InvariantFailure("could not create the fixture directory");
        const std::filesystem::path base(pattern);
        const std::filesystem::path mod = base / "mod";
        const std::filesystem::path game = base / "game";
        const std::filesystem::path outside = base / "outside";
        std::filesystem::create_directories(mod / "sub");
        std::filesystem::create_directories(game);
        std::filesystem::create_directories(outside);
        WriteFixture(mod / "a.txt", kModAlpha);
        WriteFixture(mod / "sub" / "b.bin", kModBeta);
        WriteFixture(mod / "empty.txt", "");
        WriteFixture(game / "a.txt", kGameAlpha);
        WriteFixture(outside / "secret.txt", kCanary);
        std::filesystem::create_directory_symlink("../outside", mod / "escape");
        std::filesystem::create_symlink("../outside/secret.txt", mod / "secret-link");

        auto& vfs = Spark::VirtualFileSystem::GetInstance();
        vfs.Initialize();
        vfs.Mount("game", std::make_unique<Spark::LocalFileProvider>(game.string()), Spark::GAME_PRIORITY);
        vfs.Mount("mod", std::make_unique<Spark::LocalFileProvider>(mod.string()), Spark::MOD_PRIORITY);
        return true;
    }

    bool IsFixtureContent(std::string_view bytes)
    {
        return bytes.empty() || bytes == kModAlpha || bytes == kModBeta || bytes == kGameAlpha;
    }
} // namespace

extern "C" int SparkFuzzVirtualFileSystemRead(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
        return 0;
    static const bool mounted = MountFixtures();
    (void)mounted;

    const std::string path = size == 0 ? std::string() : std::string(reinterpret_cast<const char*>(data), size);
    const auto& vfs = Spark::VirtualFileSystem::GetInstance();

    const std::vector<uint8_t> binary = vfs.ReadFile(path);
    const std::string_view bytes(reinterpret_cast<const char*>(binary.data()), binary.size());
    if (bytes.find(kCanary) != std::string_view::npos)
        InvariantFailure("ReadFile returned a file from outside the mount roots");
    if (!IsFixtureContent(bytes))
        InvariantFailure("ReadFile returned bytes that are no regular fixture file's contents");
    if (bytes == kGameAlpha)
        InvariantFailure("a lower-priority mount won over the mod mount");

    const std::string text = vfs.ReadTextFile(path);
    if (text.find(kCanary) != std::string::npos)
        InvariantFailure("ReadTextFile returned a file from outside the mount roots");
    if (text != bytes)
        InvariantFailure("ReadFile and ReadTextFile disagree");

    if (!Spark::IsVirtualPathSafe(path))
    {
        if (!binary.empty())
            InvariantFailure("an unsafe path was read");
        if (vfs.Exists(path))
            InvariantFailure("Exists claims an unsafe path");
    }
    return 0;
}

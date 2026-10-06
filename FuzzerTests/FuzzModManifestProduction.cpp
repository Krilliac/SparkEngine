/**
 * @file FuzzModManifestProduction.cpp
 * @brief libc++-compiled production adapter for the mod-manifest libFuzzer harness.
 *
 * The input is split at its first 0x00 byte into one or two mod.json documents,
 * written as <tmp>/mods/a/mod.json and <tmp>/mods/b/mod.json, and a fresh
 * Spark::ModSystem scans that directory twice with ScanForMods, the discovery path the
 * editor's ModdingPanel (scan and rescan buttons) runs over the untrusted mods tree.
 * The first document is then also offered to LoadConfig. A violation of the scan
 * contract aborts so libFuzzer records a crash rather than a silent pass:
 *  - ScanForMods returns exactly the number of mods GetAllMods() then reports, so
 *    two directories claiming one id can no longer count twice and register once,
 *  - every published id holds 1-128 bytes of [A-Za-z0-9._-] and is not "." or ".."
 *    (checked here, independently of the reader), every dependency follows the same
 *    policy, is not the mod's own id and appears once,
 *  - no two published mods share a directory, and each lies under the scanned root,
 *  - a second scan of the unchanged directory publishes the identical set,
 *  - a LoadConfig rejection leaves GetAllMods() unchanged, and an accepted config
 *    never adds or removes a mod.
 */

#include "FuzzModManifestProduction.h"

#include "Engine/Modding/ModSystem.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unistd.h>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    constexpr std::size_t kMaxInputBytes = 2u * 64u * 1024u + 1u;
    constexpr std::size_t kMaxModIdBytes = 128;
    std::atomic<std::uint64_t> s_directoryCounter{0};

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzModManifest: ModSystem violated: %s\n", what);
        std::abort();
    }

    bool WriteBytes(const fs::path& path, std::string_view bytes)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        file.close();
        return static_cast<bool>(file);
    }

    bool SatisfiesIdPolicy(const std::string& id)
    {
        if (id.empty() || id.size() > kMaxModIdBytes || id == "." || id == "..")
        {
            return false;
        }
        for (const char c : id)
        {
            const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                 c == '.' || c == '_' || c == '-';
            if (!allowed)
            {
                return false;
            }
        }
        return true;
    }

    auto Fields(const Spark::ModInfo& info)
    {
        return std::tie(info.id, info.name, info.author, info.version, info.description, info.path, info.previewImage,
                        info.dependencies, info.enabled, info.loaded, info.loadOrder);
    }

    std::vector<Spark::ModInfo> SortedMods(const Spark::ModSystem& mods)
    {
        std::vector<Spark::ModInfo> all = mods.GetAllMods();
        std::sort(all.begin(), all.end(),
                  [](const Spark::ModInfo& left, const Spark::ModInfo& right) { return left.id < right.id; });
        return all;
    }

    bool SameMods(const std::vector<Spark::ModInfo>& left, const std::vector<Spark::ModInfo>& right)
    {
        return std::equal(left.begin(), left.end(), right.begin(), right.end(),
                          [](const Spark::ModInfo& a, const Spark::ModInfo& b) { return Fields(a) == Fields(b); });
    }

    void CheckPublishedSet(const std::vector<Spark::ModInfo>& mods, std::size_t returned, const fs::path& modsRoot)
    {
        if (returned != mods.size())
        {
            InvariantFailure("ScanForMods returned a count that differs from the registered mods");
        }
        std::set<std::string> paths;
        const std::string rootPrefix = modsRoot.string() + "/";
        for (const Spark::ModInfo& info : mods)
        {
            if (!SatisfiesIdPolicy(info.id))
            {
                InvariantFailure("a published mod id breaks the id policy");
            }
            std::set<std::string> dependencies;
            for (const std::string& dependency : info.dependencies)
            {
                if (!SatisfiesIdPolicy(dependency) || dependency == info.id || !dependencies.insert(dependency).second)
                {
                    InvariantFailure("a published dependency is invalid, the mod itself, or repeated");
                }
            }
            if (info.path.rfind(rootPrefix, 0) != 0 || !paths.insert(info.path).second)
            {
                InvariantFailure("a published mod lies outside the mods root or shares a directory");
            }
            if (info.enabled || info.loaded)
            {
                InvariantFailure("a scan enabled or loaded a mod");
            }
        }
    }

    /// A fresh directory tree for one input; removed with its contents on scope exit.
    class ScopedDirectory
    {
      public:
        ScopedDirectory()
        {
            const auto suffix = s_directoryCounter.fetch_add(1, std::memory_order_relaxed);
            m_path = fs::temp_directory_path() /
                     ("spark-fuzz-mod-manifest-" + std::to_string(getpid()) + "-" + std::to_string(suffix));
            std::error_code error;
            fs::remove_all(m_path, error);
            m_created = fs::create_directory(m_path, error) && !error;
        }
        ~ScopedDirectory()
        {
            std::error_code ignored;
            fs::remove_all(m_path, ignored);
        }
        ScopedDirectory(const ScopedDirectory&) = delete;
        ScopedDirectory& operator=(const ScopedDirectory&) = delete;

        bool Created() const { return m_created; }
        const fs::path& Path() const { return m_path; }

      private:
        fs::path m_path;
        bool m_created = false;
    };
} // namespace

extern "C" int SparkFuzzScanMods(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }
    const std::string_view bytes =
        size == 0 ? std::string_view() : std::string_view(reinterpret_cast<const char*>(data), size);
    const std::size_t split = bytes.find('\0');
    const std::string_view first = bytes.substr(0, split);

    ScopedDirectory directory;
    if (!directory.Created())
    {
        return 0;
    }
    const fs::path modsRoot = directory.Path() / "mods";
    std::error_code error;
    fs::create_directories(modsRoot / "a", error);
    if (error || !WriteBytes(modsRoot / "a" / "mod.json", first))
    {
        return 0;
    }
    if (split != std::string_view::npos)
    {
        fs::create_directories(modsRoot / "b", error);
        if (error || !WriteBytes(modsRoot / "b" / "mod.json", bytes.substr(split + 1)))
        {
            return 0;
        }
    }
    // ScanForMods canonicalises the root; compare published paths against that form.
    const fs::path canonicalRoot = fs::canonical(modsRoot, error);
    if (error)
    {
        return 0;
    }

    Spark::ModSystem mods;
    const std::size_t firstCount = mods.ScanForMods(canonicalRoot.string());
    const std::vector<Spark::ModInfo> firstScan = SortedMods(mods);
    CheckPublishedSet(firstScan, firstCount, canonicalRoot);

    const std::size_t secondCount = mods.ScanForMods(canonicalRoot.string());
    const std::vector<Spark::ModInfo> secondScan = SortedMods(mods);
    if (secondCount != firstCount || !SameMods(firstScan, secondScan))
    {
        InvariantFailure("a rescan of an unchanged directory changed the published set");
    }

    const fs::path config = directory.Path() / "mods.config.json";
    if (!WriteBytes(config, first))
    {
        return 0;
    }
    if (!mods.LoadConfig(config.string()))
    {
        if (!SameMods(secondScan, SortedMods(mods)))
        {
            InvariantFailure("a rejected LoadConfig changed the registered mods");
        }
        return 0;
    }
    const std::vector<Spark::ModInfo> configured = SortedMods(mods);
    const bool sameIds = std::equal(configured.begin(), configured.end(), secondScan.begin(), secondScan.end(),
                                    [](const Spark::ModInfo& a, const Spark::ModInfo& b) { return a.id == b.id; });
    if (!sameIds)
    {
        InvariantFailure("an accepted LoadConfig added or removed a mod");
    }
    return 0;
}

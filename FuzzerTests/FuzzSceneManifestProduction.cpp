/**
 * @file FuzzSceneManifestProduction.cpp
 * @brief libc++-compiled production adapter for the .sparkscene libFuzzer harness.
 *
 * The fuzz input is handed to the shipped SceneManifest::ParseFromString, and
 * the parsed manifest is checked against the guarantees the streaming loaders
 * rely on. A violation aborts so libFuzzer records it as a crash rather than a
 * silent pass:
 *  - every accepted asset path stays inside the mount root under Windows
 *    separator semantics, decided by an independent lexical walk here (both
 *    '/' and a backslash split components; '..' may never climb above the root; no
 *    leading separator, no ':' drive/stream form), not by the parser's own
 *    filter, so a regression in IsVirtualPathSafe cannot hide itself,
 *  - no accepted path contains a control byte (an embedded NUL would make every
 *    c_str() consumer open a different file from the one that was validated),
 *  - every accepted path also passes the production IsVirtualPathSafe policy,
 *    which additionally rejects reserved device names,
 *  - the entry count never exceeds MAX_SCENE_MANIFEST_ENTRIES.
 *
 * The 8 MB byte cap and the 100,000-entry cap lie far above the smoke's
 * -max_len, so their boundary behaviour is pinned by the SceneManifest_*Cap*
 * tests in Tests/TestAreaAssetLoader.cpp; this target proves the parser stays
 * inside its invariants for arbitrary bytes below them.
 */

#include "FuzzSceneManifestProduction.h"

#include "Engine/Modding/VirtualFileSystem.h"
#include "Engine/Streaming/SceneManifest.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzSceneManifest: ParseFromString produced a manifest that violates: %s\n", what);
        std::abort();
    }

    /// Containment decided without std::filesystem, so it holds identically on
    /// every host: components are split on both separators and '..' may never
    /// climb above the root.
    bool StaysInsideRootOnEveryHost(const std::string& path)
    {
        if (path.empty() || path.front() == '/' || path.front() == '\\')
            return false;
        if (path.find(':') != std::string::npos)
            return false;

        std::size_t depth = 0;
        std::size_t componentStart = 0;
        while (componentStart <= path.size())
        {
            std::size_t componentEnd = path.find_first_of("/\\", componentStart);
            if (componentEnd == std::string::npos)
                componentEnd = path.size();

            const std::string_view component(path.data() + componentStart, componentEnd - componentStart);
            if (component == "..")
            {
                if (depth == 0)
                    return false;
                --depth;
            }
            else if (!component.empty() && component != ".")
            {
                ++depth;
            }
            componentStart = componentEnd + 1;
        }
        return true;
    }

    bool ContainsControlByte(const std::string& path)
    {
        for (const char c : path)
        {
            const auto byte = static_cast<unsigned char>(c);
            if (byte < 0x20 || byte == 0x7F)
                return true;
        }
        return false;
    }

    void CheckPaths(const std::vector<std::string>& paths)
    {
        for (const std::string& path : paths)
        {
            if (!StaysInsideRootOnEveryHost(path))
                InvariantFailure("asset path containment under Windows separator semantics");
            if (ContainsControlByte(path))
                InvariantFailure("asset path contains a control byte");
            if (!Spark::IsVirtualPathSafe(path))
                InvariantFailure("asset path containment (IsVirtualPathSafe)");
        }
    }

    void CheckManifest(const Spark::Streaming::SceneManifest& manifest)
    {
        if (manifest.TotalAssetCount() > Spark::Streaming::MAX_SCENE_MANIFEST_ENTRIES)
            InvariantFailure("entry cap");

        CheckPaths(manifest.meshPaths);
        CheckPaths(manifest.texturePaths);
        CheckPaths(manifest.audioPaths);
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production parser, VFS policy and logger sources.
extern "C" int SparkFuzzParseSceneManifest(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr && size != 0)
        return 0;

    const std::string content = size == 0 ? std::string() : std::string(reinterpret_cast<const char*>(data), size);
    const Spark::Streaming::SceneManifest manifest = Spark::Streaming::SceneManifest::ParseFromString(content);
    CheckManifest(manifest);
    return 0;
}

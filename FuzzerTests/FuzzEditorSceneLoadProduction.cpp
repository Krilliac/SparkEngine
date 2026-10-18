/**
 * @file FuzzEditorSceneLoadProduction.cpp
 * @brief libc++-compiled production adapter for the editor scene load libFuzzer harness.
 *
 * SceneManager opens scenes through SceneSerializer::LoadScene, which dispatches on the
 * file extension, reads the file through one bounded handle and decodes it with
 * DecodeSceneJSONDocument. The first fuzz byte picks the file name (JSON spellings in
 * several cases, the retired binary ".scene", other and missing extensions); the rest is
 * written to that file in a private directory and loaded over a live, non-default scene.
 * A violated contract aborts so libFuzzer records a crash:
 *  - DetectFormat treats exactly .sparkscene, .json and .scenejson (any case) as JSON;
 *    every other name is refused and leaves the caller's scene untouched;
 *  - for a JSON name, LoadScene's verdict and scene equal DecodeSceneJSONDocument's on the
 *    same bytes, a refused load leaves the caller's scene untouched, and an accepted one
 *    reports the file's size;
 *  - IsSceneFile recognizes exactly the files whose first four bytes are the scene magic
 *    or whose first byte is '{';
 *  - an accepted scene saves through SaveScene and loads back as the identical scene.
 */

#include "FuzzEditorSceneLoadProduction.h"

#include "FuzzEditorSceneFingerprint.h"
#include "SceneSystem/SceneJSONReader.h"
#include "SceneSystem/SceneSerializer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    constexpr std::array<const char*, 12> kNames = {"scene.sparkscene",
                                                    "scene.json",
                                                    "scene.scenejson",
                                                    "scene.SparkScene",
                                                    "scene.JSON",
                                                    "scene.SCENEJSON",
                                                    "scene.scene",
                                                    "scene.bin",
                                                    "scene",
                                                    "scene.json.bak",
                                                    "scene.sparkscene.tmp",
                                                    "scene."};

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzEditorSceneLoad: SceneSerializer::LoadScene violated: %s\n", what);
        std::abort();
    }

    const std::filesystem::path& FixtureDirectory()
    {
        static const std::filesystem::path directory = []
        {
            std::string pattern = (std::filesystem::temp_directory_path() / "spark-fuzz-editor-scene-XXXXXX").string();
            if (::mkdtemp(pattern.data()) == nullptr)
            {
                InvariantFailure("could not create the fixture directory");
            }
            return std::filesystem::path(pattern);
        }();
        return directory;
    }

    void WriteFile(const std::filesystem::path& path, std::string_view bytes)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out)
        {
            InvariantFailure("could not write the fixture file");
        }
    }

    /// The extension table, restated independently of DetectFormat.
    bool NamesJson(std::string_view name)
    {
        const std::size_t dot = name.rfind('.');
        if (dot == std::string_view::npos)
        {
            return false;
        }
        std::string extension(name.substr(dot));
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return extension == ".sparkscene" || extension == ".json" || extension == ".scenejson";
    }

    bool LooksLikeSceneFile(std::string_view bytes)
    {
        if (bytes.size() < sizeof(std::uint32_t))
        {
            return false;
        }
        std::uint32_t magic = 0;
        std::memcpy(&magic, bytes.data(), sizeof(magic));
        return magic == SparkEditor::SCENE_FILE_MAGIC || bytes.front() == '{';
    }

    std::string Print(const SparkEditor::SceneFile& scene)
    {
        return SparkFuzzEditorScene::Fingerprint(scene, &SparkEditor::IsValidSceneUTF8);
    }
} // namespace

extern "C" int SparkFuzzLoadEditorScene(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0) || size == 0)
    {
        return 0;
    }
    const std::string_view name = kNames[data[0] % kNames.size()];
    const std::string bytes(reinterpret_cast<const char*>(data) + 1, size - 1);

    static const SparkEditor::SceneFile sentinel = SparkFuzzEditorScene::SentinelScene();
    static const std::string sentinelPrint = Print(sentinel);

    const std::filesystem::path& directory = FixtureDirectory();
    const std::filesystem::path path = directory / std::string(name);
    WriteFile(path, bytes);

    SparkEditor::SceneSerializer serializer;
    SparkEditor::SceneFile scene = sentinel;
    const SparkEditor::SerializationResult result = serializer.LoadScene(path.string(), scene);
    const bool json =
        SparkEditor::SceneSerializer::DetectFormat(path.string()) == SparkEditor::SerializationFormat::JSON;
    if (json != NamesJson(name))
    {
        InvariantFailure("DetectFormat disagrees with the .sparkscene/.json/.scenejson table");
    }
    if (SparkEditor::SceneSerializer::IsSceneFile(path.string()) != LooksLikeSceneFile(bytes))
    {
        InvariantFailure("IsSceneFile disagrees with the magic and '{' sniff");
    }

    if (!json)
    {
        if (result.success || Print(scene) != sentinelPrint)
        {
            InvariantFailure("a file without a JSON scene extension was loaded or changed the caller's scene");
        }
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return 0;
    }

    SparkEditor::SceneFile direct = sentinel;
    SparkEditor::SerializationResult directResult;
    const bool decoded = !bytes.empty() && SparkEditor::DecodeSceneJSONDocument(bytes, direct, directResult);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    if (result.success != decoded)
    {
        InvariantFailure("LoadScene and DecodeSceneJSONDocument disagree about the same bytes");
    }
    if (Print(scene) != Print(decoded ? direct : sentinel))
    {
        InvariantFailure("LoadScene produced a different scene than DecodeSceneJSONDocument");
    }
    if (!decoded)
    {
        if (result.errorMessage.empty())
        {
            InvariantFailure("a refused load carries no error message");
        }
        return 0;
    }
    if (result.bytesProcessed != bytes.size())
    {
        InvariantFailure("an accepted load reports a different byte count than the file holds");
    }

    const std::filesystem::path saved = directory / "saved.sparkscene";
    const SparkEditor::SerializationResult saveResult = serializer.SaveScene(scene, saved.string());
    if (!saveResult.success)
    {
        InvariantFailure("an accepted scene could not be saved");
    }
    SparkEditor::SceneFile reloaded = sentinel;
    if (!serializer.LoadScene(saved.string(), reloaded).success)
    {
        InvariantFailure("a saved scene could not be loaded again");
    }
    if (Print(reloaded) != Print(scene))
    {
        InvariantFailure("load -> save -> load changed the scene");
    }
    return 0;
}

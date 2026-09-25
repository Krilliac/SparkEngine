/**
 * @file TestAssetManifestReal.cpp
 * @brief RDY-020: every asset the SparkGameFPS module references must be
 *        declared in Assets/assets.integrity.json with its exact case and must
 *        resolve through the production FPS asset root on a case-sensitive
 *        filesystem.
 *
 * The references are not a hand-maintained list. They are the string literals
 * compiled into GameModules/SparkGameFPS/Source (read from the production
 * files at test time) plus every model/material the production SceneManager
 * parses out of Assets/Scenes/level1.scene. Paths are resolved with the
 * production Spark::FPSAssets::FindAssetRoot / Resolve / ResolveScenePath, so
 * a reference added to the module or the scene is covered without editing
 * this file.
 *
 * Case matters because Windows and macOS developer machines are
 * case-insensitive: "Models/Pistol.obj" loads there and silently renders an
 * empty mesh in a Linux package. The exact-case check below compares every
 * path component against the real directory listing, so it fails the same way
 * on every host.
 */

#include "TestFramework.h"

#include "Game/FPSAssetPaths.h"

#include "Graphics/GraphicsEngine.h"
#include "Input/InputManager.h"
#include "SceneManager/SceneManager.h"

#include <nlohmann_json.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    const fs::path& SourceRoot()
    {
        static const fs::path root(SPARK_TEST_SOURCE_DIR);
        return root;
    }

    const fs::path& FPSSourceDirectory()
    {
        static const fs::path directory = SourceRoot() / "GameModules" / "SparkGameFPS" / "Source";
        return directory;
    }

    std::string ReadFile(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    /// Paths declared by the committed root integrity manifest, exactly as written.
    std::set<std::string> LoadManifestPaths()
    {
        std::set<std::string> paths;
        const std::string text = ReadFile(SourceRoot() / "Assets" / "assets.integrity.json");
        nlohmann::json manifest;
        try
        {
            manifest = nlohmann::json::parse(text);
        }
        catch (const std::exception&)
        {
            return paths;
        }
        if (!manifest.is_object() || !manifest.contains("entries") || !manifest["entries"].is_array())
            return paths;

        for (const auto& entry : manifest["entries"])
        {
            if (entry.is_object() && entry.contains("path") && entry["path"].is_string())
                paths.insert(entry["path"].get<std::string>());
        }
        return paths;
    }

    /// Top-level directories of the manifest ("Models", "Scenes", ...).
    std::set<std::string> ManifestRootDirectories(const std::set<std::string>& manifestPaths)
    {
        std::set<std::string> roots;
        for (const std::string& path : manifestPaths)
        {
            const std::size_t slash = path.find('/');
            if (slash != std::string::npos && slash > 0)
                roots.insert(path.substr(0, slash));
        }
        return roots;
    }

    bool IsIdentifierChar(const char c)
    {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
    }

    /**
     * Every string literal in a C++ translation unit, skipping comments and
     * character literals. Encoding prefixes (L, u8, u, U) and raw strings are
     * handled, so a path in a doc comment or example never counts as a
     * reference while a path in code always does.
     */
    std::vector<std::string> ExtractStringLiterals(const std::string& source)
    {
        std::vector<std::string> literals;
        std::size_t i = 0;
        const std::size_t size = source.size();

        const auto readQuoted = [&](const char quote) -> std::string
        {
            std::string value;
            ++i; // opening quote
            while (i < size && source[i] != quote && source[i] != '\n')
            {
                if (source[i] == '\\' && i + 1 < size)
                {
                    value += source[i];
                    ++i;
                }
                value += source[i];
                ++i;
            }
            ++i; // closing quote
            return value;
        };

        while (i < size)
        {
            const char c = source[i];
            if (c == '/' && i + 1 < size && source[i + 1] == '/')
            {
                while (i < size && source[i] != '\n')
                    ++i;
            }
            else if (c == '/' && i + 1 < size && source[i + 1] == '*')
            {
                const std::size_t end = source.find("*/", i + 2);
                i = end == std::string::npos ? size : end + 2;
            }
            else if (c == '"')
            {
                literals.push_back(readQuoted('"'));
            }
            else if (c == '\'')
            {
                readQuoted('\'');
            }
            else if (std::isdigit(static_cast<unsigned char>(c)) != 0)
            {
                // Numbers may carry digit separators (1'000); swallow them whole.
                while (i < size && (IsIdentifierChar(source[i]) || source[i] == '.' || source[i] == '\''))
                    ++i;
            }
            else if (IsIdentifierChar(c))
            {
                const std::size_t start = i;
                while (i < size && IsIdentifierChar(source[i]))
                    ++i;
                const std::string_view identifier(source.data() + start, i - start);
                const bool rawPrefix = identifier == "R" || identifier == "LR" || identifier == "uR" ||
                                       identifier == "UR" || identifier == "u8R";
                if (rawPrefix && i < size && source[i] == '"')
                {
                    const std::size_t open = source.find('(', i + 1);
                    if (open == std::string::npos)
                        break;
                    const std::string terminator = ")" + source.substr(i + 1, open - i - 1) + "\"";
                    const std::size_t close = source.find(terminator, open + 1);
                    if (close == std::string::npos)
                        break;
                    literals.push_back(source.substr(open + 1, close - open - 1));
                    i = close + terminator.size();
                }
                // L"..", u8"..": the loop reaches the quote on its next pass.
            }
            else
            {
                ++i;
            }
        }
        return literals;
    }

    /**
     * Map a literal onto a manifest-relative path when it names a file below an
     * asset root: "Models/pistol.obj" and "Assets/Materials/Wood.json" qualify,
     * the directory prefix "Scenes/" and the include "Audio/MusicManager.h" do not.
     */
    bool AsAssetReference(const std::string& literal, const std::set<std::string>& roots, std::string& reference)
    {
        constexpr std::string_view kAssetsPrefix = "Assets/";
        std::string candidate = literal;
        if (candidate.starts_with(kAssetsPrefix))
            candidate.erase(0, kAssetsPrefix.size());

        const std::size_t slash = candidate.find('/');
        if (slash == std::string::npos || !roots.contains(candidate.substr(0, slash)))
            return false;

        const std::string extension = fs::path(candidate).extension().string();
        if (extension.empty() || extension == ".h" || extension == ".hpp" || extension == ".cpp" || extension == ".inl")
            return false;

        reference = candidate;
        return true;
    }

    struct SourceReference
    {
        std::string path;   ///< Manifest-relative, e.g. "Models/pistol.obj".
        std::string origin; ///< Production file that names it.
    };

    std::vector<SourceReference> CollectFPSSourceReferences(const std::set<std::string>& roots)
    {
        std::vector<SourceReference> references;
        std::error_code error;
        for (fs::recursive_directory_iterator it(FPSSourceDirectory(), error), end; !error && it != end;
             it.increment(error))
        {
            const fs::path& file = it->path();
            const std::string extension = file.extension().string();
            if (!it->is_regular_file(error) || (extension != ".cpp" && extension != ".h"))
                continue;

            for (const std::string& literal : ExtractStringLiterals(ReadFile(file)))
            {
                std::string reference;
                if (AsAssetReference(literal, roots, reference))
                    references.push_back({reference, file.lexically_relative(SourceRoot()).generic_string()});
            }
        }
        return references;
    }

    /// Every model and material path the production scene parser keeps for level1.scene.
    std::vector<std::string> CollectLevel1SceneReferences(bool& loaded)
    {
        GraphicsEngine graphics;
        InputManager input;
        SceneManager scene(&graphics, &input);
        // The same call Game::Initialize makes for the shipped level.
        loaded = scene.LoadScene(Spark::FPSAssets::Resolve(L"Scenes/level1.scene"));

        std::vector<std::string> references;
        for (int index = 0; index < scene.GetNodeCount(); ++index)
        {
            const SceneNode* node = scene.GetNode(index);
            if (node == nullptr)
                continue;
            if (!node->modelPath.empty())
                references.push_back(node->modelPath);
            if (!node->materialPath.empty())
                references.push_back(node->materialPath);
        }
        return references;
    }

    /**
     * True only when every component of @p relative names an existing entry
     * with byte-identical case and the leaf is a regular, non-symlink file.
     * Traversal and empty components are rejected outright, so the result is
     * the same on case-sensitive and case-insensitive filesystems.
     */
    bool ExistsWithExactCase(const fs::path& root, const std::string& relative)
    {
        if (relative.empty() || relative.front() == '/' || relative.find('\\') != std::string::npos)
            return false;

        fs::path current = root;
        std::size_t start = 0;
        while (start <= relative.size())
        {
            const std::size_t slash = relative.find('/', start);
            const std::string component =
                relative.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            if (component.empty() || component == "." || component == "..")
                return false;

            std::error_code error;
            bool found = false;
            for (fs::directory_iterator it(current, error), end; !error && it != end; it.increment(error))
            {
                if (it->path().filename().string() == component)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
                return false;
            current /= component;
            if (slash == std::string::npos)
                break;
            start = slash + 1;
        }

        std::error_code error;
        return fs::is_regular_file(fs::symlink_status(current, error));
    }

    std::string JoinSorted(std::set<std::string> values)
    {
        std::string joined;
        for (const std::string& value : values)
            joined += (joined.empty() ? "" : ", ") + value;
        return joined;
    }

    bool FilesystemIsCaseSensitive(const fs::path& assetRoot)
    {
        std::error_code error;
        return fs::is_directory(assetRoot / "Models", error) && !fs::exists(assetRoot / "MODELS", error);
    }
} // namespace

// ============================================================================
// AssetManifest_*: production references are declared with exact case
// ============================================================================

TEST(AssetManifest_FPSProductionSourceReferencesAreDeclared)
{
    const std::set<std::string> manifest = LoadManifestPaths();
    ASSERT_TRUE(!manifest.empty());

    const std::vector<SourceReference> references = CollectFPSSourceReferences(ManifestRootDirectories(manifest));
    std::set<std::string> referenced;
    std::set<std::string> undeclared;
    for (const SourceReference& reference : references)
    {
        referenced.insert(reference.path);
        if (!manifest.contains(reference.path))
            undeclared.insert(reference.path + " (" + reference.origin + ")");
    }

    // Anchors prove the extractor sees the module's real load calls
    // (Game.cpp's level load, Player.cpp's weapon models, the arena builder's
    // materials, the music tracks); an extractor that returns nothing must not
    // pass as "no undeclared references".
    EXPECT_TRUE(referenced.contains("Scenes/level1.scene"));
    EXPECT_TRUE(referenced.contains("Models/pistol.obj"));
    EXPECT_TRUE(referenced.contains("Models/rifle.obj"));
    EXPECT_TRUE(referenced.contains("Materials/Terrain_Dirt.json"));
    EXPECT_TRUE(referenced.contains("Audio/music_combat.wav"));
    // Paths in comments and #include lines are not references.
    EXPECT_FALSE(referenced.contains("Scenes/x.scene"));
    EXPECT_FALSE(referenced.contains("Audio/MusicManager.h"));

    EXPECT_EQ(JoinSorted(undeclared), std::string());
}

TEST(AssetManifest_Level1SceneReferencesAreDeclared)
{
    const std::set<std::string> manifest = LoadManifestPaths();
    ASSERT_TRUE(!manifest.empty());

    bool loaded = false;
    const std::vector<std::string> references = CollectLevel1SceneReferences(loaded);
    ASSERT_TRUE(loaded);
    // level1.scene authors a material on every visible object; an empty list
    // means the parser dropped them, not that the scene is clean.
    ASSERT_TRUE(!references.empty());

    constexpr std::string_view kAssetsPrefix = "Assets/";
    std::set<std::string> undeclared;
    for (const std::string& reference : references)
    {
        // Scene paths are project-relative ("Assets/Materials/Wood.json");
        // GameObject resolves them against the directory above the asset root.
        if (!reference.starts_with(kAssetsPrefix) || !manifest.contains(reference.substr(kAssetsPrefix.size())))
            undeclared.insert(reference);
    }
    EXPECT_EQ(JoinSorted(undeclared), std::string());
}

// ============================================================================
// PathCase_*: the production asset root resolves them with exact case
// ============================================================================

TEST(PathCase_FPSReferencesResolveWithExactCase)
{
    const std::set<std::string> manifest = LoadManifestPaths();
    ASSERT_TRUE(!manifest.empty());

    // The repository layout is one of the layouts FindAssetRoot accepts.
    const fs::path sourceAssets = Spark::FPSAssets::FindAssetRoot({SourceRoot()});
    ASSERT_TRUE(!sourceAssets.empty());
    EXPECT_TRUE(fs::equivalent(sourceAssets, SourceRoot() / "Assets"));
    ASSERT_TRUE(Spark::FPSAssets::RootExists());
    const fs::path& runtimeAssets = Spark::FPSAssets::Root();

    std::set<std::string> references;
    for (const SourceReference& reference : CollectFPSSourceReferences(ManifestRootDirectories(manifest)))
        references.insert(reference.path);
    bool loaded = false;
    for (const std::string& reference : CollectLevel1SceneReferences(loaded))
        references.insert(reference.starts_with("Assets/") ? reference.substr(7) : reference);
    ASSERT_TRUE(loaded);
    ASSERT_TRUE(references.size() >= 5u);

    std::set<std::string> sourceMismatches;
    std::set<std::string> runtimeMismatches;
    for (const std::string& reference : references)
    {
        if (!ExistsWithExactCase(sourceAssets, reference))
            sourceMismatches.insert(reference);

        // Exactly what the module passes to its loaders.
        const fs::path resolved(Spark::FPSAssets::Resolve(fs::path(reference).wstring()));
        fs::path expected = runtimeAssets / fs::path(reference);
        expected.make_preferred();
        std::error_code error;
        if (resolved != expected || !fs::is_regular_file(resolved, error) ||
            !ExistsWithExactCase(runtimeAssets, reference))
            runtimeMismatches.insert(reference);
    }
    EXPECT_EQ(JoinSorted(sourceMismatches), std::string());
    EXPECT_EQ(JoinSorted(runtimeMismatches), std::string());

    // The console scene_load path accepts the shipped level by every documented spelling.
    for (const std::string spelling : {"level1.scene", "Scenes/level1.scene", "Assets/Scenes/level1.scene"})
    {
        fs::path scenePath;
        std::string sceneError;
        EXPECT_TRUE(Spark::FPSAssets::ResolveScenePath(spelling, scenePath, sceneError));
        EXPECT_EQ(sceneError, std::string());
    }
}

TEST(PathCase_CaseAlteredAndTraversalReferencesAreRejected)
{
    const std::set<std::string> manifest = LoadManifestPaths();
    ASSERT_TRUE(!manifest.empty());
    const fs::path sourceAssets = SourceRoot() / "Assets";
    ASSERT_TRUE(manifest.contains("Models/pistol.obj"));
    ASSERT_TRUE(ExistsWithExactCase(sourceAssets, "Models/pistol.obj"));

    // Case-altered spellings of a real asset: undeclared, and not found by an
    // exact-case lookup even where the host filesystem would open them.
    for (const std::string altered : {"Models/Pistol.obj", "models/pistol.obj", "Models/pistol.OBJ"})
    {
        EXPECT_FALSE(manifest.contains(altered));
        EXPECT_FALSE(ExistsWithExactCase(sourceAssets, altered));
    }

    // Traversal and non-canonical spellings of the same asset are rejected too.
    for (const std::string traversal : {"Models/../Models/pistol.obj", "./Models/pistol.obj", "Models//pistol.obj",
                                        "Models\\pistol.obj", "/Models/pistol.obj", "../Assets/Models/pistol.obj"})
    {
        EXPECT_FALSE(manifest.contains(traversal));
        EXPECT_FALSE(ExistsWithExactCase(sourceAssets, traversal));
    }

    // The production scene resolver refuses traversal, absolute and drive paths
    // before touching the filesystem.
    for (const std::string rejected :
         {"../Scenes/level1.scene", "Scenes/../../Scenes/level1.scene", "/Scenes/level1.scene",
          "C:/Assets/Scenes/level1.scene", "..\\Scenes\\level1.scene"})
    {
        fs::path scenePath;
        std::string sceneError;
        EXPECT_FALSE(Spark::FPSAssets::ResolveScenePath(rejected, scenePath, sceneError));
        EXPECT_FALSE(sceneError.empty());
        EXPECT_TRUE(scenePath.empty());
    }

    // On a case-sensitive host the production resolvers must not find a
    // case-altered file; that is the failure a Linux package would hit.
    if (FilesystemIsCaseSensitive(Spark::FPSAssets::Root()))
    {
        std::error_code error;
        EXPECT_FALSE(fs::exists(fs::path(Spark::FPSAssets::Resolve(L"Models/Pistol.obj")), error));
        fs::path scenePath;
        std::string sceneError;
        EXPECT_FALSE(Spark::FPSAssets::ResolveScenePath("Level1.scene", scenePath, sceneError));
        EXPECT_EQ(sceneError, std::string("scene file does not exist"));
    }
}

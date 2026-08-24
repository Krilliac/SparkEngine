// TestFileUtils.cpp - Tests for file I/O and path utilities
// Uses the actual FileUtils.h header

#include "TestFramework.h"
#include "Graphics/ProjectAssetPath.h"
#include "Utils/FileUtils.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifdef SPARK_PLATFORM_WINDOWS
#include "Graphics/GraphicsEngine.h"
#include <d3d11.h>
#include <objbase.h>
#include <wrl/client.h>
#endif

using namespace Spark::FileUtils;

static std::string GetTempDir()
{
    return std::filesystem::temp_directory_path().string();
}

namespace
{
    std::string PathToTestUtf8(const std::filesystem::path& path)
    {
        const std::u8string utf8 = path.generic_u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }

    class ScopedAssetProjectFixture
    {
      public:
        explicit ScopedAssetProjectFixture(const char* tag)
        {
            const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
            base = std::filesystem::temp_directory_path() /
                   (std::string("spark-project-assets-") + tag + "-" + std::to_string(stamp));
            root = base / std::filesystem::u8path("Project Caf\xC3\xA9 \xF0\x9F\x9A\x80");
            secondRoot = base / "SecondProject";
            outside = base / "Outside";
            std::filesystem::create_directories(root / "Assets" / "Textures");
            std::filesystem::create_directories(secondRoot / "Assets" / "Textures");
            std::filesystem::create_directories(secondRoot / "Assets" / "Materials");
            std::filesystem::create_directories(outside);
        }

        ~ScopedAssetProjectFixture()
        {
            std::error_code ec;
            std::filesystem::remove_all(base, ec);
        }

        ScopedAssetProjectFixture(const ScopedAssetProjectFixture&) = delete;
        ScopedAssetProjectFixture& operator=(const ScopedAssetProjectFixture&) = delete;

        std::filesystem::path base;
        std::filesystem::path root;
        std::filesystem::path secondRoot;
        std::filesystem::path outside;
    };

    bool WriteOnePixelBmp(const std::filesystem::path& path, uint8_t red, uint8_t green, uint8_t blue)
    {
        std::array<uint8_t, 58> bmp = {0x42, 0x4D, 0x3A, 0x00, 0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x36, 0x00,
                                       0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x01, 0x00,  0x00, 0x00, 0x01, 0x00,
                                       0x00, 0x00, 0x01, 0x00, 0x20, 0x00, 0x00, 0x00,  0x00, 0x00, 0x04, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, blue, green, red,  0xFF};
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bmp.data()), static_cast<std::streamsize>(bmp.size()));
        return output.good();
    }

#ifdef SPARK_PLATFORM_WINDOWS
    class ScopedComInitialization
    {
      public:
        ScopedComInitialization()
        {
            const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            m_ownsInitialization = result == S_OK || result == S_FALSE;
        }

        ~ScopedComInitialization()
        {
            if (m_ownsInitialization)
                CoUninitialize();
        }

      private:
        bool m_ownsInitialization = false;
    };
#endif
} // namespace

// =============================================================================
// Path Manipulation Tests
// =============================================================================

TEST(FileUtils_GetExtension)
{
    EXPECT_EQ(GetExtension("model.fbx"), std::string(".fbx"));
    EXPECT_EQ(GetExtension("archive.tar.gz"), std::string(".gz"));
    EXPECT_EQ(GetExtension("noext"), std::string(""));
}

TEST(FileUtils_GetFilename)
{
    EXPECT_EQ(GetFilename("path/to/model.fbx"), std::string("model.fbx"));
    EXPECT_EQ(GetFilename("model.fbx"), std::string("model.fbx"));
}

TEST(FileUtils_GetStem)
{
    EXPECT_EQ(GetStem("path/to/model.fbx"), std::string("model"));
    EXPECT_EQ(GetStem("noext"), std::string("noext"));
}

TEST(FileUtils_GetDirectory)
{
    EXPECT_EQ(GetDirectory("path/to/model.fbx"), std::string("path/to"));
}

TEST(FileUtils_JoinPath)
{
    std::string joined = JoinPath("assets", "model.fbx");
    // Should contain both parts with a separator
    EXPECT_TRUE(joined.contains("assets"));
    EXPECT_TRUE(joined.contains("model.fbx"));
}

#if SPARK_HAS_FILESYSTEM

TEST(FileUtils_ChangeExtension)
{
    std::string changed = ChangeExtension("model.fbx", ".obj");
    EXPECT_TRUE(changed.contains(".obj"));
    EXPECT_TRUE(!changed.contains(".fbx"));
}

TEST(FileUtils_NormalizePath)
{
    std::string norm = NormalizePath("a/b/../c");
    EXPECT_TRUE(!norm.contains(".."));
}

#endif

// =============================================================================
// File I/O Tests
// =============================================================================

TEST(FileUtils_WriteAndReadText)
{
    std::string path = GetTempDir() + "/spark_test_text.txt";
    std::string content = "Hello, SparkEngine!\nLine 2.";

    EXPECT_TRUE(WriteTextFile(path, content));

    auto result = ReadTextFile(path);
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), content);

    std::remove(path.c_str());
}

TEST(FileUtils_WriteAndReadBinary)
{
    std::string path = GetTempDir() + "/spark_test_binary.bin";
    std::vector<uint8_t> data = {0x00, 0x01, 0x02, 0xFF, 0xFE, 0xFD};

    EXPECT_TRUE(WriteBinaryFile(path, data));

    auto result = ReadBinaryFile(path);
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ((int)result.value().size(), (int)data.size());
    for (int i = 0; i < (int)data.size(); ++i)
    {
        EXPECT_EQ(result.value()[i], data[i]);
    }

    std::remove(path.c_str());
}

TEST(FileUtils_ReadNonexistent)
{
    auto result = ReadTextFile(GetTempDir() + "/spark_nonexistent_file_xyz.txt");
    EXPECT_FALSE(result.has_value());
}

#if SPARK_HAS_FILESYSTEM

TEST(FileUtils_FileExists)
{
    std::string path = GetTempDir() + "/spark_test_exists.txt";
    WriteTextFile(path, "test");
    EXPECT_TRUE(FileExists(path));
    std::remove(path.c_str());
    EXPECT_FALSE(FileExists(path));
}

TEST(FileUtils_IsDirectory)
{
    EXPECT_TRUE(IsDirectory(GetTempDir()));
    EXPECT_FALSE(IsDirectory(GetTempDir() + "/spark_nonexistent_dir_xyz"));
}

TEST(FileUtils_GetFileSize)
{
    std::string path = GetTempDir() + "/spark_test_size.txt";
    WriteTextFile(path, "12345");

    auto size = GetFileSize(path);
    EXPECT_TRUE(size.has_value());
    EXPECT_EQ(size.value(), (uintmax_t)5);

    std::remove(path.c_str());
}

TEST(FileUtils_CreateDirectories)
{
    std::string dir = GetTempDir() + "/spark_test_dir/sub/deep";
    EXPECT_TRUE(CreateDirectories(dir));
    EXPECT_TRUE(IsDirectory(dir));

    // Cleanup
    std::error_code ec;
    fs::remove_all(GetTempDir() + "/spark_test_dir", ec);
}

TEST(FileUtils_ListFiles)
{
    std::string dir = GetTempDir() + "/spark_test_list";
    CreateDirectories(dir);
    WriteTextFile(dir + "/a.txt", "a");
    WriteTextFile(dir + "/b.txt", "b");
    WriteTextFile(dir + "/c.dat", "c");

    auto all = ListFiles(dir);
    EXPECT_EQ((int)all.size(), 3);

    auto txtOnly = ListFiles(dir, ".txt");
    EXPECT_EQ((int)txtOnly.size(), 2);

    // Cleanup
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(ProjectAssetPath_ConfinesUnicodeAssetsAndDerivesSceneRoot)
{
    ScopedAssetProjectFixture fixture("resolver");
    const std::string relativeUnicode = "Assets/Textures/na\xC3\xAFve.bmp";
    const std::filesystem::path unicodeFile = fixture.root / std::filesystem::u8path(relativeUnicode);
    EXPECT_TRUE(WriteOnePixelBmp(unicodeFile, 10, 20, 30));

    const std::string rootUtf8 = PathToTestUtf8(fixture.root);
    const auto resolved = Spark::ResolveProjectAssetPath(rootUtf8, relativeUnicode);
    EXPECT_TRUE(resolved.has_value());
    if (resolved)
    {
        EXPECT_TRUE(std::filesystem::exists(resolved->nativePath));
        EXPECT_TRUE(resolved->cacheKey.find("na\xC3\xAFve.bmp") != std::string::npos);
    }

#ifdef SPARK_PLATFORM_WINDOWS
    const std::filesystem::path caseFile = fixture.root / "Assets" / "Textures" / "CaseIdentity.bmp";
    EXPECT_TRUE(WriteOnePixelBmp(caseFile, 40, 50, 60));
    const auto mixedCase = Spark::ResolveProjectAssetPath(rootUtf8, "Assets/Textures/CaseIdentity.bmp");
    const auto lowerCase = Spark::ResolveProjectAssetPath(rootUtf8, "assets/textures/caseidentity.BMP");
    EXPECT_TRUE(mixedCase.has_value());
    EXPECT_TRUE(lowerCase.has_value());
    if (mixedCase && lowerCase)
        EXPECT_EQ(mixedCase->cacheKey, lowerCase->cacheKey);
#endif

    EXPECT_FALSE(Spark::ResolveProjectAssetPath(rootUtf8, "Assets/../Outside/secret.bmp").has_value());
    EXPECT_FALSE(Spark::ResolveProjectAssetPath(rootUtf8, "../Assets/Textures/secret.bmp").has_value());
    EXPECT_FALSE(Spark::ResolveProjectAssetPath(rootUtf8, "Textures/secret.bmp").has_value());
    EXPECT_FALSE(Spark::ResolveProjectAssetPath(rootUtf8, PathToTestUtf8(fixture.outside / "secret.bmp")).has_value());

    const auto missing = Spark::ResolveProjectAssetPath(rootUtf8, "Assets/Textures/imported-later.bmp");
    EXPECT_TRUE(missing.has_value());
    if (missing)
        EXPECT_FALSE(std::filesystem::exists(missing->nativePath));

    const auto second = Spark::ResolveProjectAssetPath(PathToTestUtf8(fixture.secondRoot), relativeUnicode);
    EXPECT_TRUE(second.has_value());
    if (resolved && second)
        EXPECT_TRUE(resolved->cacheKey != second->cacheKey);

    EXPECT_TRUE(WriteOnePixelBmp(fixture.outside / "secret.bmp", 1, 2, 3));
    std::error_code linkError;
    std::filesystem::create_directory_symlink(fixture.outside, fixture.root / "Assets" / "Escape", linkError);
    if (!linkError)
    {
        EXPECT_FALSE(Spark::ResolveProjectAssetPath(rootUtf8, "Assets/Escape/secret.bmp").has_value());
    }
    else
    {
        std::cout << "[ INFO   ] ProjectAssetPath symlink escape check skipped: " << linkError.message() << "\n";
    }

    const std::filesystem::path scene = fixture.root / "Scenes" / "Nested" / "Level.sparkscene";
    std::filesystem::create_directories(scene.parent_path());
    std::ofstream(scene, std::ios::binary) << "{}";
    const auto derivedRoot = Spark::DeriveProjectRootFromScenePath(PathToTestUtf8(scene));
    const auto canonicalRoot = Spark::CanonicalizeFilesystemPath(rootUtf8);
    EXPECT_TRUE(derivedRoot.has_value());
    EXPECT_TRUE(canonicalRoot.has_value());
    if (derivedRoot && canonicalRoot)
    {
        const auto canonicalDerived = Spark::CanonicalizeFilesystemPath(*derivedRoot);
        EXPECT_TRUE(canonicalDerived.has_value());
        if (canonicalDerived)
            EXPECT_EQ(canonicalDerived->cacheKey, canonicalRoot->cacheKey);
    }

    // A nested folder may itself be named Scenes. Keep walking until the
    // qualifying project-level Scenes ancestor is found instead of rejecting
    // the otherwise valid scene at the first name match.
    const std::filesystem::path nestedScenes = fixture.root / "Scenes" / "Nested" / "Scenes" / "Level.sparkscene";
    std::filesystem::create_directories(nestedScenes.parent_path());
    std::ofstream(nestedScenes, std::ios::binary) << "{}";
    const auto nestedDerivedRoot = Spark::DeriveProjectRootFromScenePath(PathToTestUtf8(nestedScenes));
    EXPECT_TRUE(nestedDerivedRoot.has_value());
    if (nestedDerivedRoot && canonicalRoot)
    {
        const auto canonicalNestedRoot = Spark::CanonicalizeFilesystemPath(*nestedDerivedRoot);
        EXPECT_TRUE(canonicalNestedRoot.has_value());
        if (canonicalNestedRoot)
            EXPECT_EQ(canonicalNestedRoot->cacheKey, canonicalRoot->cacheKey);
    }

    const std::filesystem::path unanchoredScene = fixture.root / "Loose" / "Level.sparkscene";
    std::filesystem::create_directories(unanchoredScene.parent_path());
    std::ofstream(unanchoredScene, std::ios::binary) << "{}";
    EXPECT_FALSE(Spark::DeriveProjectRootFromScenePath(PathToTestUtf8(unanchoredScene)).has_value());
}

#ifdef SPARK_PLATFORM_WINDOWS
TEST(ProjectAssetTextureCache_RetriesFailuresAndSeparatesProjects)
{
    ScopedAssetProjectFixture fixture("texture-cache");
    const std::string relativeUnicode = "Assets/Textures/retry-\xC3\xA9.bmp";
    const auto firstPath = Spark::ResolveProjectAssetPath(PathToTestUtf8(fixture.root), relativeUnicode);
    const auto secondPath = Spark::ResolveProjectAssetPath(PathToTestUtf8(fixture.secondRoot), relativeUnicode);
    EXPECT_TRUE(firstPath.has_value());
    EXPECT_TRUE(secondPath.has_value());
    if (!firstPath || !secondPath)
        return;
    EXPECT_TRUE(firstPath->cacheKey != secondPath->cacheKey);

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel{};
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                       &device, &featureLevel, &context);
    if (FAILED(result))
    {
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                   &featureLevel, &context);
    }
    if (FAILED(result))
    {
        std::cout << "[ INFO   ] D3D11 texture-cache checks skipped: no hardware or WARP device is available.\n";
        return;
    }

    ScopedComInitialization com;
    GraphicsEngine graphics;
    EXPECT_TRUE(SUCCEEDED(graphics.InitializeFromDevice(device.Get(), context.Get())));

    // A failed load is negatively cached to avoid a WIC open and warning every
    // frame. The importer invalidates the exact canonical path when the file arrives.
    EXPECT_TRUE(graphics.GetOrLoadTextureSRV(firstPath->cacheKey) == nullptr);
    EXPECT_TRUE(WriteOnePixelBmp(firstPath->nativePath, 255, 0, 0));
    EXPECT_TRUE(graphics.InvalidateBasicTexture(firstPath->cacheKey));
    ID3D11ShaderResourceView* first = graphics.GetOrLoadTextureSRV(firstPath->cacheKey);
    EXPECT_TRUE(first != nullptr);
    EXPECT_TRUE(graphics.GetOrLoadTextureSRV(firstPath->cacheKey) == first);

    // The same relative asset name in a new project gets a distinct canonical
    // key and resource, while repeated loads inside that project still dedupe.
    EXPECT_TRUE(WriteOnePixelBmp(secondPath->nativePath, 0, 0, 255));
    ID3D11ShaderResourceView* second = graphics.GetOrLoadTextureSRV(secondPath->cacheKey);
    EXPECT_TRUE(second != nullptr);
    EXPECT_TRUE(second != first);
    EXPECT_TRUE(graphics.GetOrLoadTextureSRV(secondPath->cacheKey) == second);

    // Parsed materials stay cached, but each missing declared texture must be
    // retried through the original project-confinement boundary after import.
    const std::filesystem::path materialNative = fixture.root / "Assets" / "Materials" / "retry.json";
    std::filesystem::create_directories(materialNative.parent_path());
    std::ofstream(materialNative, std::ios::binary) << R"({"albedo":"Textures/material-later.bmp"})";
    const auto materialPath =
        Spark::ResolveProjectAssetPath(PathToTestUtf8(fixture.root), "Assets/Materials/retry.json");
    EXPECT_TRUE(materialPath.has_value());
    if (!materialPath)
        return;
    const GraphicsEngine::BasicMaterial* material =
        graphics.GetOrLoadBasicMaterial("Assets/Materials/retry.json", PathToTestUtf8(fixture.root));
    EXPECT_TRUE(material != nullptr);
    EXPECT_TRUE(material && material->srv.Get() == nullptr);

    const std::filesystem::path materialTextureNative = fixture.root / "Assets" / "Textures" / "material-later.bmp";
    EXPECT_TRUE(WriteOnePixelBmp(materialTextureNative, 0, 255, 0));
    EXPECT_TRUE(graphics.InvalidateBasicTexture(PathToTestUtf8(materialTextureNative)));
    const GraphicsEngine::BasicMaterial* retriedMaterial =
        graphics.GetOrLoadBasicMaterial("Assets/Materials/retry.json", PathToTestUtf8(fixture.root));
    EXPECT_TRUE(retriedMaterial == material);
    EXPECT_TRUE(retriedMaterial && retriedMaterial->srv.Get() != nullptr);

    // Identical declared JSON paths in different projects must not share a
    // parsed cache entry or its root-bound texture retry context.
    std::ofstream(fixture.secondRoot / "Assets" / "Materials" / "retry.json", std::ios::binary)
        << R"({"tiling":[7,9]})";
    const GraphicsEngine::BasicMaterial* secondMaterial =
        graphics.GetOrLoadBasicMaterial("Assets/Materials/retry.json", PathToTestUtf8(fixture.secondRoot));
    EXPECT_TRUE(secondMaterial != nullptr);
    EXPECT_TRUE(secondMaterial != material);
    EXPECT_NEAR(secondMaterial ? secondMaterial->tiling.x : 0.0f, 7.0f, 0.0001f);
    EXPECT_NEAR(secondMaterial ? secondMaterial->tiling.y : 0.0f, 9.0f, 0.0001f);

    // Missing material JSONs are also negatively cached until the editor's
    // import/save hook invalidates the declared project-relative identity.
    EXPECT_TRUE(graphics.GetOrLoadBasicMaterial("Assets/Materials/imported.json", PathToTestUtf8(fixture.root)) ==
                nullptr);
    std::ofstream(fixture.root / "Assets" / "Materials" / "imported.json", std::ios::binary) << R"({"tiling":[3,4]})";
    EXPECT_TRUE(graphics.InvalidateBasicMaterial("Assets/Materials/imported.json", PathToTestUtf8(fixture.root)));
    const GraphicsEngine::BasicMaterial* importedMaterial =
        graphics.GetOrLoadBasicMaterial("Assets/Materials/imported.json", PathToTestUtf8(fixture.root));
    EXPECT_TRUE(importedMaterial != nullptr);
    EXPECT_NEAR(importedMaterial ? importedMaterial->tiling.x : 0.0f, 3.0f, 0.0001f);
    EXPECT_NEAR(importedMaterial ? importedMaterial->tiling.y : 0.0f, 4.0f, 0.0001f);
}
#endif

#endif

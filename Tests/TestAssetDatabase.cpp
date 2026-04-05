// TestAssetDatabase.cpp - Tests for asset database logic
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

namespace TestAssetDB
{

    enum class AssetType
    {
        Unknown,
        Texture,
        Model,
        Audio,
        Material,
        Script
    };

    struct AssetMetadata
    {
        std::string path;
        std::string name;
        AssetType type = AssetType::Unknown;
        std::vector<std::string> dependencies;
    };

    static AssetType DetectFileType(const std::string& path)
    {
        auto dotPos = path.rfind('.');
        if (dotPos == std::string::npos)
        {
            return AssetType::Unknown;
        }

        std::string ext = path.substr(dotPos);
        // Convert to lowercase
        for (auto& ch : ext)
        {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }

        if (ext == ".png" || ext == ".jpg" || ext == ".tga" || ext == ".bmp" || ext == ".dds")
        {
            return AssetType::Texture;
        }
        if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb")
        {
            return AssetType::Model;
        }
        if (ext == ".wav" || ext == ".ogg" || ext == ".mp3")
        {
            return AssetType::Audio;
        }
        if (ext == ".mat" || ext == ".material")
        {
            return AssetType::Material;
        }
        if (ext == ".as" || ext == ".lua")
        {
            return AssetType::Script;
        }

        return AssetType::Unknown;
    }

    class AssetDatabase
    {
      public:
        void IndexFile(const std::string& path, const std::string& name)
        {
            AssetMetadata meta;
            meta.path = path;
            meta.name = name;
            meta.type = DetectFileType(path);
            m_assets[path] = meta;
        }

        const AssetMetadata* LookupByPath(const std::string& path) const
        {
            auto it = m_assets.find(path);
            if (it == m_assets.end())
            {
                return nullptr;
            }
            return &it->second;
        }

        std::vector<const AssetMetadata*> Search(const std::string& substring) const
        {
            std::vector<const AssetMetadata*> results;
            for (const auto& [path, meta] : m_assets)
            {
                if (meta.name.find(substring) != std::string::npos)
                {
                    results.push_back(&meta);
                }
            }
            return results;
        }

        void AddDependency(const std::string& assetPath, const std::string& dependencyPath)
        {
            auto it = m_assets.find(assetPath);
            if (it != m_assets.end())
            {
                it->second.dependencies.push_back(dependencyPath);
            }
        }

        std::vector<std::string> GetDependencies(const std::string& assetPath) const
        {
            auto it = m_assets.find(assetPath);
            if (it == m_assets.end())
            {
                return {};
            }
            return it->second.dependencies;
        }

      private:
        std::unordered_map<std::string, AssetMetadata> m_assets;
    };

} // namespace TestAssetDB

TEST(AssetDB_MetadataIndexing)
{
    TestAssetDB::AssetDatabase db;

    db.IndexFile("textures/brick.png", "brick");
    db.IndexFile("models/hero.fbx", "hero");
    db.IndexFile("audio/explosion.wav", "explosion");

    const auto* brick = db.LookupByPath("textures/brick.png");
    EXPECT_TRUE(brick != nullptr);
    EXPECT_EQ(brick->name, std::string("brick"));
    EXPECT_EQ(brick->path, std::string("textures/brick.png"));

    const auto* hero = db.LookupByPath("models/hero.fbx");
    EXPECT_TRUE(hero != nullptr);
    EXPECT_EQ(hero->name, std::string("hero"));

    const auto* missing = db.LookupByPath("nonexistent/file.dat");
    EXPECT_TRUE(missing == nullptr);
}

TEST(AssetDB_FileTypeDetection)
{
    using TestAssetDB::AssetType;
    using TestAssetDB::DetectFileType;

    EXPECT_EQ(static_cast<int>(DetectFileType("textures/wall.png")), static_cast<int>(AssetType::Texture));
    EXPECT_EQ(static_cast<int>(DetectFileType("textures/floor.jpg")), static_cast<int>(AssetType::Texture));
    EXPECT_EQ(static_cast<int>(DetectFileType("textures/sky.dds")), static_cast<int>(AssetType::Texture));
    EXPECT_EQ(static_cast<int>(DetectFileType("models/character.fbx")), static_cast<int>(AssetType::Model));
    EXPECT_EQ(static_cast<int>(DetectFileType("models/prop.obj")), static_cast<int>(AssetType::Model));
    EXPECT_EQ(static_cast<int>(DetectFileType("models/scene.gltf")), static_cast<int>(AssetType::Model));
    EXPECT_EQ(static_cast<int>(DetectFileType("audio/bgm.wav")), static_cast<int>(AssetType::Audio));
    EXPECT_EQ(static_cast<int>(DetectFileType("audio/sfx.ogg")), static_cast<int>(AssetType::Audio));
    EXPECT_EQ(static_cast<int>(DetectFileType("audio/music.mp3")), static_cast<int>(AssetType::Audio));
    EXPECT_EQ(static_cast<int>(DetectFileType("data/unknown.xyz")), static_cast<int>(AssetType::Unknown));
}

TEST(AssetDB_SearchFilter)
{
    TestAssetDB::AssetDatabase db;

    db.IndexFile("textures/brick_wall.png", "brick_wall");
    db.IndexFile("textures/brick_floor.png", "brick_floor");
    db.IndexFile("textures/stone_wall.png", "stone_wall");
    db.IndexFile("models/hero.fbx", "hero");
    db.IndexFile("models/brick_pillar.fbx", "brick_pillar");

    auto brickResults = db.Search("brick");
    EXPECT_EQ(static_cast<int>(brickResults.size()), 3);

    auto wallResults = db.Search("wall");
    EXPECT_EQ(static_cast<int>(wallResults.size()), 2);

    auto heroResults = db.Search("hero");
    EXPECT_EQ(static_cast<int>(heroResults.size()), 1);
    EXPECT_EQ(heroResults[0]->name, std::string("hero"));

    auto noResults = db.Search("dragon");
    EXPECT_EQ(static_cast<int>(noResults.size()), 0);
}

TEST(AssetDB_DependencyTracking)
{
    TestAssetDB::AssetDatabase db;

    db.IndexFile("materials/brick.mat", "brick_material");
    db.IndexFile("textures/brick_diffuse.png", "brick_diffuse");
    db.IndexFile("textures/brick_normal.png", "brick_normal");

    db.AddDependency("materials/brick.mat", "textures/brick_diffuse.png");
    db.AddDependency("materials/brick.mat", "textures/brick_normal.png");

    auto deps = db.GetDependencies("materials/brick.mat");
    EXPECT_EQ(static_cast<int>(deps.size()), 2);
    EXPECT_EQ(deps[0], std::string("textures/brick_diffuse.png"));
    EXPECT_EQ(deps[1], std::string("textures/brick_normal.png"));

    auto noDeps = db.GetDependencies("textures/brick_diffuse.png");
    EXPECT_EQ(static_cast<int>(noDeps.size()), 0);

    auto missingDeps = db.GetDependencies("nonexistent/asset.dat");
    EXPECT_EQ(static_cast<int>(missingDeps.size()), 0);
}

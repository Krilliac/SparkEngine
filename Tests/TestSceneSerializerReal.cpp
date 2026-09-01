/**
 * @file TestSceneSerializerReal.cpp
 * @brief Production-linked regression tests for editor scene persistence.
 */

#include "TestFramework.h"
#include "SceneSystem/SceneComponentCodec.h"
#include "SceneSystem/SceneSerializer.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <locale>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace
{
    class TemporarySceneFile
    {
      public:
        explicit TemporarySceneFile(const std::string& extension)
        {
            const auto nonce = std::chrono::high_resolution_clock::now().time_since_epoch().count();
#ifdef _WIN32
            const auto processId = static_cast<uint64_t>(::_getpid());
#else
            const auto processId = static_cast<uint64_t>(::getpid());
#endif
            m_path = std::filesystem::temp_directory_path() /
                     ("spark_scene_serializer_" + std::to_string(processId) + "_" + std::to_string(nonce) + extension);
        }

        ~TemporarySceneFile()
        {
            std::error_code ignored;
            std::filesystem::remove(m_path, ignored);
            std::filesystem::remove(m_path.string() + ".bak", ignored);
        }

        const std::filesystem::path& Path() const { return m_path; }

      private:
        std::filesystem::path m_path;
    };

    class CommaDecimalPoint final : public std::numpunct<char>
    {
      protected:
        char do_decimal_point() const override { return ','; }
    };

    class GlobalLocaleGuard
    {
      public:
        explicit GlobalLocaleGuard(const std::locale& replacement) : m_previous(std::locale())
        {
            std::locale::global(replacement);
        }
        ~GlobalLocaleGuard() { std::locale::global(m_previous); }

      private:
        std::locale m_previous;
    };
} // namespace

TEST(SceneSerializerReal_SparkSceneUsesCompleteJSONFormat)
{
    EXPECT_EQ(static_cast<int>(SparkEditor::SceneSerializer::DetectFormat("Level.sparkscene")),
              static_cast<int>(SparkEditor::SerializationFormat::JSON));
    EXPECT_EQ(static_cast<int>(SparkEditor::SceneSerializer::DetectFormat("Level.SPARKSCENE")),
              static_cast<int>(SparkEditor::SerializationFormat::JSON));
    EXPECT_EQ(SparkEditor::SceneSerializer::GetSupportedExtensions(SparkEditor::SerializationFormat::BINARY).size(),
              size_t{0});
}

TEST(SceneSerializerReal_AutoSaveRejectsUnsupportedExtensions)
{
    using namespace SparkEditor;
    TemporarySceneFile file(".spks");
    SceneSerializer serializer;
    SceneFile scene;

    const auto result = serializer.SaveScene(scene, file.Path().string());
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(std::filesystem::exists(file.Path()));
}

TEST(SceneSerializerReal_ObjectIDAllocationDoesNotOverflow)
{
    using namespace SparkEditor;
    SceneFile scene;
    SceneObject first;
    first.id = 1;
    scene.objects.push_back(first);
    SceneObject nearMaximum;
    nearMaximum.id = std::numeric_limits<ObjectID>::max() - 1;
    scene.objects.push_back(nearMaximum);
    EXPECT_EQ(scene.GetNextObjectID(), ObjectID{2});

    scene.objects.front().id = std::numeric_limits<ObjectID>::max();
    EXPECT_EQ(scene.GetNextObjectID(), ObjectID{1});
}

TEST(SceneSerializerReal_CompleteRoundTripPreservesLargeIDs)
{
    using namespace SparkEditor;
    TemporarySceneFile file(".sparkscene");
    SceneSerializer serializer;
    SceneFile scene;
    std::strncpy(scene.header.sceneName, "CompleteScene", sizeof(scene.header.sceneName) - 1);
    scene.header.timestamp = UINT64_C(9007199254740993);

    SceneObject object;
    object.id = UINT64_C(9007199254740993);
    object.name = "LargeIDObject\b\f\x01 \xf0\x9f\x9a\x80";
    object.tag = "Gameplay";
    object.layer = 7;
    object.active = false;
    object.staticObject = true;
    object.transform.parentID = INVALID_OBJECT_ID;
    object.transform.position.x = 123456.7890625f;
    object.componentTypes = {ComponentType::SCRIPT, ComponentType::SPRITE_RENDERER, ComponentType::FOLIAGE_VOLUME};
    scene.objects.push_back(object);

    for (ComponentType type : object.componentTypes)
    {
        Component component;
        component.type = type;
        component.objectID = object.id;
        component.enabled = type != ComponentType::FOLIAGE_VOLUME;
        if (type == ComponentType::SCRIPT)
            component.SetData(ScriptData{});
        else if (type == ComponentType::SPRITE_RENDERER)
            component.SetData(SpriteRendererData{});
        else if (type == ComponentType::FOLIAGE_VOLUME)
        {
            FoliageVolumeData foliage;
            foliage.seed = 127;
            component.SetData(foliage);
        }
        scene.components.push_back(component);
    }

    AssetReference asset;
    asset.assetPath = "Assets/example.mesh";
    asset.assetType = "Mesh";
    asset.lastModified = UINT64_C(9007199254740999);
    asset.fileSize = UINT64_C(9007199254741001);
    asset.dependencies.push_back("Assets/example.material");
    scene.assetReferences.push_back(asset);
    scene.environment.fogEnabled = true;
    scene.environment.fogDensity = 0.25f;
    scene.defaultCamera.fieldOfView = 63.0f;
    scene.UpdateHeader();
    scene.header.timestamp = UINT64_C(9007199254740993);

    const auto save = serializer.SaveScene(scene, file.Path().string());
    EXPECT_TRUE(save.success);
    EXPECT_TRUE(serializer.ValidateSceneFile(file.Path().string()).success);

    SceneFile loaded;
    const auto load = serializer.LoadScene(file.Path().string(), loaded);
    EXPECT_TRUE(load.success);
    EXPECT_EQ(loaded.header.timestamp, UINT64_C(9007199254740993));
    EXPECT_EQ(loaded.objects.size(), size_t{1});
    EXPECT_EQ(loaded.objects[0].id, UINT64_C(9007199254740993));
    EXPECT_EQ(loaded.objects[0].name, std::string("LargeIDObject\b\f\x01 \xf0\x9f\x9a\x80"));
    EXPECT_EQ(loaded.objects[0].transform.parentID, INVALID_OBJECT_ID);
    EXPECT_EQ(loaded.objects[0].transform.position.x, object.transform.position.x);
    EXPECT_EQ(loaded.objects[0].tag, std::string("Gameplay"));
    EXPECT_EQ(static_cast<uint32_t>(loaded.objects[0].componentTypes[1]),
              static_cast<uint32_t>(ComponentType::SPRITE_RENDERER));
    EXPECT_EQ(static_cast<uint32_t>(loaded.objects[0].componentTypes[2]),
              static_cast<uint32_t>(ComponentType::FOLIAGE_VOLUME));
    EXPECT_TRUE(loaded.objects[0].staticObject);
    EXPECT_FALSE(loaded.objects[0].active);
    EXPECT_EQ(loaded.components.size(), size_t{3});
    const auto foliage = std::find_if(loaded.components.begin(), loaded.components.end(), [](const Component& value)
                                      { return value.type == ComponentType::FOLIAGE_VOLUME; });
    EXPECT_TRUE(foliage != loaded.components.end());
    EXPECT_TRUE(foliage->GetData<FoliageVolumeData>() != nullptr);
    EXPECT_EQ(foliage->GetData<FoliageVolumeData>()->seed, 127);
    EXPECT_FALSE(foliage->enabled);
    EXPECT_EQ(loaded.assetReferences[0].lastModified, UINT64_C(9007199254740999));
    EXPECT_EQ(loaded.assetReferences[0].fileSize, UINT64_C(9007199254741001));
    EXPECT_TRUE(loaded.environment.fogEnabled);
    EXPECT_NEAR(loaded.environment.fogDensity, 0.25f, 0.001f);
    EXPECT_NEAR(loaded.defaultCamera.fieldOfView, 63.0f, 0.001f);
}

TEST(SceneSerializerReal_ParsesUnicodeEscapesAndRejectsInvalidStrings)
{
    using namespace SparkEditor;
    TemporarySceneFile file(".sparkscene");
    {
        std::ofstream output(file.Path(), std::ios::binary | std::ios::trunc);
        output << R"({"sceneName":"Spark \uD83D\uDE80","version":2})";
    }

    SceneSerializer serializer;
    SceneFile loaded;
    auto result = serializer.LoadScene(file.Path().string(), loaded);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(std::string(loaded.header.sceneName), std::string("Spark \xf0\x9f\x9a\x80"));

    {
        std::ofstream output(file.Path(), std::ios::binary | std::ios::trunc);
        output << R"({"sceneName":"invalid\q"})";
    }
    loaded.header.timestamp = 1234;
    result = serializer.LoadScene(file.Path().string(), loaded);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(loaded.header.timestamp, UINT64_C(1234));

    {
        std::ofstream output(file.Path(), std::ios::binary | std::ios::trunc);
        output << "{\"sceneName\":\"";
        output.put(static_cast<char>(0xc0));
        output.put(static_cast<char>(0xaf));
        output << "\"}";
    }
    result = serializer.LoadScene(file.Path().string(), loaded);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(loaded.header.timestamp, UINT64_C(1234));
}

TEST(SceneSerializerReal_FailedLoadIsTransactional)
{
    using namespace SparkEditor;
    TemporarySceneFile file(".sparkscene");
    {
        std::ofstream output(file.Path());
        output << "{\"sceneName\":\"partial\",\"objects\":[{\"id\":1}] trailing";
    }

    SceneFile liveScene;
    SceneObject existing;
    existing.id = 42;
    existing.name = "KeepMe";
    liveScene.objects.push_back(existing);

    SceneSerializer serializer;
    const auto result = serializer.LoadScene(file.Path().string(), liveScene);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(liveScene.objects.size(), size_t{1});
    EXPECT_EQ(liveScene.objects[0].id, ObjectID{42});
    EXPECT_EQ(liveScene.objects[0].name, std::string("KeepMe"));
}

TEST(SceneSerializerReal_NumbersIgnoreProcessLocale)
{
    using namespace SparkEditor;
    GlobalLocaleGuard localeGuard(std::locale(std::locale::classic(), new CommaDecimalPoint));
    TemporarySceneFile inputFile(".sparkscene");
    {
        std::ofstream output(inputFile.Path(), std::ios::binary | std::ios::trunc);
        output << R"({"version":2,"ambientIntensity":1.5})";
    }

    SceneSerializer serializer;
    SceneFile loaded;
    auto result = serializer.LoadScene(inputFile.Path().string(), loaded);
    EXPECT_TRUE(result.success);
    EXPECT_NEAR(loaded.header.ambientIntensity, 1.5f, 0.0001f);

    TemporarySceneFile outputFile(".sparkscene");
    result = serializer.SaveScene(loaded, outputFile.Path().string(), SerializationFormat::JSON);
    EXPECT_TRUE(result.success);
    std::ifstream input(outputFile.Path(), std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    EXPECT_TRUE(content.find("1.5") != std::string::npos);
}

TEST(SceneSerializerReal_RejectsSemanticCorruptionAndOutOfRangeIntegers)
{
    using namespace SparkEditor;
    TemporarySceneFile file(".sparkscene");
    SceneSerializer serializer;
    SceneFile liveScene;
    liveScene.header.timestamp = 5678;

    {
        std::ofstream output(file.Path(), std::ios::binary | std::ios::trunc);
        output << R"({"objectCount":1,"objects":[null]})";
    }
    auto result = serializer.LoadScene(file.Path().string(), liveScene);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(liveScene.header.timestamp, UINT64_C(5678));

    {
        std::ofstream output(file.Path(), std::ios::binary | std::ios::trunc);
        output << R"({"objectCount":1,"objects":[{"id":"oops","active":"false","transform":"oops"}]})";
    }
    result = serializer.LoadScene(file.Path().string(), liveScene);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(liveScene.header.timestamp, UINT64_C(5678));

    {
        std::ofstream output(file.Path(), std::ios::binary | std::ios::trunc);
        output << R"({"version":3})";
    }
    result = serializer.LoadScene(file.Path().string(), liveScene);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(liveScene.header.timestamp, UINT64_C(5678));

    {
        std::ofstream output(file.Path(), std::ios::binary | std::ios::trunc);
        output << "{\"sceneName\":\"" << std::string(64, 'N') << "\"}";
    }
    result = serializer.LoadScene(file.Path().string(), liveScene);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(liveScene.header.timestamp, UINT64_C(5678));

    {
        std::ofstream output(file.Path(), std::ios::binary | std::ios::trunc);
        output << R"({"sceneName":"bad\u0000name"})";
    }
    result = serializer.LoadScene(file.Path().string(), liveScene);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(liveScene.header.timestamp, UINT64_C(5678));

    {
        std::ofstream output(file.Path(), std::ios::binary | std::ios::trunc);
        output << R"({"environment":{"skyType":2147483647},"defaultCamera":{"projectionType":-1}})";
    }
    result = serializer.LoadScene(file.Path().string(), liveScene);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(liveScene.header.timestamp, UINT64_C(5678));

    {
        std::ofstream output(file.Path(), std::ios::binary | std::ios::trunc);
        output << R"({"objectCount":1,"objects":[{"id":1,"componentTypes":["UnknownType"]}]})";
    }
    result = serializer.LoadScene(file.Path().string(), liveScene);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(liveScene.header.timestamp, UINT64_C(5678));

    {
        std::ofstream output(file.Path(), std::ios::binary | std::ios::trunc);
        output << R"({"objectCount":1,"objects":[{"id":1,"layer":1e308}]})";
    }
    result = serializer.LoadScene(file.Path().string(), liveScene);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(liveScene.header.timestamp, UINT64_C(5678));

    {
        std::ofstream output(file.Path(), std::ios::binary | std::ios::trunc);
        output << R"({"objectCount":1,"objects":[{"id":1,"layer":1.0000000000000001}]})";
    }
    result = serializer.LoadScene(file.Path().string(), liveScene);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(liveScene.header.timestamp, UINT64_C(5678));

    {
        std::ofstream output(file.Path(), std::ios::binary | std::ios::trunc);
        output << R"({"objectCount":1,"objects":[{"id":18446744073709551615}]})";
    }
    result = serializer.LoadScene(file.Path().string(), liveScene);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(liveScene.header.timestamp, UINT64_C(5678));
}

TEST(SceneSerializerReal_RejectsInvalidHierarchyTransactionally)
{
    using namespace SparkEditor;
    TemporarySceneFile file(".sparkscene");
    SceneSerializer serializer;
    SceneFile liveScene;
    liveScene.header.timestamp = 6789;

    const auto rejects = [&](const std::string& json)
    {
        {
            std::ofstream output(file.Path(), std::ios::binary | std::ios::trunc);
            output << json;
        }
        const auto result = serializer.LoadScene(file.Path().string(), liveScene);
        EXPECT_FALSE(result.success);
        EXPECT_EQ(liveScene.header.timestamp, UINT64_C(6789));
    };

    rejects(R"({"objectCount":1,"objects":[{"id":1,"transform":{"parentID":1,"childIDs":[1]}}]})");
    rejects(
        R"({"objectCount":2,"objects":[{"id":1,"transform":{"parentID":2,"childIDs":[2]}},{"id":2,"transform":{"parentID":1,"childIDs":[1]}}]})");
    rejects(
        R"({"objectCount":2,"objects":[{"id":1,"transform":{"childIDs":[2,2]}},{"id":2,"transform":{"parentID":1}}]})");
    rejects(R"({"objectCount":2,"objects":[{"id":1,"transform":{"childIDs":[2]}},{"id":2}]})");
    rejects(R"({"objectCount":1,"objects":[{"id":1,"componentTypes":["Camera"]}]})");
    rejects(R"({"objectCount":1,"objects":[{"id":1}],"components":[{"type":"Camera","objectID":1}]})");
    rejects(
        R"({"objectCount":1,"objects":[{"id":1,"componentTypes":["Camera","Camera"]}],"components":[{"type":"Camera","objectID":1},{"type":"Camera","objectID":1}]})");
}

TEST(SceneSerializerReal_NonFiniteSavePreservesExistingFile)
{
    using namespace SparkEditor;
    TemporarySceneFile file(".sparkscene");
    {
        std::ofstream output(file.Path(), std::ios::binary | std::ios::trunc);
        output << "sentinel";
    }

    SceneFile scene;
    scene.header.ambientIntensity = std::numeric_limits<float>::infinity();
    SceneSerializer serializer;
    const auto result = serializer.SaveScene(scene, file.Path().string(), SerializationFormat::JSON);
    EXPECT_FALSE(result.success);

    std::ifstream input(file.Path(), std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, std::string("sentinel"));
}

TEST(SceneSerializerReal_InvalidUTF8SavePreservesExistingFile)
{
    using namespace SparkEditor;
    TemporarySceneFile file(".sparkscene");
    {
        std::ofstream output(file.Path(), std::ios::binary | std::ios::trunc);
        output << "sentinel";
    }

    SceneFile scene;
    SceneObject object;
    object.id = 1;
    object.name.assign(1, static_cast<char>(0x80));
    scene.objects.push_back(object);
    SceneSerializer serializer;
    const auto result = serializer.SaveScene(scene, file.Path().string(), SerializationFormat::JSON);
    EXPECT_FALSE(result.success);

    std::ifstream input(file.Path(), std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, std::string("sentinel"));
}

TEST(SceneSerializerReal_InvalidSavePreservesExistingFile)
{
    using namespace SparkEditor;
    TemporarySceneFile file(".sparkscene");
    {
        std::ofstream output(file.Path(), std::ios::binary | std::ios::trunc);
        output << "sentinel";
    }

    SceneSerializer serializer;
    SceneFile invalidScene;
    invalidScene.objects.emplace_back(); // Reserved object ID 0.
    auto result = serializer.SaveScene(invalidScene, file.Path().string(), SerializationFormat::JSON);
    EXPECT_FALSE(result.success);

    std::ifstream firstInput(file.Path(), std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(firstInput)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, std::string("sentinel"));

    SceneFile unterminatedHeader;
    std::fill(std::begin(unterminatedHeader.header.sceneName), std::end(unterminatedHeader.header.sceneName), 'X');
    result = serializer.SaveScene(unterminatedHeader, file.Path().string(), SerializationFormat::JSON);
    EXPECT_FALSE(result.success);

    std::ifstream secondInput(file.Path(), std::ios::binary);
    content.assign(std::istreambuf_iterator<char>(secondInput), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, std::string("sentinel"));

    SceneFile defaultComponent;
    defaultComponent.components.emplace_back();
    result = serializer.SaveScene(defaultComponent, file.Path().string(), SerializationFormat::JSON);
    EXPECT_FALSE(result.success);

    SceneFile unsupportedVersion;
    unsupportedVersion.header.version = SCENE_FILE_VERSION + 1;
    result = serializer.SaveScene(unsupportedVersion, file.Path().string(), SerializationFormat::JSON);
    EXPECT_FALSE(result.success);
}

TEST(SceneSerializerReal_RejectsExcessiveJSONNesting)
{
    using namespace SparkEditor;
    TemporarySceneFile file(".sparkscene");
    {
        std::ofstream output(file.Path(), std::ios::binary | std::ios::trunc);
        for (int i = 0; i < 129; ++i)
            output << '[';
        output << "null";
        for (int i = 0; i < 129; ++i)
            output << ']';
    }

    SceneSerializer serializer;
    SceneFile liveScene;
    liveScene.header.timestamp = 9012;
    const auto result = serializer.LoadScene(file.Path().string(), liveScene);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(liveScene.header.timestamp, UINT64_C(9012));
}

TEST(SceneSerializerReal_ComponentPayloadsOwnLongStringsAndRoundTripAsNamedFields)
{
    using namespace SparkEditor;
    const std::string longPath = "Assets/" + std::string(256, 'x') + "/payload.asset";

    MeshRenderer mesh;
    mesh.meshAssetPath = longPath + ".mesh";
    mesh.materialAssetPath = longPath + ".material";
    Collider collider;
    collider.meshAssetPath = longPath + ".collider";
    collider.physicsMaterial = longPath + ".physics";
    AudioSource audio;
    audio.audioClipPath = longPath + ".wav";
    SpriteRendererData sprite;
    sprite.texturePath = longPath + ".png";
    TilemapData tilemap;
    tilemap.tilesetTexturePath = longPath + ".tiles";
    ParallaxLayerData parallax;
    parallax.texturePath = longPath + ".parallax";

    SceneFile scene;
    SceneObject object;
    object.id = 1;
    object.componentTypes = {ComponentType::MESH_RENDERER,   ComponentType::COLLIDER, ComponentType::AUDIO_SOURCE,
                             ComponentType::SPRITE_RENDERER, ComponentType::TILEMAP,  ComponentType::PARALLAX_BG};
    scene.objects.push_back(object);

    const auto add = [&](ComponentType type, const auto& value)
    {
        Component component;
        component.type = type;
        component.objectID = 1;
        component.SetData(value);
        scene.components.push_back(std::move(component));
    };
    add(ComponentType::MESH_RENDERER, mesh);
    add(ComponentType::COLLIDER, collider);
    add(ComponentType::AUDIO_SOURCE, audio);
    add(ComponentType::SPRITE_RENDERER, sprite);
    add(ComponentType::TILEMAP, tilemap);
    add(ComponentType::PARALLAX_BG, parallax);
    scene.UpdateHeader();

    // std::any copies and moves the owned values rather than aliasing a byte image.
    Component copied = scene.components.front();
    copied.GetData<MeshRenderer>()->meshAssetPath = "copy-only";
    EXPECT_EQ(scene.components.front().GetData<MeshRenderer>()->meshAssetPath, mesh.meshAssetPath);
    Component moved = std::move(copied);
    EXPECT_EQ(moved.GetData<MeshRenderer>()->meshAssetPath, std::string("copy-only"));
    EXPECT_TRUE(moved.GetData<AudioSource>() == nullptr);
    const Component& constMoved = moved;
    EXPECT_TRUE(constMoved.GetData<MeshRenderer>() != nullptr);

    TemporarySceneFile file(".sparkscene");
    SceneSerializer serializer;
    const auto save = serializer.SaveScene(scene, file.Path().string(), SerializationFormat::JSON);
    EXPECT_TRUE(save.success);

    std::ifstream jsonInput(file.Path(), std::ios::binary);
    const std::string json((std::istreambuf_iterator<char>(jsonInput)), std::istreambuf_iterator<char>());
    EXPECT_TRUE(json.find("\"schema\": 1") != std::string::npos);
    EXPECT_TRUE(json.find("\"fields\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"meshAssetPath\"") != std::string::npos);
    EXPECT_TRUE(json.find("\"data\":\"") == std::string::npos);

    SceneFile loaded;
    const auto load = serializer.LoadScene(file.Path().string(), loaded);
    EXPECT_TRUE(load.success);
    EXPECT_EQ(loaded.components.size(), size_t{6});
    EXPECT_EQ(loaded.components[0].GetData<MeshRenderer>()->meshAssetPath, mesh.meshAssetPath);
    EXPECT_EQ(loaded.components[1].GetData<Collider>()->physicsMaterial, collider.physicsMaterial);
    EXPECT_EQ(loaded.components[2].GetData<AudioSource>()->audioClipPath, audio.audioClipPath);
    EXPECT_EQ(loaded.components[3].GetData<SpriteRendererData>()->texturePath, sprite.texturePath);
    EXPECT_EQ(loaded.components[4].GetData<TilemapData>()->tilesetTexturePath, tilemap.tilesetTexturePath);
    EXPECT_EQ(loaded.components[5].GetData<ParallaxLayerData>()->texturePath, parallax.texturePath);
}

TEST(SceneSerializerReal_AllBuiltInPayloadCodecsDefaultAndRoundTrip)
{
    using namespace SparkEditor;
    const std::vector<ComponentType> types = GetSceneComponentPayloadTypes();
    EXPECT_EQ(types.size(), size_t{63});

    SceneFile scene;
    SceneObject object;
    object.id = 1;
    object.componentTypes = types;
    scene.objects.push_back(object);
    for (ComponentType type : types)
    {
        Component component;
        component.type = type;
        component.objectID = 1;
        std::string error;
        EXPECT_TRUE(InitializeDefaultSceneComponentPayload(component, error));
        EXPECT_TRUE(component.HasData());
        scene.components.push_back(std::move(component));
    }
    scene.UpdateHeader();

    TemporarySceneFile file(".sparkscene");
    SceneSerializer serializer;
    ASSERT_TRUE(serializer.SaveScene(scene, file.Path().string(), SerializationFormat::JSON).success);
    SceneFile loaded;
    ASSERT_TRUE(serializer.LoadScene(file.Path().string(), loaded).success);
    ASSERT_EQ(loaded.components.size(), types.size());
    for (size_t index = 0; index < types.size(); ++index)
    {
        EXPECT_EQ(static_cast<uint32_t>(loaded.components[index].type), static_cast<uint32_t>(types[index]));
        EXPECT_TRUE(loaded.components[index].HasData());
        EXPECT_TRUE(loaded.components[index].DataType() == scene.components[index].DataType());
    }
}

TEST(SceneSerializerReal_WrongCustomAndLegacyRawPayloadsFailClosed)
{
    using namespace SparkEditor;
    SceneSerializer serializer;
    TemporarySceneFile outputFile(".sparkscene");

    SceneFile wrongType;
    SceneObject object;
    object.id = 1;
    object.componentTypes = {ComponentType::MESH_RENDERER};
    wrongType.objects.push_back(object);
    Component component;
    component.type = ComponentType::MESH_RENDERER;
    component.objectID = 1;
    component.SetData(AudioSource{});
    wrongType.components.push_back(component);
    wrongType.UpdateHeader();
    EXPECT_FALSE(serializer.SaveScene(wrongType, outputFile.Path().string(), SerializationFormat::JSON).success);

    SceneFile invalidEnum;
    invalidEnum.objects.push_back(object);
    invalidEnum.objects[0].componentTypes = {ComponentType::LIGHT};
    component.type = ComponentType::LIGHT;
    Light light;
    light.type = static_cast<Light::Type>(999);
    component.SetData(light);
    invalidEnum.components.push_back(component);
    invalidEnum.UpdateHeader();
    EXPECT_FALSE(serializer.SaveScene(invalidEnum, outputFile.Path().string(), SerializationFormat::JSON).success);

    const auto rejectsSemanticPayload = [&](ComponentType type, const auto& payload)
    {
        SceneFile invalid;
        SceneObject owner;
        owner.id = 1;
        owner.componentTypes = {type};
        invalid.objects.push_back(owner);
        Component invalidComponent;
        invalidComponent.type = type;
        invalidComponent.objectID = 1;
        invalidComponent.SetData(payload);
        invalid.components.push_back(std::move(invalidComponent));
        invalid.UpdateHeader();
        EXPECT_FALSE(serializer.SaveScene(invalid, outputFile.Path().string(), SerializationFormat::JSON).success);
    };
    RigidBody2DData invalidBody2D;
    invalidBody2D.bodyType = 999;
    rejectsSemanticPayload(ComponentType::RIGID_BODY_2D, invalidBody2D);
    LODGroupData invalidLod;
    invalidLod.lodCount = 999;
    rejectsSemanticPayload(ComponentType::LOD_GROUP, invalidLod);
    VehicleData invalidVehicle;
    invalidVehicle.wheelCount = 0;
    rejectsSemanticPayload(ComponentType::VEHICLE, invalidVehicle);
    Text3DData invalidText;
    invalidText.alignment = 999;
    rejectsSemanticPayload(ComponentType::TEXT_3D, invalidText);
    HealthData invalidHealth;
    invalidHealth.health = -1.0f;
    rejectsSemanticPayload(ComponentType::HEALTH, invalidHealth);
    ParticleEmitterData invalidParticle;
    invalidParticle.startSize = 0.0f;
    rejectsSemanticPayload(ComponentType::PARTICLE_SYSTEM, invalidParticle);
    AIAgentData invalidAI;
    invalidAI.detectionRange = 10.0f;
    invalidAI.attackRange = 20.0f;
    rejectsSemanticPayload(ComponentType::AI_AGENT, invalidAI);
    ProjectileData invalidProjectile;
    invalidProjectile.maxRange = 0.0f;
    rejectsSemanticPayload(ComponentType::PROJECTILE, invalidProjectile);
    InteractionData invalidInteraction;
    invalidInteraction.interactionRadius = 0.0f;
    rejectsSemanticPayload(ComponentType::INTERACTION, invalidInteraction);
    AudioSource invalidAudio;
    invalidAudio.pitch = 5.0f;
    rejectsSemanticPayload(ComponentType::AUDIO_SOURCE, invalidAudio);
    AudioReverbZoneData invalidReverb;
    invalidReverb.innerRadius = 0.0f;
    rejectsSemanticPayload(ComponentType::AUDIO_REVERB_ZONE, invalidReverb);
    DialogueTriggerData invalidDialogue;
    invalidDialogue.interactionRadius = 0.0f;
    rejectsSemanticPayload(ComponentType::DIALOGUE_TRIGGER, invalidDialogue);
    AreaBoundaryData invalidBoundary;
    invalidBoundary.loadRadius = 0.0f;
    rejectsSemanticPayload(ComponentType::AREA_BOUNDARY, invalidBoundary);
    SpringArmData invalidSpringArm;
    invalidSpringArm.minLength = 0.0f;
    rejectsSemanticPayload(ComponentType::SPRING_ARM, invalidSpringArm);
    Camera2DData invalidCamera2D;
    invalidCamera2D.followSmoothing = 2.0f;
    rejectsSemanticPayload(ComponentType::CAMERA_2D, invalidCamera2D);

    const auto acceptsSemanticPayload = [&](ComponentType type, const auto& payload)
    {
        SceneFile valid;
        SceneObject owner;
        owner.id = 1;
        owner.componentTypes = {type};
        valid.objects.push_back(owner);
        Component validComponent;
        validComponent.type = type;
        validComponent.objectID = 1;
        validComponent.SetData(payload);
        valid.components.push_back(std::move(validComponent));
        valid.UpdateHeader();
        EXPECT_TRUE(serializer.SaveScene(valid, outputFile.Path().string(), SerializationFormat::JSON).success);
        SceneFile reloaded;
        EXPECT_TRUE(serializer.LoadScene(outputFile.Path().string(), reloaded).success);
        EXPECT_EQ(reloaded.components.size(), size_t{1});
    };
    AnimationControllerData reverseAnimation;
    reverseAnimation.playbackSpeed = -1.0f;
    acceptsSemanticPayload(ComponentType::ANIMATION, reverseAnimation);
    RigidBody staticBody;
    staticBody.bodyType = RigidBody::STATIC;
    staticBody.mass = 0.0f;
    acceptsSemanticPayload(ComponentType::RIGID_BODY, staticBody);
    RigidBody2DData staticBody2D;
    staticBody2D.bodyType = 0;
    staticBody2D.mass = 0.0f;
    staticBody2D.friction = 1.0f;
    acceptsSemanticPayload(ComponentType::RIGID_BODY_2D, staticBody2D);
    AudioSource loudAudio;
    loudAudio.volume = 2.0f;
    loudAudio.minDistance = 1.0f;
    loudAudio.maxDistance = 2.0f;
    acceptsSemanticPayload(ComponentType::AUDIO_SOURCE, loudAudio);

    SceneFile custom;
    custom.objects.push_back(object);
    custom.objects[0].componentTypes = {ComponentType::CUSTOM};
    component.type = ComponentType::CUSTOM;
    component.SetData(HealthData{});
    custom.components.push_back(component);
    custom.UpdateHeader();
    EXPECT_FALSE(serializer.SaveScene(custom, outputFile.Path().string(), SerializationFormat::JSON).success);

    TemporarySceneFile inputFile(".sparkscene");
    {
        std::ofstream output(inputFile.Path(), std::ios::binary | std::ios::trunc);
        output
            << R"({"version":1,"objectCount":1,"componentCount":1,"objects":[{"id":1,"componentTypes":["MeshRenderer"]}],"components":[{"type":"MeshRenderer","objectID":1,"data":"deadbeef"}]})";
    }
    SceneFile live;
    live.header.timestamp = 42;
    EXPECT_FALSE(serializer.LoadScene(inputFile.Path().string(), live).success);
    EXPECT_EQ(live.header.timestamp, UINT64_C(42));

    {
        std::ofstream output(inputFile.Path(), std::ios::binary | std::ios::trunc);
        output << R"({"version":1,"objectCount":0,"componentCount":0,"objects":[],"components":[]})";
    }
    EXPECT_FALSE(serializer.LoadScene(inputFile.Path().string(), live).success);
    EXPECT_EQ(live.header.timestamp, UINT64_C(42));

    {
        std::ofstream output(inputFile.Path(), std::ios::binary | std::ios::trunc);
        output << R"({"objectCount":0,"componentCount":0,"objects":[],"components":[]})";
    }
    EXPECT_FALSE(serializer.LoadScene(inputFile.Path().string(), live).success);
    EXPECT_EQ(live.header.timestamp, UINT64_C(42));

    {
        std::ofstream output(inputFile.Path(), std::ios::binary | std::ios::trunc);
        output << R"({"version":2,"version":1,"objectCount":0,"componentCount":0,"objects":[],"components":[]})";
    }
    EXPECT_FALSE(serializer.LoadScene(inputFile.Path().string(), live).success);
    EXPECT_EQ(live.header.timestamp, UINT64_C(42));
}

TEST(SceneSerializerReal_ComponentSchemaRejectsMissingUnknownAndWrongTypedFields)
{
    using namespace SparkEditor;
    TemporarySceneFile file(".sparkscene");
    SceneSerializer serializer;
    SceneFile live;
    live.header.timestamp = 99;

    const auto rejects = [&](const std::string& fields)
    {
        std::ofstream output(file.Path(), std::ios::binary | std::ios::trunc);
        output << "{\"version\":2,\"objectCount\":1,\"componentCount\":1,"
                  "\"objects\":[{\"id\":1,\"componentTypes\":[\"Health\"]}],"
                  "\"components\":[{\"type\":\"Health\",\"objectID\":1,\"data\":{\"schema\":1,\"fields\":"
               << fields << "}}]}";
        output.close();
        EXPECT_FALSE(serializer.LoadScene(file.Path().string(), live).success);
        EXPECT_EQ(live.header.timestamp, UINT64_C(99));
    };

    rejects(R"({"health":100.0})");
    rejects(R"({"health":100.0,"maxHealth":100.0,"unknown":1})");
    rejects(R"({"health":"100","maxHealth":100.0})");
}

TEST(SceneSerializerReal_BinaryFormatFailsWithoutWriting)
{
    using namespace SparkEditor;
    TemporarySceneFile file(".spks");
    SceneSerializer serializer;
    SceneFile scene;

    const auto result = serializer.SaveScene(scene, file.Path().string(), SerializationFormat::BINARY);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(std::filesystem::exists(file.Path()));

    SceneFile liveScene;
    SceneObject existing;
    existing.id = 77;
    liveScene.objects.push_back(existing);
    const auto load = serializer.LoadScene(file.Path().string(), liveScene);
    EXPECT_FALSE(load.success);
    EXPECT_EQ(liveScene.objects.size(), size_t{1});
}

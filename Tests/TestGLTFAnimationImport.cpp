/**
 * @file TestGLTFAnimationImport.cpp
 * @brief glTF skeleton and animation import through AnimationManager, plus skinned glTF in the portable MeshAsset.
 *
 * Every fixture is a GLB assembled in-test: Armature (non-joint) -> Root joint (+1 Y) -> Tip
 * joint (+2 Y), with a Body node binding a three-vertex triangle to the skin. Expected values are
 * written out by hand from the authored transforms and keys, not recomputed with loader math.
 */

#include "TestFramework.h"
#include "Engine/Animation/AnimationSystem.h"
#include "Graphics/AssetPipeline.h"
#include "Graphics/GLTFAnimationLoader.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace
{
    using Spark::Animation::AnimationClip;
    using Spark::Animation::AnimationManager;
    using Spark::Graphics::Detail::LoadGLTFAnimationClips;

    constexpr uint32_t kUnsignedByte = 5121;
    constexpr uint32_t kUnsignedShort = 5123;
    constexpr uint32_t kFloat = 5126;
    constexpr float kHalfSqrt2 = 0.70710678f;

    struct Sampler
    {
        std::vector<float> times;
        std::vector<float> values;
        const char* type = "VEC3";
        std::string interpolation = "LINEAR";
        /// Written as the output accessor's count; defaults to times.size().
        size_t outputCount = 0;
    };

    struct Channel
    {
        size_t sampler = 0;
        int node = 0;
        std::string path;
    };

    struct Animation
    {
        std::string name;
        std::vector<Sampler> samplers;
        std::vector<Channel> channels;
    };

    struct Fixture
    {
        bool skinned = true;
        std::string armatureTranslation = "[0,0,0]";
        std::string skinJoints = "[1,2]";
        /// WEIGHTS_0 for the three vertices (four per vertex); every row sums to 1 by default.
        std::vector<float> weights = {0.25f, 0.75f, 0, 0, 1, 0, 0, 0, 0.5f, 0.5f, 0, 0};
        std::vector<Animation> animations;
    };

    /// "Wave": Root translates (0,1,0) -> (0,3,0) over 2 s; Tip rotates 0 -> 90 degrees about Z over 1 s.
    Animation WaveAnimation()
    {
        Animation wave;
        wave.name = "Wave";
        wave.samplers.push_back({{0.0f, 2.0f}, {0, 1, 0, 0, 3, 0}, "VEC3"});
        // The second key is deliberately not unit length; the importer must renormalize it.
        wave.samplers.push_back({{0.0f, 1.0f}, {0, 0, 0, 1, 0, 0, 2 * kHalfSqrt2, 2 * kHalfSqrt2}, "VEC4"});
        wave.channels.push_back({0, 1, "translation"});
        wave.channels.push_back({1, 2, "rotation"});
        return wave;
    }

    class GLBBuilder
    {
      public:
        size_t AddFloats(const std::vector<float>& values, size_t count, const char* type)
        {
            const size_t start = m_bin.size();
            for (float value : values)
            {
                AppendU32(m_bin, std::bit_cast<uint32_t>(value));
            }
            return AddAccessor(start, kFloat, count, type);
        }

        size_t AddBytes(const std::vector<uint8_t>& bytes, uint32_t componentType, size_t count, const char* type)
        {
            const size_t start = m_bin.size();
            m_bin.insert(m_bin.end(), bytes.begin(), bytes.end());
            return AddAccessor(start, componentType, count, type);
        }

        std::vector<uint8_t> Finish(const std::string& documentTail) const
        {
            std::string json =
                "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":" + std::to_string(m_bin.size()) +
                "}],\"bufferViews\":[" + Join(m_views) + "],\"accessors\":[" + Join(m_accessors) + "]" + documentTail +
                "}";
            while (json.size() % 4 != 0)
            {
                json.push_back(' ');
            }

            std::vector<uint8_t> glb;
            AppendU32(glb, 0x46546C67);
            AppendU32(glb, 2);
            AppendU32(glb, static_cast<uint32_t>(12 + 8 + json.size() + 8 + m_bin.size()));
            AppendU32(glb, static_cast<uint32_t>(json.size()));
            AppendU32(glb, 0x4E4F534A);
            glb.insert(glb.end(), json.begin(), json.end());
            AppendU32(glb, static_cast<uint32_t>(m_bin.size()));
            AppendU32(glb, 0x004E4942);
            glb.insert(glb.end(), m_bin.begin(), m_bin.end());
            return glb;
        }

        static std::string Join(const std::vector<std::string>& items)
        {
            std::string joined;
            for (size_t i = 0; i < items.size(); ++i)
            {
                joined += (i ? "," : "") + items[i];
            }
            return joined;
        }

      private:
        static void AppendU32(std::vector<uint8_t>& bytes, uint32_t value)
        {
            for (int shift = 0; shift < 32; shift += 8)
            {
                bytes.push_back(static_cast<uint8_t>(value >> shift));
            }
        }

        size_t AddAccessor(size_t start, uint32_t componentType, size_t count, const char* type)
        {
            m_views.push_back("{\"buffer\":0,\"byteOffset\":" + std::to_string(start) +
                              ",\"byteLength\":" + std::to_string(m_bin.size() - start) + "}");
            while (m_bin.size() % 4 != 0)
            {
                m_bin.push_back(0);
            }
            m_accessors.push_back("{\"bufferView\":" + std::to_string(m_views.size() - 1) +
                                  ",\"componentType\":" + std::to_string(componentType) +
                                  ",\"count\":" + std::to_string(count) + ",\"type\":\"" + type + "\"}");
            return m_accessors.size() - 1;
        }

        std::vector<uint8_t> m_bin;
        std::vector<std::string> m_views;
        std::vector<std::string> m_accessors;
    };

    std::vector<uint8_t> BuildGLB(const Fixture& fixture)
    {
        GLBBuilder builder;
        const size_t positions = builder.AddFloats({0, 0, 0, 1, 0, 0, 0, 1, 0}, 3, "VEC3");
        const size_t normals = builder.AddFloats({0, 0, 1, 0, 0, 1, 0, 0, 1}, 3, "VEC3");
        // Skin joint 0 is Root (bone 0), joint 1 is Tip (bone 1).
        const size_t joints = builder.AddBytes({0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0}, kUnsignedByte, 3, "VEC4");
        const size_t weights = builder.AddFloats(fixture.weights, 3, "VEC4");
        const size_t indices = builder.AddBytes({0, 0, 1, 0, 2, 0}, kUnsignedShort, 3, "SCALAR");

        std::vector<std::string> animations;
        for (const Animation& animation : fixture.animations)
        {
            std::vector<std::string> samplers;
            for (const Sampler& sampler : animation.samplers)
            {
                const size_t components = std::string(sampler.type) == "VEC4" ? 4 : 3;
                const size_t outputCount = sampler.outputCount ? sampler.outputCount : sampler.times.size();
                const size_t input = builder.AddFloats(sampler.times, sampler.times.size(), "SCALAR");
                std::vector<float> values = sampler.values;
                values.resize(outputCount * components, 0.0f);
                const size_t output = builder.AddFloats(values, outputCount, sampler.type);
                samplers.push_back("{\"input\":" + std::to_string(input) + ",\"output\":" + std::to_string(output) +
                                   ",\"interpolation\":\"" + sampler.interpolation + "\"}");
            }
            std::vector<std::string> channels;
            for (const Channel& channel : animation.channels)
            {
                channels.push_back("{\"sampler\":" + std::to_string(channel.sampler) + ",\"target\":{\"node\":" +
                                   std::to_string(channel.node) + ",\"path\":\"" + channel.path + "\"}}");
            }
            animations.push_back("{\"name\":\"" + animation.name + "\",\"samplers\":[" + GLBBuilder::Join(samplers) +
                                 "],\"channels\":[" + GLBBuilder::Join(channels) + "]}");
        }

        const std::string skinAttributes =
            fixture.skinned ? ",\"JOINTS_0\":" + std::to_string(joints) + ",\"WEIGHTS_0\":" + std::to_string(weights)
                            : std::string();
        std::string tail = ",\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":" + std::to_string(positions) +
                           ",\"NORMAL\":" + std::to_string(normals) + skinAttributes +
                           "},\"indices\":" + std::to_string(indices) + "}]}]";
        tail += ",\"nodes\":[{\"name\":\"Armature\",\"translation\":" + fixture.armatureTranslation +
                ",\"children\":[1,3]},{\"name\":\"Root\",\"translation\":[0,1,0],\"children\":[2]},"
                "{\"name\":\"Tip\",\"translation\":[0,2,0]},{\"name\":\"Body\",\"mesh\":0" +
                std::string(fixture.skinned ? ",\"skin\":0" : "") + "}]";
        if (fixture.skinned)
        {
            tail += ",\"skins\":[{\"name\":\"Rig\",\"joints\":" + fixture.skinJoints + "}]";
        }
        if (!animations.empty())
        {
            tail += ",\"animations\":[" + GLBBuilder::Join(animations) + "]";
        }
        return builder.Finish(tail);
    }

    struct TemporaryGLB
    {
        TemporaryGLB(const std::string& name, const Fixture& fixture)
            : directory(std::filesystem::temp_directory_path() / ("spark_gltf_anim_" + name)),
              path(directory / "rig.glb")
        {
            std::error_code ec;
            std::filesystem::remove_all(directory, ec);
            std::filesystem::create_directories(directory, ec);
            const std::vector<uint8_t> glb = BuildGLB(fixture);
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
        }

        ~TemporaryGLB()
        {
            std::error_code ec;
            std::filesystem::remove_all(directory, ec);
        }

        std::filesystem::path directory;
        std::filesystem::path path;
    };

    /// Import a fixture expected to fail; returns the diagnostic and checks no clips leaked out.
    std::string RejectedClips(const std::string& name, const Fixture& fixture, bool& cleared)
    {
        TemporaryGLB glb(name, fixture);
        std::vector<AnimationClip> clips(1);
        std::string error;
        const bool loaded = LoadGLTFAnimationClips(glb.path, clips, error);
        cleared = !loaded && clips.empty() && !error.empty();
        return loaded ? std::string("<loaded>") : error;
    }

    Fixture WaveFixture()
    {
        Fixture fixture;
        fixture.animations.push_back(WaveAnimation());
        return fixture;
    }
} // namespace

TEST(GLTF_Animation_ManagerLoadsAndCachesGLTFSkeleton)
{
    TemporaryGLB glb("skeleton", WaveFixture());
    const std::string path = glb.path.string();
    auto& manager = AnimationManager::GetInstance();

    auto skeleton = manager.LoadSkeleton(path);
    ASSERT_TRUE(skeleton != nullptr);
    ASSERT_EQ(skeleton->bones.size(), 2u);
    EXPECT_EQ(skeleton->name, path);
    EXPECT_EQ(skeleton->bones[0].name, std::string("Root"));
    EXPECT_EQ(skeleton->bones[0].parentIndex, -1);
    EXPECT_EQ(skeleton->bones[1].name, std::string("Tip"));
    EXPECT_EQ(skeleton->bones[1].parentIndex, 0);
    EXPECT_EQ(skeleton->FindBone("Tip"), 1);
    EXPECT_NEAR(skeleton->bones[0].localBindPose._42, 1.0f, 1e-6f);
    EXPECT_NEAR(skeleton->bones[1].localBindPose._42, 2.0f, 1e-6f);

    // Cached by path: a second load and the registry lookup return the same object.
    EXPECT_TRUE(manager.LoadSkeleton(path) == skeleton);
    EXPECT_TRUE(manager.GetSkeleton(path) == skeleton);
}

TEST(GLTF_Animation_ManagerDoesNotCacheRejectedGLTFSkeleton)
{
    Fixture fixture = WaveFixture();
    fixture.skinned = false;
    TemporaryGLB glb("unskinned_skeleton", fixture);
    const std::string path = glb.path.string();
    auto& manager = AnimationManager::GetInstance();

    auto skeleton = manager.LoadSkeleton(path);
    ASSERT_TRUE(skeleton != nullptr);
    EXPECT_TRUE(skeleton->bones.empty());
    EXPECT_TRUE(manager.GetSkeleton(path) == nullptr);
}

TEST(GLTF_Animation_ManagerImportsLinearClipBoundToSkeleton)
{
    TemporaryGLB glb("clip", WaveFixture());
    auto& manager = AnimationManager::GetInstance();
    auto clips = manager.LoadAnimations(glb.path.string());
    ASSERT_EQ(clips.size(), 1u);

    const AnimationClip& clip = *clips[0];
    EXPECT_EQ(clip.name, std::string("Wave"));
    EXPECT_NEAR(clip.duration, 2.0f, 1e-6f);
    EXPECT_NEAR(clip.ticksPerSecond, 1.0f, 1e-6f);
    EXPECT_TRUE(clip.loop);
    ASSERT_EQ(clip.channels.size(), 2u);

    const auto* root = clip.FindChannel("Root");
    ASSERT_TRUE(root != nullptr);
    EXPECT_EQ(root->boneIndex, 0);
    ASSERT_EQ(root->positionKeys.size(), 2u);
    EXPECT_NEAR(root->positionKeys[1].time, 2.0f, 1e-6f);
    EXPECT_NEAR(root->positionKeys[1].value.y, 3.0f, 1e-6f);
    const auto rootMid = root->InterpolatePosition(1.0f);
    EXPECT_NEAR(rootMid.y, 2.0f, 1e-5f);
    // Unanimated paths carry the rest value, not the identity an empty track would sample as.
    ASSERT_EQ(root->rotationKeys.size(), 1u);
    EXPECT_NEAR(root->rotationKeys[0].value.w, 1.0f, 1e-6f);
    ASSERT_EQ(root->scaleKeys.size(), 1u);
    EXPECT_NEAR(root->scaleKeys[0].value.x, 1.0f, 1e-6f);

    const auto* tip = clip.FindChannel("Tip");
    ASSERT_TRUE(tip != nullptr);
    EXPECT_EQ(tip->boneIndex, 1);
    ASSERT_EQ(tip->positionKeys.size(), 1u);
    EXPECT_NEAR(tip->positionKeys[0].value.y, 2.0f, 1e-6f);
    ASSERT_EQ(tip->rotationKeys.size(), 2u);
    EXPECT_NEAR(tip->rotationKeys[1].value.z, kHalfSqrt2, 1e-6f);
    EXPECT_NEAR(tip->rotationKeys[1].value.w, kHalfSqrt2, 1e-6f);

    manager.RegisterClip("GLTF_Animation_Wave", clips[0]);
    EXPECT_TRUE(manager.GetClip("GLTF_Animation_Wave") == clips[0]);
}

TEST(GLTF_Animation_FileWithoutAnimationsYieldsNoClips)
{
    TemporaryGLB glb("no_animations", Fixture{});
    std::vector<AnimationClip> clips(1);
    std::string error;
    EXPECT_TRUE(LoadGLTFAnimationClips(glb.path, clips, error));
    EXPECT_TRUE(clips.empty());
    EXPECT_TRUE(error.empty());
}

TEST(GLTF_Animation_ManagerReturnsNoClipsWhenAnyAnimationIsRejected)
{
    Fixture fixture = WaveFixture();
    Animation stepped = WaveAnimation();
    stepped.name = "Stepped";
    stepped.samplers[1].interpolation = "STEP";
    fixture.animations.push_back(stepped);
    TemporaryGLB glb("partial", fixture);
    EXPECT_TRUE(AnimationManager::GetInstance().LoadAnimations(glb.path.string()).empty());
}

TEST(GLTF_Animation_RejectsClipsWhenSkinnedMeshWeightsAreInvalid)
{
    // The skin hierarchy is valid but vertex 2's weights sum to 0.5, so the skinned importer rejects
    // the file. LoadSkeleton() therefore yields no skeleton, and clips must not be produced for it.
    Fixture fixture = WaveFixture();
    fixture.weights[8] = 0.25f;
    fixture.weights[9] = 0.25f;
    bool cleared = false;
    const std::string error = RejectedClips("invalid_weights", fixture, cleared);
    EXPECT_TRUE(cleared);
    EXPECT_STR_CONTAINS(error, "skinned mesh rejected");
    EXPECT_STR_CONTAINS(error, "weights summing to");

    TemporaryGLB glb("invalid_weights_manager", fixture);
    auto& manager = AnimationManager::GetInstance();
    EXPECT_TRUE(manager.LoadSkeleton(glb.path.string())->bones.empty());
    EXPECT_TRUE(manager.LoadAnimations(glb.path.string()).empty());
}

TEST(GLTF_Animation_RejectsNonLinearInterpolation)
{
    Fixture fixture = WaveFixture();
    fixture.animations[0].samplers[0].interpolation = "CUBICSPLINE";
    fixture.animations[0].samplers[0].outputCount = 6;
    bool cleared = false;
    const std::string error = RejectedClips("cubic", fixture, cleared);
    EXPECT_TRUE(cleared);
    EXPECT_STR_CONTAINS(error, "only LINEAR");
}

TEST(GLTF_Animation_RejectsNonJointTarget)
{
    Fixture fixture = WaveFixture();
    fixture.animations[0].channels[0].node = 0;
    bool cleared = false;
    const std::string error = RejectedClips("non_joint", fixture, cleared);
    EXPECT_TRUE(cleared);
    EXPECT_STR_CONTAINS(error, "non-joint node 0 'Armature'");
}

TEST(GLTF_Animation_RejectsMorphWeightChannel)
{
    Fixture fixture = WaveFixture();
    fixture.animations[0].channels[0].path = "weights";
    bool cleared = false;
    const std::string error = RejectedClips("weights", fixture, cleared);
    EXPECT_TRUE(cleared);
    // cgltf_validate already refuses a weights channel on a node without morph targets; the
    // importer's own weights check backs it up for files that do declare targets.
    EXPECT_STR_CONTAINS(error, "cgltf validation failed");
}

TEST(GLTF_Animation_RejectsAnimatedRootUnderOffsetArmature)
{
    Fixture fixture = WaveFixture();
    fixture.armatureTranslation = "[0,0,2]";
    bool cleared = false;
    const std::string error = RejectedClips("offset_root", fixture, cleared);
    EXPECT_TRUE(cleared);
    EXPECT_STR_CONTAINS(error, "non-identity non-joint ancestor");

    // Animating only the child joint stays valid under the same offset armature.
    fixture.animations[0].channels.erase(fixture.animations[0].channels.begin());
    TemporaryGLB glb("offset_child", fixture);
    std::vector<AnimationClip> clips;
    std::string loadError;
    EXPECT_TRUE(LoadGLTFAnimationClips(glb.path, clips, loadError));
    EXPECT_EQ(clips.size(), 1u);
}

TEST(GLTF_Animation_RejectsNonIncreasingKeyTimes)
{
    Fixture fixture = WaveFixture();
    fixture.animations[0].samplers[0].times = {1.0f, 1.0f};
    bool cleared = false;
    const std::string error = RejectedClips("times", fixture, cleared);
    EXPECT_TRUE(cleared);
    EXPECT_STR_CONTAINS(error, "strictly increasing");
}

TEST(GLTF_Animation_RejectsOutputCountMismatch)
{
    Fixture fixture = WaveFixture();
    fixture.animations[0].samplers[0].outputCount = 3;
    bool cleared = false;
    const std::string error = RejectedClips("output_count", fixture, cleared);
    EXPECT_TRUE(cleared);
    // cgltf_validate catches the LINEAR count mismatch before the importer's own count check.
    EXPECT_STR_CONTAINS(error, "cgltf validation failed");
}

TEST(GLTF_Animation_RejectsDuplicateChannelPath)
{
    Fixture fixture = WaveFixture();
    fixture.animations[0].channels.push_back({0, 1, "translation"});
    bool cleared = false;
    const std::string error = RejectedClips("duplicate_path", fixture, cleared);
    EXPECT_TRUE(cleared);
    EXPECT_STR_CONTAINS(error, "duplicates another channel's target path");
}

TEST(GLTF_Animation_RejectsDuplicateAnimationName)
{
    Fixture fixture = WaveFixture();
    fixture.animations.push_back(WaveAnimation());
    bool cleared = false;
    const std::string error = RejectedClips("duplicate_name", fixture, cleared);
    EXPECT_TRUE(cleared);
    EXPECT_STR_CONTAINS(error, "duplicates another animation's name");
}

TEST(GLTF_Animation_RejectsZeroLengthRotation)
{
    Fixture fixture = WaveFixture();
    fixture.animations[0].samplers[1].values = {0, 0, 0, 1, 0, 0, 0, 0};
    bool cleared = false;
    const std::string error = RejectedClips("zero_quaternion", fixture, cleared);
    EXPECT_TRUE(cleared);
    EXPECT_STR_CONTAINS(error, "zero-length quaternion");
}

TEST(GLTF_Animation_RejectsWrongOutputType)
{
    Fixture fixture = WaveFixture();
    fixture.animations[0].samplers[1].type = "VEC3";
    fixture.animations[0].samplers[1].values = {0, 0, 0, 0, 0, 0};
    bool cleared = false;
    const std::string error = RejectedClips("wrong_type", fixture, cleared);
    EXPECT_TRUE(cleared);
    EXPECT_STR_CONTAINS(error, "rotation output must be");
}

// The portable MeshAsset (AssetTypesLinux.cpp) loads without a device and keeps skin influences.
// The Windows D3D11 MeshAsset (AssetTypesWindows.cpp) requires a device and still rejects skinned
// glTF, so these tests are not built there; Tests/CMakeLists.txt lowers the exact count to match.
#ifndef SPARK_PLATFORM_WINDOWS
TEST(GLTF_Animation_PortableMeshAssetKeepsSkinInfluences)
{
    TemporaryGLB glb("mesh_asset", WaveFixture());
    MeshAsset asset(glb.path.string());
    ASSERT_TRUE(SUCCEEDED(asset.Load(nullptr)));

    const MeshAssetData& data = asset.GetMeshData();
    ASSERT_EQ(data.vertices.size(), 3u);
    ASSERT_EQ(data.indices.size(), 3u);
    // Vertex 0: 0.25 on Root (bone 0), 0.75 on Tip (bone 1).
    EXPECT_EQ(data.vertices[0].boneIndices.x, 0u);
    EXPECT_EQ(data.vertices[0].boneIndices.y, 1u);
    EXPECT_NEAR(data.vertices[0].boneWeights.x, 0.25f, 1e-6f);
    EXPECT_NEAR(data.vertices[0].boneWeights.y, 0.75f, 1e-6f);
    EXPECT_EQ(data.vertices[1].boneIndices.x, 1u);
    EXPECT_NEAR(data.vertices[1].boneWeights.x, 1.0f, 1e-6f);
    for (const auto& vertex : data.vertices)
    {
        const float sum = vertex.boneWeights.x + vertex.boneWeights.y + vertex.boneWeights.z + vertex.boneWeights.w;
        EXPECT_NEAR(sum, 1.0f, 1e-6f);
    }
    EXPECT_EQ(asset.GetMetadata().customProperties.at("gltf.boneCount"), std::string("2"));

    // The bone indices address the skeleton AnimationManager builds from the same file.
    auto skeleton = AnimationManager::GetInstance().LoadSkeleton(glb.path.string());
    ASSERT_EQ(skeleton->bones.size(), 2u);
    EXPECT_EQ(skeleton->bones[data.vertices[1].boneIndices.x].name, std::string("Tip"));
}

TEST(GLTF_Animation_PortableMeshAssetStaticGLBHasNoInfluences)
{
    Fixture fixture;
    fixture.skinned = false;
    TemporaryGLB glb("static_mesh_asset", fixture);
    MeshAsset asset(glb.path.string());
    ASSERT_TRUE(SUCCEEDED(asset.Load(nullptr)));

    const MeshAssetData& data = asset.GetMeshData();
    ASSERT_EQ(data.vertices.size(), 3u);
    EXPECT_NEAR(data.vertices[0].boneWeights.x, 0.0f, 1e-6f);
    EXPECT_TRUE(asset.GetMetadata().customProperties.count("gltf.boneCount") == 0);
}

TEST(GLTF_Animation_PortableMeshAssetFailsOnInvalidSkin)
{
    Fixture fixture = WaveFixture();
    fixture.skinJoints = "[1,1]";
    TemporaryGLB glb("invalid_skin_asset", fixture);
    MeshAsset asset(glb.path.string());
    EXPECT_FALSE(SUCCEEDED(asset.Load(nullptr)));
    EXPECT_TRUE(asset.GetMeshData().vertices.empty());
}
#endif // SPARK_PLATFORM_WINDOWS

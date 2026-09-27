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
#include "Graphics/GLTFSkinnedMeshLoader.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
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
        /// Write the skin's inverseBindMatrices: the inverses of Root's (+1 Y) and Tip's (+3 Y) bind poses.
        bool inverseBinds = false;
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
        std::string inverseBinds;
        if (fixture.inverseBinds)
        {
            // Column-major translations by -1 and -3 along Y.
            const size_t accessor = builder.AddFloats(
                {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -3, 0, 1}, 2,
                "MAT4");
            inverseBinds = ",\"inverseBindMatrices\":" + std::to_string(accessor);
        }

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
            tail += ",\"skins\":[{\"name\":\"Rig\",\"joints\":" + fixture.skinJoints + inverseBinds + "}]";
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
    Animation cubic = WaveAnimation();
    cubic.name = "Cubic";
    cubic.samplers[1].interpolation = "CUBICSPLINE";
    cubic.samplers[1].outputCount = 6;
    fixture.animations.push_back(cubic);
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

TEST(GLTF_Animation_RejectsCubicSplineInterpolation)
{
    Fixture fixture = WaveFixture();
    fixture.animations[0].samplers[0].interpolation = "CUBICSPLINE";
    fixture.animations[0].samplers[0].outputCount = 6;
    bool cleared = false;
    const std::string error = RejectedClips("cubic", fixture, cleared);
    EXPECT_TRUE(cleared);
    EXPECT_STR_CONTAINS(error, "channel 0 uses CUBICSPLINE interpolation; only LINEAR and STEP are supported");
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

// ---------------------------------------------------------------------------------------------
// Posed-vertex checks through the production evaluator against an independent skinning reference.
//
// The "Pose" clip is sampled with AnimationEvaluator::SampleClip and ComputeSkinningMatrices on the
// skeleton AnimationManager imports, and the mesh the skinned importer imports is skinned with
// those matrices. The reference below never touches engine math, DirectXMath or quaternions: it
// writes each joint's glTF TRS at time t in closed form from the authored keys, builds rotations
// with Rodrigues' formula, and composes column-vector matrices exactly as the glTF 2.0 spec does
// (global = parent * T * R * S, skin = global * inverseBind).
// ---------------------------------------------------------------------------------------------
namespace
{
    using Spark::Animation::AnimationEvaluator;
    using Spark::Graphics::Detail::GLTFSkinnedMeshData;
    using Spark::Graphics::Detail::LoadGLTFSkinnedMesh;

    constexpr float kPi = 3.14159265358979f;
    constexpr float kPoseTolerance = 1.0e-4f;

    /// "Pose" over 2 s. Root: translation (0,1,0) -> (2,1,0) and 0 -> 90 degrees about Z, both LINEAR.
    /// Tip: 0 -> 120 degrees about X (LINEAR) and STEP scale 1 -> 2 (at 1 s) -> 0.5 (at 2 s).
    Animation PoseAnimation()
    {
        Animation pose;
        pose.name = "Pose";
        pose.samplers.push_back({{0.0f, 2.0f}, {0, 1, 0, 2, 1, 0}, "VEC3"});
        pose.samplers.push_back({{0.0f, 2.0f}, {0, 0, 0, 1, 0, 0, kHalfSqrt2, kHalfSqrt2}, "VEC4"});
        pose.samplers.push_back({{0.0f, 2.0f}, {0, 0, 0, 1, 0.8660254f, 0, 0, 0.5f}, "VEC4"});
        pose.samplers.push_back({{0.0f, 1.0f, 2.0f}, {1, 1, 1, 2, 2, 2, 0.5f, 0.5f, 0.5f}, "VEC3", "STEP"});
        pose.channels.push_back({0, 1, "translation"});
        pose.channels.push_back({1, 1, "rotation"});
        pose.channels.push_back({2, 2, "rotation"});
        pose.channels.push_back({3, 2, "scale"});
        return pose;
    }

    Fixture PoseFixture()
    {
        Fixture fixture;
        fixture.inverseBinds = true;
        fixture.animations.push_back(PoseAnimation());
        return fixture;
    }

    /// Row-major 4x4 matrix applied to column vectors (glTF convention).
    struct Mat4
    {
        float r[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    };

    Mat4 Multiply(const Mat4& a, const Mat4& b)
    {
        Mat4 product;
        for (int row = 0; row < 4; ++row)
        {
            for (int column = 0; column < 4; ++column)
            {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k)
                {
                    sum += a.r[row][k] * b.r[k][column];
                }
                product.r[row][column] = sum;
            }
        }
        return product;
    }

    Mat4 Translation(float x, float y, float z)
    {
        Mat4 m;
        m.r[0][3] = x;
        m.r[1][3] = y;
        m.r[2][3] = z;
        return m;
    }

    Mat4 UniformScale(float s)
    {
        Mat4 m;
        m.r[0][0] = s;
        m.r[1][1] = s;
        m.r[2][2] = s;
        return m;
    }

    /// Right-handed rotation by `degrees` about the unit axis (x, y, z), via Rodrigues' formula.
    Mat4 Rotation(float x, float y, float z, float degrees)
    {
        const float angle = degrees * kPi / 180.0f;
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        const float t = 1.0f - c;
        Mat4 m;
        m.r[0][0] = c + x * x * t;
        m.r[0][1] = x * y * t - z * s;
        m.r[0][2] = x * z * t + y * s;
        m.r[1][0] = y * x * t + z * s;
        m.r[1][1] = c + y * y * t;
        m.r[1][2] = y * z * t - x * s;
        m.r[2][0] = z * x * t - y * s;
        m.r[2][1] = z * y * t + x * s;
        m.r[2][2] = c + z * z * t;
        return m;
    }

    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    /// Reference posed vertices of the fixture triangle at clip time t (0 <= t <= 2), from the keys in closed form.
    std::vector<Vec3> ReferencePose(float t)
    {
        // Armature (non-joint) has the identity transform, so Root's local transform is its global one.
        const Mat4 root = Multiply(Translation(t, 1.0f, 0.0f), Rotation(0, 0, 1, 45.0f * t));
        const float tipScale = t < 1.0f ? 1.0f : (t < 2.0f ? 2.0f : 0.5f);
        const Mat4 tip = Multiply(
            root, Multiply(Translation(0, 2, 0), Multiply(Rotation(1, 0, 0, 60.0f * t), UniformScale(tipScale))));
        const Mat4 skin[2] = {Multiply(root, Translation(0, -1, 0)), Multiply(tip, Translation(0, -3, 0))};

        // Authored positions and (Root, Tip) weights of the three vertices.
        const float positions[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
        const float weights[3][2] = {{0.25f, 0.75f}, {0.0f, 1.0f}, {0.5f, 0.5f}};
        std::vector<Vec3> posed(3);
        for (int v = 0; v < 3; ++v)
        {
            float out[3] = {};
            for (int joint = 0; joint < 2; ++joint)
            {
                for (int row = 0; row < 3; ++row)
                {
                    const Mat4& m = skin[joint];
                    out[row] += weights[v][joint] * (m.r[row][0] * positions[v][0] + m.r[row][1] * positions[v][1] +
                                                     m.r[row][2] * positions[v][2] + m.r[row][3]);
                }
            }
            posed[v] = {out[0], out[1], out[2]};
        }
        return posed;
    }

    /// Linear blend skinning with engine skinning matrices (row vectors, translation in _41.._43).
    std::vector<Vec3> SkinVertices(const GLTFSkinnedMeshData& mesh, const std::vector<XMFLOAT4X4>& palette)
    {
        std::vector<Vec3> posed;
        for (const auto& vertex : mesh.vertices)
        {
            Vec3 out;
            for (size_t influence = 0; influence < 4; ++influence)
            {
                const float weight = vertex.weights[influence];
                if (weight == 0.0f)
                {
                    continue;
                }
                const XMFLOAT4X4& m = palette.at(vertex.joints[influence]);
                const auto& p = vertex.position;
                out.x += weight * (p[0] * m.m[0][0] + p[1] * m.m[1][0] + p[2] * m.m[2][0] + m.m[3][0]);
                out.y += weight * (p[0] * m.m[0][1] + p[1] * m.m[1][1] + p[2] * m.m[2][1] + m.m[3][1]);
                out.z += weight * (p[0] * m.m[0][2] + p[1] * m.m[1][2] + p[2] * m.m[2][2] + m.m[3][2]);
            }
            posed.push_back(out);
        }
        return posed;
    }

    /// Largest per-component distance between two posed triangles (infinity when sizes differ).
    float MaxDeviation(const std::vector<Vec3>& a, const std::vector<Vec3>& b)
    {
        if (a.size() != b.size())
        {
            return std::numeric_limits<float>::infinity();
        }
        float deviation = 0.0f;
        for (size_t i = 0; i < a.size(); ++i)
        {
            deviation =
                std::max({deviation, std::abs(a[i].x - b[i].x), std::abs(a[i].y - b[i].y), std::abs(a[i].z - b[i].z)});
        }
        return deviation;
    }

    /// The production path: skeleton and clip through AnimationManager, mesh through the skinned importer.
    struct ImportedRig
    {
        std::shared_ptr<Spark::Animation::Skeleton> skeleton;
        std::shared_ptr<AnimationClip> clip;
        GLTFSkinnedMeshData mesh;

        std::vector<Vec3> Pose(const AnimationClip& sampled, float time) const
        {
            std::vector<XMFLOAT4X4> local;
            std::vector<XMFLOAT4X4> palette;
            AnimationEvaluator::SampleClip(sampled, *skeleton, time, local);
            AnimationEvaluator::ComputeSkinningMatrices(*skeleton, local, palette);
            return SkinVertices(mesh, palette);
        }
    };

    bool ImportRig(const TemporaryGLB& glb, ImportedRig& rig)
    {
        auto& manager = AnimationManager::GetInstance();
        rig.skeleton = manager.LoadSkeleton(glb.path.string());
        auto clips = manager.LoadAnimations(glb.path.string());
        std::string error;
        if (!rig.skeleton || rig.skeleton->bones.size() != 2 || clips.size() != 1 ||
            !LoadGLTFSkinnedMesh(glb.path, rig.mesh, error))
        {
            return false;
        }
        rig.clip = clips[0];
        return true;
    }
} // namespace

TEST(GLTF_Animation_BindPoseSkinsToAuthoredPositions)
{
    TemporaryGLB glb("bind_pose", PoseFixture());
    ImportedRig rig;
    ASSERT_TRUE(ImportRig(glb, rig));

    // The bind pose times the inverse bind matrices is the identity, so every vertex stays put.
    std::vector<XMFLOAT4X4> local;
    for (const auto& bone : rig.skeleton->bones)
    {
        local.push_back(bone.localBindPose);
    }
    std::vector<XMFLOAT4X4> palette;
    AnimationEvaluator::ComputeSkinningMatrices(*rig.skeleton, local, palette);
    const std::vector<Vec3> authored = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    EXPECT_TRUE(MaxDeviation(SkinVertices(rig.mesh, palette), authored) < kPoseTolerance);
    // The clip's first keys are the rest pose, so t = 0 matches too.
    EXPECT_TRUE(MaxDeviation(rig.Pose(*rig.clip, 0.0f), authored) < kPoseTolerance);
}

TEST(GLTF_Animation_PosedVerticesMatchIndependentSkinningReference)
{
    TemporaryGLB glb("posed", PoseFixture());
    ImportedRig rig;
    ASSERT_TRUE(ImportRig(glb, rig));
    EXPECT_NEAR(rig.clip->duration, 2.0f, 1e-6f);
    EXPECT_TRUE(rig.clip->loop);

    // Start, between keys, the STEP key, mid-STEP interval, and the last key before it.
    for (const float time : {0.0f, 0.5f, 1.0f, 1.5f, 1.999f})
    {
        const float deviation = MaxDeviation(rig.Pose(*rig.clip, time), ReferencePose(time));
        EXPECT_TRUE(deviation < kPoseTolerance);
        if (deviation >= kPoseTolerance)
        {
            std::fprintf(stderr, "  posed-vertex deviation %.6f at t=%.3f\n", deviation, time);
        }
    }

    // A looping clip wraps its end time to the start; a one-shot clip holds the last keys.
    EXPECT_TRUE(MaxDeviation(rig.Pose(*rig.clip, 2.0f), ReferencePose(0.0f)) < kPoseTolerance);
    AnimationClip oneShot = *rig.clip;
    oneShot.loop = false;
    EXPECT_TRUE(MaxDeviation(rig.Pose(oneShot, 2.0f), ReferencePose(2.0f)) < kPoseTolerance);
}

TEST(GLTF_Animation_StepSamplerHoldsPreviousKeyUntilNextKey)
{
    TemporaryGLB glb("step", PoseFixture());
    ImportedRig rig;
    ASSERT_TRUE(ImportRig(glb, rig));
    const auto* tip = rig.clip->FindChannel("Tip");
    ASSERT_TRUE(tip != nullptr);

    const float beforeKey = std::nextafter(1.0f, 0.0f);
    EXPECT_NEAR(tip->InterpolateScale(0.5f).x, 1.0f, 1e-6f);
    EXPECT_NEAR(tip->InterpolateScale(beforeKey).x, 1.0f, 1e-6f);
    EXPECT_NEAR(tip->InterpolateScale(1.0f).x, 2.0f, 1e-6f);
    EXPECT_NEAR(tip->InterpolateScale(std::nextafter(2.0f, 0.0f)).y, 2.0f, 1e-6f);
    EXPECT_NEAR(tip->InterpolateScale(2.0f).z, 0.5f, 1e-6f);
    // The LINEAR rotation track on the same joint still blends between its keys.
    EXPECT_NEAR(tip->InterpolateRotation(1.0f).x, 0.5f, 1e-5f);
    EXPECT_TRUE(MaxDeviation(rig.Pose(*rig.clip, beforeKey), ReferencePose(beforeKey)) < kPoseTolerance);
}

TEST(GLTF_Animation_MutatedKeyframeFailsPosedVertexComparison)
{
    // Tip's last rotation key becomes 150 degrees about X instead of 120; the reference is unchanged.
    Fixture fixture = PoseFixture();
    fixture.animations[0].samplers[2].values = {0, 0, 0, 1, 0.9659258f, 0, 0, 0.2588190f};
    TemporaryGLB glb("mutated", fixture);
    ImportedRig rig;
    ASSERT_TRUE(ImportRig(glb, rig));

    EXPECT_TRUE(MaxDeviation(rig.Pose(*rig.clip, 0.0f), ReferencePose(0.0f)) < kPoseTolerance);
    EXPECT_TRUE(MaxDeviation(rig.Pose(*rig.clip, 1.0f), ReferencePose(1.0f)) > 0.1f);
    EXPECT_TRUE(MaxDeviation(rig.Pose(*rig.clip, 1.5f), ReferencePose(1.5f)) > 0.1f);
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

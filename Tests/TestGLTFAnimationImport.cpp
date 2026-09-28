/**
 * @file TestGLTFAnimationImport.cpp
 * @brief glTF skeleton and animation import through AnimationManager, plus skinned glTF in MeshAsset (D3D11 and portable).
 *
 * Every fixture is a GLB assembled in-test by GLTFSkinningReference.h (shared with the D3D11 GPU
 * skinning test). Expected values are written out by hand from the authored transforms and keys,
 * not recomputed with loader math.
 */

#include "TestFramework.h"
#include "Engine/Animation/AnimationSystem.h"
#include "Graphics/AssetPipeline.h"
#include "Graphics/GLTFAnimationLoader.h"
#include "Graphics/GLTFSkinnedMeshLoader.h"
#include "GLTFSkinningReference.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{
    using namespace SparkTest::GLTFRig;
    using Spark::Animation::AnimationClip;
    using Spark::Animation::AnimationManager;
    using Spark::Graphics::Detail::LoadGLTFAnimationClips;

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

TEST(GLTF_Animation_ManagerLoadsNonASCIIUtf8Path)
{
    // "L<a-umlaut>ufer" as UTF-8. Decoding the manager's UTF-8 path with the Windows ANSI code page
    // would turn it into mojibake, and neither the skeleton nor the clips would be found.
    TemporaryGLB glb("L\xC3\xA4ufer", WaveFixture());
    ASSERT_TRUE(std::filesystem::is_regular_file(glb.path));
    const std::string path = PathToUtf8(glb.path);
    auto& manager = AnimationManager::GetInstance();

    auto skeleton = manager.LoadSkeleton(path);
    ASSERT_TRUE(skeleton != nullptr);
    EXPECT_EQ(skeleton->bones.size(), 2u);
    EXPECT_EQ(manager.LoadAnimations(path).size(), 1u);
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

TEST(GLTF_Animation_StepZeroLengthRotationReportsFileKeyIndex)
{
    // STEP import inserts a hold key before every key after the first; the diagnostic must still
    // name the key as the file numbers it (key 1), not its expanded position (key 2).
    Fixture fixture = WaveFixture();
    fixture.animations[0].samplers[1].interpolation = "STEP";
    fixture.animations[0].samplers[1].values = {0, 0, 0, 1, 0, 0, 0, 0};
    bool cleared = false;
    const std::string error = RejectedClips("step_zero_quaternion", fixture, cleared);
    EXPECT_TRUE(cleared);
    EXPECT_STR_CONTAINS(error, "rotation key 1 is a zero-length quaternion");
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
// those matrices, then compared with ReferencePose() from GLTFSkinningReference.h, which never
// touches engine math, DirectXMath or quaternions.
// ---------------------------------------------------------------------------------------------
namespace
{
    using Spark::Animation::AnimationEvaluator;
    using Spark::Graphics::Detail::GLTFSkinnedMeshData;
    using Spark::Graphics::Detail::LoadGLTFSkinnedMesh;

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

TEST(GLTF_Animation_MaxDeviationFailsOnNonFiniteVertices)
{
    const std::vector<Vec3> reference = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    std::vector<Vec3> posed = reference;
    posed[1].y = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(MaxDeviation(posed, reference) < kPoseTolerance);
    EXPECT_FALSE(MaxDeviation(reference, posed) < kPoseTolerance);
    posed[1].y = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(MaxDeviation(posed, reference) < kPoseTolerance);
    EXPECT_TRUE(MaxDeviation(reference, reference) < kPoseTolerance);
}

TEST(GLTF_Animation_OppositeHemisphereRotationKeysTakeShortestArc)
{
    // Root's 90 degree key authored as -q: the same rotation, but its dot product with the identity
    // key is negative. Sampling must take the short way (45 degrees at t = 1), not the long way
    // round (-135 degrees), so the pose still matches the reference built from +q.
    Fixture fixture = PoseFixture();
    fixture.animations[0].samplers[1].values = {0, 0, 0, 1, 0, 0, -kHalfSqrt2, -kHalfSqrt2};
    TemporaryGLB glb("opposite_hemisphere", fixture);
    ImportedRig rig;
    ASSERT_TRUE(ImportRig(glb, rig));

    for (const float time : {0.5f, 1.0f, 1.5f})
    {
        EXPECT_TRUE(MaxDeviation(rig.Pose(*rig.clip, time), ReferencePose(time)) < kPoseTolerance);
    }
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

// MeshAsset keeps skin influences on every platform: the D3D11 MeshAsset (AssetTypesWindows.cpp) and
// the portable one (AssetTypesLinux.cpp) share ImportGLTFMeshAssetData. The D3D11 path needs a device,
// so Windows loads through WARP and the vertex buffer upload runs too.
namespace
{
    HRESULT LoadMeshAsset(MeshAsset& asset)
    {
#ifdef SPARK_PLATFORM_WINDOWS
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        D3D_FEATURE_LEVEL featureLevel{};
        ASSERT_TRUE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                                D3D11_SDK_VERSION, device.GetAddressOf(), &featureLevel, nullptr)));
        return asset.Load(device.Get());
#else
        return asset.Load(nullptr);
#endif
    }
} // namespace

TEST(GLTF_Animation_PortableMeshAssetKeepsSkinInfluences)
{
    TemporaryGLB glb("mesh_asset", WaveFixture());
    MeshAsset asset(glb.path.string());
    ASSERT_TRUE(SUCCEEDED(LoadMeshAsset(asset)));

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
    ASSERT_TRUE(SUCCEEDED(LoadMeshAsset(asset)));

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
    EXPECT_FALSE(SUCCEEDED(LoadMeshAsset(asset)));
    EXPECT_TRUE(asset.GetMeshData().vertices.empty());
}

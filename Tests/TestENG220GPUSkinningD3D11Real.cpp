/**
 * @file TestENG220GPUSkinningD3D11Real.cpp
 * @brief ENG-220: production GPUSkinning on a WARP D3D11 device matches the independent glTF reference.
 *
 * The skinned fixture GLB is imported through the shared MeshAsset glTF import
 * (ImportGLTFMeshAssetData), posed through AnimationManager + AnimationEvaluator, skinned by
 * GPUSkinning::DispatchSkinning running Shaders/HLSL/Compute/SkinningCS.hlsl, and read back from
 * the output UAV buffer. Every posed vertex is compared with ReferencePose() from
 * GLTFSkinningReference.h, which never touches engine math. A mutated bone matrix must fail the
 * same comparison, so the GPU readback cannot pass by accident. Windows only (CTest GPUSkinningD3D11).
 */

#include "TestFramework.h"

#ifdef SPARK_PLATFORM_WINDOWS

#include "Engine/Animation/AnimationSystem.h"
#include "Graphics/AssetPipeline.h"
#include "Graphics/GPUSkinning.h"
#include "GLTFSkinningReference.h"

#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{
    using namespace SparkTest::GLTFRig;
    using Microsoft::WRL::ComPtr;
    using Spark::Animation::AnimationClip;
    using Spark::Animation::AnimationEvaluator;
    using Spark::Animation::AnimationManager;
    using Spark::Animation::Skeleton;
    using Spark::Graphics::GPUSkinning;
    using Spark::Graphics::SkinningOutputVertex;
    using Spark::Graphics::SkinningSourceVertex;

    constexpr uint32_t kMeshId = 220;

    /// GPUSkinning compiles its shader relative to the working directory (the runtime directory in
    /// the engine); the tests compile the checked-in source from the source root.
    class ScopedCurrentDirectory
    {
      public:
        explicit ScopedCurrentDirectory(const wchar_t* path)
        {
            const DWORD required = GetCurrentDirectoryW(0, nullptr);
            m_previous.resize(required);
            GetCurrentDirectoryW(required, m_previous.data());
            SetCurrentDirectoryW(path);
        }

        ~ScopedCurrentDirectory()
        {
            if (!m_previous.empty())
                SetCurrentDirectoryW(m_previous.c_str());
        }

        ScopedCurrentDirectory(const ScopedCurrentDirectory&) = delete;
        ScopedCurrentDirectory& operator=(const ScopedCurrentDirectory&) = delete;

      private:
        std::wstring m_previous;
    };

    /// WARP device plus an initialized GPUSkinning singleton; Shutdown() runs on every exit path.
    struct SkinningDevice
    {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;

        SkinningDevice()
        {
            D3D_FEATURE_LEVEL featureLevel{};
            ASSERT_TRUE(
                SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                            device.GetAddressOf(), &featureLevel, context.GetAddressOf())));
            ScopedCurrentDirectory sourceDirectory(L"" SPARK_TEST_SOURCE_DIR_WIDE);
            ASSERT_TRUE(GPUSkinning::GetInstance().Initialize(device.Get()));
        }

        ~SkinningDevice() { GPUSkinning::GetInstance().Shutdown(); }

        SkinningDevice(const SkinningDevice&) = delete;
        SkinningDevice& operator=(const SkinningDevice&) = delete;

        /// Dispatches the registered mesh with @p palette and reads the output UAV buffer back.
        std::vector<SkinningOutputVertex> Skin(const std::vector<XMFLOAT4X4>& palette)
        {
            auto& skinning = GPUSkinning::GetInstance();
            skinning.DispatchSkinning(context.Get(), kMeshId, palette.data(), static_cast<uint32_t>(palette.size()));

            ID3D11ShaderResourceView* output = skinning.GetSkinnedBuffer(kMeshId);
            if (!output)
                return {};
            ComPtr<ID3D11Resource> resource;
            output->GetResource(resource.GetAddressOf());
            ComPtr<ID3D11Buffer> buffer;
            if (FAILED(resource.As(&buffer)))
                return {};
            D3D11_BUFFER_DESC desc{};
            buffer->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            desc.StructureByteStride = 0;
            ComPtr<ID3D11Buffer> staging;
            if (FAILED(device->CreateBuffer(&desc, nullptr, staging.GetAddressOf())))
                return {};
            context->CopyResource(staging.Get(), buffer.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
                return {};
            std::vector<SkinningOutputVertex> vertices(desc.ByteWidth / sizeof(SkinningOutputVertex));
            std::memcpy(vertices.data(), mapped.pData, vertices.size() * sizeof(SkinningOutputVertex));
            context->Unmap(staging.Get(), 0);
            return vertices;
        }
    };

    std::vector<Vec3> Positions(const std::vector<SkinningOutputVertex>& vertices)
    {
        std::vector<Vec3> positions;
        for (const auto& vertex : vertices)
            positions.push_back({vertex.position.x, vertex.position.y, vertex.position.z});
        return positions;
    }

    /// The Pose fixture through the production import, registered with GPUSkinning.
    struct GPURig
    {
        std::shared_ptr<Skeleton> skeleton;
        std::shared_ptr<AnimationClip> clip;

        explicit GPURig(const TemporaryGLB& glb)
        {
            MeshAssetData mesh;
            size_t boneCount = 0;
            std::string error;
            ASSERT_TRUE(Spark::Graphics::Detail::ImportGLTFMeshAssetData(glb.path, mesh, boneCount, error));
            ASSERT_EQ(boneCount, size_t(2));
            ASSERT_EQ(mesh.vertices.size(), size_t(3));

            std::vector<SkinningSourceVertex> source;
            for (const auto& vertex : mesh.vertices)
            {
                SkinningSourceVertex skinned{};
                skinned.position = vertex.position;
                skinned.normal = vertex.normal;
                skinned.texCoord = vertex.texCoord0;
                const uint32_t indices[4] = {vertex.boneIndices.x, vertex.boneIndices.y, vertex.boneIndices.z,
                                             vertex.boneIndices.w};
                const float weights[4] = {vertex.boneWeights.x, vertex.boneWeights.y, vertex.boneWeights.z,
                                          vertex.boneWeights.w};
                std::memcpy(skinned.boneIndices, indices, sizeof(indices));
                std::memcpy(skinned.boneWeights, weights, sizeof(weights));
                source.push_back(skinned);
            }
            ASSERT_TRUE(GPUSkinning::GetInstance().RegisterMesh(
                kMeshId, source.data(), static_cast<uint32_t>(source.size()), static_cast<uint32_t>(boneCount)));

            auto& manager = AnimationManager::GetInstance();
            skeleton = manager.LoadSkeleton(glb.path.string());
            auto clips = manager.LoadAnimations(glb.path.string());
            ASSERT_TRUE(skeleton != nullptr && skeleton->bones.size() == 2 && clips.size() == 1);
            clip = clips[0];
        }

        std::vector<XMFLOAT4X4> Palette(float time) const
        {
            std::vector<XMFLOAT4X4> local;
            std::vector<XMFLOAT4X4> palette;
            AnimationEvaluator::SampleClip(*clip, *skeleton, time, local);
            AnimationEvaluator::ComputeSkinningMatrices(*skeleton, local, palette);
            return palette;
        }
    };

    XMFLOAT4X4 TranslationMatrix(float x, float y, float z)
    {
        XMFLOAT4X4 m{};
        m._11 = 1.0f;
        m._22 = 1.0f;
        m._33 = 1.0f;
        m._44 = 1.0f;
        m._41 = x;
        m._42 = y;
        m._43 = z;
        return m;
    }
} // namespace

TEST(GPUSkinningD3D11_BindPoseMatchesAuthoredPositions)
{
    SkinningDevice gpu;
    TemporaryGLB glb("gpu_bind_pose", PoseFixture());
    GPURig rig(glb);

    std::vector<XMFLOAT4X4> local;
    for (const auto& bone : rig.skeleton->bones)
        local.push_back(bone.localBindPose);
    std::vector<XMFLOAT4X4> palette;
    AnimationEvaluator::ComputeSkinningMatrices(*rig.skeleton, local, palette);

    const std::vector<Vec3> authored = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    const std::vector<SkinningOutputVertex> skinned = gpu.Skin(palette);
    ASSERT_EQ(skinned.size(), size_t(3));
    EXPECT_TRUE(MaxDeviation(Positions(skinned), authored) < kPoseTolerance);
    // The bind pose leaves the authored +Z normals unchanged.
    for (const auto& vertex : skinned)
        EXPECT_NEAR(vertex.normal.z, 1.0f, 1e-4f);
}

TEST(GPUSkinningD3D11_PosedFramesMatchCPUReference)
{
    SkinningDevice gpu;
    TemporaryGLB glb("gpu_posed", PoseFixture());
    GPURig rig(glb);

    // Start, the STEP key, and mid-STEP interval of the "Pose" clip.
    for (const float time : {0.0f, 1.0f, 1.5f})
    {
        const float deviation = MaxDeviation(Positions(gpu.Skin(rig.Palette(time))), ReferencePose(time));
        EXPECT_TRUE(deviation < kPoseTolerance);
        if (!(deviation < kPoseTolerance))
            std::fprintf(stderr, "  GPU posed-vertex deviation %.6f at t=%.3f\n", deviation, time);
    }
}

TEST(GPUSkinningD3D11_MutatedBoneFailsComparison)
{
    SkinningDevice gpu;
    TemporaryGLB glb("gpu_mutated", PoseFixture());
    GPURig rig(glb);

    std::vector<XMFLOAT4X4> palette = rig.Palette(1.0f);
    EXPECT_TRUE(MaxDeviation(Positions(gpu.Skin(palette)), ReferencePose(1.0f)) < kPoseTolerance);

    // Tip's skinning matrix moved by 0.5 along Y: every vertex Tip influences must leave tolerance.
    palette[1]._42 += 0.5f;
    EXPECT_TRUE(MaxDeviation(Positions(gpu.Skin(palette)), ReferencePose(1.0f)) > 0.1f);
}

TEST(GPUSkinningD3D11_FourInfluenceBlendMatchesReference)
{
    SkinningDevice gpu;

    // Translation-only bones, so the expected blend is p + sum(w_i * t_i) in closed form. Bone 4 is
    // poisoned with a NaN translation: a zero weight on it must be skipped, not multiplied in
    // (0 * NaN is NaN, which MaxDeviation reports as an infinite deviation).
    const float translations[4][3] = {{1, 0, 0}, {0, 2, 0}, {0, 0, 3}, {4, 4, 4}};
    std::vector<XMFLOAT4X4> palette;
    for (const auto& t : translations)
        palette.push_back(TranslationMatrix(t[0], t[1], t[2]));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    palette.push_back(TranslationMatrix(nan, nan, nan));

    // 130 vertices: more than two 64-thread groups, so the dispatch must cover a partial group.
    // Odd vertices blend all four real bones with 0.1/0.2/0.3/0.4; even vertices blend bones 0-2
    // with 0.2/0.3/0.5 and name the poisoned bone 4 with weight 0.
    constexpr uint32_t kVertexCount = 130;
    std::vector<SkinningSourceVertex> source(kVertexCount);
    std::vector<Vec3> expected(kVertexCount);
    for (uint32_t v = 0; v < kVertexCount; ++v)
    {
        SkinningSourceVertex& vertex = source[v];
        const float x = static_cast<float>(v) * 0.25f;
        vertex.position = {x, -1.0f, 0.5f};
        vertex.normal = {0.0f, 1.0f, 0.0f};
        vertex.texCoord = {static_cast<float>(v) / kVertexCount, 0.75f};
        const bool even = (v % 2) == 0;
        const uint32_t indices[4] = {0, 1, 2, even ? 4u : 3u};
        const float weights[4] = {even ? 0.2f : 0.1f, even ? 0.3f : 0.2f, even ? 0.5f : 0.3f, even ? 0.0f : 0.4f};
        Vec3 blended{x, -1.0f, 0.5f};
        for (uint32_t i = 0; i < 4; ++i)
        {
            vertex.boneIndices[i] = indices[i];
            vertex.boneWeights[i] = weights[i];
            if (weights[i] > 0.0f)
            {
                blended.x += weights[i] * translations[indices[i]][0];
                blended.y += weights[i] * translations[indices[i]][1];
                blended.z += weights[i] * translations[indices[i]][2];
            }
        }
        expected[v] = blended;
    }
    ASSERT_TRUE(GPUSkinning::GetInstance().RegisterMesh(kMeshId, source.data(), kVertexCount,
                                                        static_cast<uint32_t>(palette.size())));

    const std::vector<SkinningOutputVertex> skinned = gpu.Skin(palette);
    ASSERT_EQ(skinned.size(), size_t(kVertexCount));
    EXPECT_TRUE(MaxDeviation(Positions(skinned), expected) < kPoseTolerance);
    // Texture coordinates pass through and translation-only bones leave the unit normal alone.
    EXPECT_NEAR(skinned[129].texCoord.x, 129.0f / kVertexCount, 1e-6f);
    EXPECT_NEAR(skinned[129].texCoord.y, 0.75f, 1e-6f);
    EXPECT_NEAR(skinned[64].normal.y, 1.0f, 1e-5f);
}

#endif // SPARK_PLATFORM_WINDOWS

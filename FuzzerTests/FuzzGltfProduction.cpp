/**
 * @file FuzzGltfProduction.cpp
 * @brief libc++-compiled production adapter for the glTF/GLB libFuzzer harness.
 *
 * The loaders take a path, and external buffer URIs are resolved against the
 * document's directory, so the adapter builds one private directory tree per
 * process:
 *
 *   <tmp>/root/inside.bin    one triangle (three float VEC3 positions)
 *   <tmp>/outside.bin        the same layout outside the root, every vertex set
 *                            to kOutsideSentinel
 *   <tmp>/root/model.gltf    the fuzz input (model.glb when it starts with the
 *                            GLB magic)
 *
 * Every input runs through all three shipped importers on the same file:
 * LoadGLTFStaticMesh, then GLTFFileHasSkin and, for a skinned document,
 * LoadGLTFSkinnedMesh and LoadGLTFAnimationClips. All of them parse through
 * GLTF::ParseDocument with its root-confined file callback. The results are
 * checked against the guarantees their consumers rely on; a violation aborts so
 * libFuzzer records it as a crash rather than a silent pass:
 *  - no accepted vertex carries the out-of-root sentinel, an independent proof
 *    that '../' and rooted URIs never reached <tmp>/outside.bin,
 *  - every index is below the vertex count, and the counts stay within
 *    GLTF::kMaxVertices / kMaxIndices,
 *  - every position, normal, UV and skin weight is finite, and each vertex's
 *    four weights are non-negative and sum to 1,
 *  - skin joint indices name existing bones, the skeleton fits the GPU skinning
 *    palette, and every bone's parent is -1 or an earlier bone,
 *  - clip channels name existing bones, and key times are finite,
 *    non-negative, non-decreasing and within the clip duration, with finite
 *    values; clips are produced only when the skinned mesh itself loads,
 *  - every failed load returns empty output.
 */

#include "FuzzGltfProduction.h"

#include "Graphics/GLTFAnimationLoader.h"
#include "Graphics/GLTFSkinnedMeshLoader.h"
#include "Graphics/GLTFStaticMeshLoader.h"
#include "Graphics/GLTFValidation.h"

#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

namespace
{
    namespace fs = std::filesystem;
    using Spark::Graphics::Detail::GLTFSkinnedMeshData;
    using Spark::Graphics::Detail::GLTFStaticMeshData;

    constexpr std::size_t kMaxInputBytes = 64u * 1024u;
    // Mirror of GPUSkinning.h kMaxBonesPerMesh, the skin palette the loader enforces.
    constexpr std::size_t kMaxBones = 256;
    constexpr float kWeightSumTolerance = 1.0e-3f;

    // Written only into outside.bin. A float triple is never traced by
    // libFuzzer's integer comparison hooks, so mutation cannot learn it from
    // this oracle and inline it into a data URI.
    constexpr std::array<float, 3> kOutsideSentinel = {1234.5f, -4321.25f, 777.125f};
    constexpr std::array<float, 9> kInsideTriangle = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};

    [[noreturn]] void InfrastructureFailure(const char* operation)
    {
        std::fprintf(stderr, "SparkFuzzGltf infrastructure failure during %s: %s\n", operation, std::strerror(errno));
        std::_Exit(70);
    }

    [[noreturn]] void InvariantFailure(const char* loader, const char* what)
    {
        std::fprintf(stderr, "SparkFuzzGltf: %s published a result that violates: %s\n", loader, what);
        std::abort();
    }

    void WriteFile(const fs::path& path, const void* data, std::size_t size)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (size != 0)
            output.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        output.close();
        if (!output)
            InfrastructureFailure("fixture write");
    }

    /// The per-process directory tree; removed at exit.
    class FixtureTree
    {
      public:
        FixtureTree()
        {
            std::string pattern = (fs::temp_directory_path() / "spark-gltf-fuzz-XXXXXX").string();
            if (::mkdtemp(pattern.data()) == nullptr)
                InfrastructureFailure("mkdtemp");
            m_base = pattern;
            m_root = m_base / "root";
            std::error_code ec;
            fs::create_directory(m_root, ec);
            if (ec)
                InfrastructureFailure("root directory");

            WriteFile(m_root / "inside.bin", kInsideTriangle.data(), sizeof(kInsideTriangle));
            std::array<float, 9> outside{};
            for (std::size_t i = 0; i < outside.size(); ++i)
                outside[i] = kOutsideSentinel[i % 3];
            WriteFile(m_base / "outside.bin", outside.data(), sizeof(outside));
        }

        FixtureTree(const FixtureTree&) = delete;
        FixtureTree& operator=(const FixtureTree&) = delete;

        ~FixtureTree()
        {
            std::error_code ignored;
            fs::remove_all(m_base, ignored);
        }

        fs::path Stage(const std::uint8_t* data, std::size_t size) const
        {
            const bool glb = size >= 4 && std::memcmp(data, "glTF", 4) == 0;
            const fs::path path = m_root / (glb ? "model.glb" : "model.gltf");
            std::error_code ignored;
            fs::remove(m_root / (glb ? "model.gltf" : "model.glb"), ignored);
            WriteFile(path, data, size);
            return path;
        }

      private:
        fs::path m_base;
        fs::path m_root;
    };

    bool IsSentinel(const std::array<float, 3>& position)
    {
        return position == kOutsideSentinel;
    }

    template <std::size_t N> bool AllFinite(const std::array<float, N>& values)
    {
        for (const float value : values)
        {
            if (!std::isfinite(value))
                return false;
        }
        return true;
    }

    template <typename Vertex>
    void CheckGeometry(const char* loader, const std::vector<Vertex>& vertices,
                       const std::vector<std::uint32_t>& indices)
    {
        using Spark::Graphics::Detail::GLTF::kMaxIndices;
        using Spark::Graphics::Detail::GLTF::kMaxVertices;
        if (vertices.empty() || indices.empty())
            InvariantFailure(loader, "a successful load has no vertices or indices");
        if (vertices.size() > kMaxVertices || indices.size() > kMaxIndices)
            InvariantFailure(loader, "vertex or index count exceeds the import limits");
        for (const std::uint32_t index : indices)
        {
            if (index >= vertices.size())
                InvariantFailure(loader, "index is not below the vertex count");
        }
        for (const Vertex& vertex : vertices)
        {
            if (!AllFinite(vertex.position) || !AllFinite(vertex.normal) || !AllFinite(vertex.texCoord))
                InvariantFailure(loader, "non-finite position, normal or UV");
            if (IsSentinel(vertex.position))
                InvariantFailure(loader, "a vertex was read from a buffer outside the document root");
        }
    }

    void CheckStatic(bool loaded, const GLTFStaticMeshData& mesh)
    {
        constexpr const char* kLoader = "LoadGLTFStaticMesh";
        if (!loaded)
        {
            if (!mesh.vertices.empty() || !mesh.indices.empty() || !mesh.primitives.empty())
                InvariantFailure(kLoader, "a failed load returns geometry");
            return;
        }
        CheckGeometry(kLoader, mesh.vertices, mesh.indices);
    }

    void CheckSkinned(bool loaded, const GLTFSkinnedMeshData& mesh)
    {
        constexpr const char* kLoader = "LoadGLTFSkinnedMesh";
        const auto& bones = mesh.skeleton.bones;
        if (!loaded)
        {
            if (!mesh.vertices.empty() || !mesh.indices.empty() || !mesh.primitives.empty() || !bones.empty())
                InvariantFailure(kLoader, "a failed load returns geometry or bones");
            return;
        }
        CheckGeometry(kLoader, mesh.vertices, mesh.indices);

        if (bones.empty() || bones.size() > kMaxBones)
            InvariantFailure(kLoader, "skeleton is empty or exceeds the skinning palette");
        for (std::size_t i = 0; i < bones.size(); ++i)
        {
            const std::int32_t parent = bones[i].parentIndex;
            if (parent < -1 || (parent >= 0 && static_cast<std::size_t>(parent) >= i))
                InvariantFailure(kLoader, "bone parent is neither -1 nor an earlier bone");
        }
        for (const auto& vertex : mesh.vertices)
        {
            float sum = 0.0f;
            for (std::size_t k = 0; k < 4; ++k)
            {
                if (vertex.joints[k] >= bones.size())
                    InvariantFailure(kLoader, "skin joint index is not below the bone count");
                if (!std::isfinite(vertex.weights[k]) || vertex.weights[k] < 0.0f)
                    InvariantFailure(kLoader, "skin weight is negative or non-finite");
                sum += vertex.weights[k];
            }
            if (std::fabs(sum - 1.0f) > kWeightSumTolerance)
                InvariantFailure(kLoader, "skin weights do not sum to 1");
        }
    }

    /// VectorKey holds an XMFLOAT3 and QuatKey an XMFLOAT4.
    template <typename Value> bool IsFiniteValue(const Value& value)
    {
        const bool xyz = std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        if constexpr (requires { value.w; })
            return xyz && std::isfinite(value.w);
        else
            return xyz;
    }

    template <typename Key> void CheckKeys(const std::vector<Key>& keys, float duration)
    {
        constexpr const char* kLoader = "LoadGLTFAnimationClips";
        float previous = 0.0f;
        for (const Key& key : keys)
        {
            if (!std::isfinite(key.time) || key.time < previous || key.time > duration)
                InvariantFailure(kLoader, "key time is non-finite, decreasing or past the clip duration");
            previous = key.time;
            if (!IsFiniteValue(key.value))
                InvariantFailure(kLoader, "non-finite key value");
        }
    }

    void CheckClips(bool loaded, const std::vector<Spark::Animation::AnimationClip>& clips, bool skinnedLoaded,
                    std::size_t boneCount)
    {
        constexpr const char* kLoader = "LoadGLTFAnimationClips";
        if (!loaded)
        {
            if (!clips.empty())
                InvariantFailure(kLoader, "a failed load returns clips");
            return;
        }
        if (!skinnedLoaded)
            InvariantFailure(kLoader, "clips were imported for a file whose skinned mesh does not load");
        for (const auto& clip : clips)
        {
            if (!std::isfinite(clip.duration) || clip.duration < 0.0f)
                InvariantFailure(kLoader, "clip duration is negative or non-finite");
            for (const auto& channel : clip.channels)
            {
                if (channel.boneIndex < 0 || static_cast<std::size_t>(channel.boneIndex) >= boneCount)
                    InvariantFailure(kLoader, "channel bone index is not a skeleton bone");
                CheckKeys(channel.positionKeys, clip.duration);
                CheckKeys(channel.rotationKeys, clip.duration);
                CheckKeys(channel.scaleKeys, clip.duration);
            }
        }
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production loaders, cgltf and adapter.
extern "C" int SparkFuzzLoadGltf(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr && size != 0)
        return 0;

    static const FixtureTree tree;
    const fs::path path = tree.Stage(data, size);

    std::string error;
    GLTFStaticMeshData staticMesh;
    CheckStatic(Spark::Graphics::Detail::LoadGLTFStaticMesh(path, staticMesh, error), staticMesh);

    bool hasSkin = false;
    if (!Spark::Graphics::Detail::GLTFFileHasSkin(path, hasSkin, error) || !hasSkin)
        return 0;

    GLTFSkinnedMeshData skinnedMesh;
    const bool skinnedLoaded = Spark::Graphics::Detail::LoadGLTFSkinnedMesh(path, skinnedMesh, error);
    CheckSkinned(skinnedLoaded, skinnedMesh);

    std::vector<Spark::Animation::AnimationClip> clips;
    const bool clipsLoaded = Spark::Graphics::Detail::LoadGLTFAnimationClips(path, clips, error);
    CheckClips(clipsLoaded, clips, skinnedLoaded, skinnedMesh.skeleton.bones.size());
    return 0;
}

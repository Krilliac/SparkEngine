/**
 * @file FuzzFbxProduction.cpp
 * @brief libc++-compiled production adapter for the binary FBX libFuzzer harness.
 *
 * FBXImporter::ImportFromMemory is the importer's whole parse path (Import only
 * reads the file into a buffer first), so the fuzz input goes straight to it with
 * every import option on: normals, UVs, animations, triangulation and UV flip.
 * Arrays are inflated through the instrumented SparkFuzzMiniz, the same miniz the
 * engine links. The result is then checked against the guarantees its consumers
 * rely on; a violation aborts so libFuzzer records it as a crash rather than a
 * silent pass:
 *  - every published mesh has whole xyz triples, a vertexCount that matches
 *    them, per-vertex normal/UV arrays when present, and only indices below
 *    vertexCount,
 *  - the published meshes stay inside the importer's aggregate output budget
 *    (8M vertex floats, 16M indices),
 *  - every bone parent is -1 or names an existing bone,
 *  - a failed import publishes no meshes, bones or animations.
 *
 * The 256 MiB input and 1M-element array limits lie far above the smoke's
 * -max_len, so the smoke pins the element cap with array-count-over-1M.fbx:
 * it claims an 80 MB array, which would exceed -malloc_limit_mb if the count
 * were not rejected before the destination is sized.
 */

#include "FuzzFbxProduction.h"

#include "Graphics/FBXImporter.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    // Mirrors of the importer's documented output budget (FBXImporter.cpp).
    constexpr std::size_t kMaxTotalVertexFloats = 8u * 1024u * 1024u;
    constexpr std::size_t kMaxTotalIndices = 16u * 1024u * 1024u;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzFbx: FBXImporter published a result that violates: %s\n", what);
        std::abort();
    }

    void CheckMesh(const Spark::Graphics::FBXMeshData& mesh)
    {
        if (mesh.vertices.size() % 3 != 0)
            InvariantFailure("mesh vertices are not whole xyz triples");
        const std::size_t vertexCount = mesh.vertices.size() / 3;
        if (mesh.vertexCount != vertexCount)
            InvariantFailure("mesh vertexCount differs from its vertex float stride");
        if (!mesh.normals.empty() && mesh.normals.size() != mesh.vertices.size())
            InvariantFailure("mesh normals are not one xyz triple per vertex");
        if (!mesh.uvs.empty() && mesh.uvs.size() != vertexCount * 2)
            InvariantFailure("mesh uvs are not one uv pair per vertex");
        for (const std::uint32_t index : mesh.indices)
        {
            if (index >= vertexCount)
                InvariantFailure("mesh index is not below the mesh vertex count");
        }
    }

    void CheckImport(const Spark::Graphics::FBXImportResult& result)
    {
        if (!result.success)
        {
            if (!result.meshes.empty() || !result.bones.empty() || !result.animations.empty())
                InvariantFailure("a failed import still publishes meshes, bones or animations");
            return;
        }

        std::size_t totalVertexFloats = 0;
        std::size_t totalIndices = 0;
        for (const Spark::Graphics::FBXMeshData& mesh : result.meshes)
        {
            CheckMesh(mesh);
            totalVertexFloats += mesh.vertices.size();
            totalIndices += mesh.indices.size();
        }
        if (totalVertexFloats > kMaxTotalVertexFloats)
            InvariantFailure("summed vertex floats exceed the 8M aggregate budget");
        if (totalIndices > kMaxTotalIndices)
            InvariantFailure("summed indices exceed the 16M aggregate budget");

        const std::size_t boneCount = result.bones.size();
        for (const Spark::Graphics::FBXBoneData& bone : result.bones)
        {
            if (bone.parentIndex < -1 ||
                (bone.parentIndex >= 0 && static_cast<std::size_t>(bone.parentIndex) >= boneCount))
                InvariantFailure("bone parent is neither -1 nor an existing bone");
        }
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production importer, miniz and logger.
extern "C" int SparkFuzzImportFbx(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr && size != 0)
        return 0;

    Spark::Graphics::FBXImportOptions options;
    options.importNormals = true;
    options.importUVs = true;
    options.importAnimations = true;
    options.triangulate = true;
    options.flipUVs = true;

    // ImportFromMemory rejects a null buffer before its size checks, so an
    // empty unit is handed over as a non-null pointer with size 0.
    static const std::uint8_t kEmpty = 0;
    const std::uint8_t* bytes = data != nullptr ? data : &kEmpty;
    CheckImport(Spark::Graphics::FBXImporter::GetInstance().ImportFromMemory(bytes, size, options));
    return 0;
}

/**
 * @file FuzzObjStaticProduction.cpp
 * @brief libc++-compiled production adapter for the OBJ static-mesh libFuzzer harness.
 *
 * LoadOBJStaticMesh takes a path and resolves `mtllib` names against the OBJ's
 * directory, so the adapter keeps a private tree for the process:
 *
 *     <tmp>/canary.mtl       newmtl SPARK_CANARY   (outside the asset directory)
 *     <tmp>/root/lib.mtl     two in-directory materials
 *     <tmp>/root/model.obj   the fuzz input
 *
 * An input that names a material library is loaded twice, with and without
 * the canary on disk. A loader confined to the asset directory cannot tell the
 * difference, so any change in the result means it read a file outside that
 * directory. The first result is also checked against the invariants the
 * loader promises. A violation aborts so libFuzzer records it as a crash
 * rather than a silent pass.
 */

#include "FuzzObjStaticProduction.h"

#include "Graphics/OBJStaticMeshLoader.h"

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
#include <string_view>

#include <unistd.h>

namespace
{
    using Spark::Graphics::Detail::OBJStaticMeshData;

    constexpr std::size_t kMaxInputBytes = 64u * 1024u;
    constexpr char kCanaryLibrary[] = "newmtl SPARK_CANARY\nKd 1 0 0\n";
    constexpr char kInRootLibrary[] = "newmtl inroot_red\nKd 1 0 0\nnewmtl inroot_green\nKd 0 1 0\n";

    [[noreturn]] void InfrastructureFailure(const char* operation)
    {
        std::fprintf(stderr, "SparkFuzzObjStatic infrastructure failure during %s: %s\n", operation,
                     std::strerror(errno));
        std::_Exit(70);
    }

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzObjStatic: LoadOBJStaticMesh violated: %s\n", what);
        std::abort();
    }

    void WriteFile(const std::filesystem::path& path, const void* data, std::size_t size)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        out.close();
        if (!out)
            InfrastructureFailure("write");
    }

    /// The per-process tree described in the file comment. It lives until exit.
    struct FuzzTree
    {
        std::filesystem::path base;
        std::filesystem::path model;
        std::filesystem::path canary;

        FuzzTree()
        {
            std::array<char, 64> pattern{};
            std::snprintf(pattern.data(), pattern.size(), "/tmp/spark-obj-fuzz-XXXXXX");
            if (::mkdtemp(pattern.data()) == nullptr)
                InfrastructureFailure("mkdtemp");
            base = pattern.data();
            std::error_code ec;
            std::filesystem::create_directory(base / "root", ec);
            if (ec)
                InfrastructureFailure("mkdir");
            model = base / "root" / "model.obj";
            canary = base / "canary.mtl";
            WriteFile(base / "root" / "lib.mtl", kInRootLibrary, sizeof(kInRootLibrary) - 1);
        }

        ~FuzzTree()
        {
            std::error_code ec;
            std::filesystem::remove_all(base, ec);
        }

        void PlaceCanary() const { WriteFile(canary, kCanaryLibrary, sizeof(kCanaryLibrary) - 1); }

        void RemoveCanary() const
        {
            if (::unlink(canary.c_str()) != 0)
                InfrastructureFailure("unlink");
        }
    };

    bool SameResult(bool loadedA, const OBJStaticMeshData& a, bool loadedB, const OBJStaticMeshData& b)
    {
        if (loadedA != loadedB || a.vertices.size() != b.vertices.size() || a.indices != b.indices ||
            a.submeshes.size() != b.submeshes.size())
            return false;
        for (std::size_t i = 0; i < a.submeshes.size(); ++i)
        {
            if (a.submeshes[i].indexStart != b.submeshes[i].indexStart ||
                a.submeshes[i].indexCount != b.submeshes[i].indexCount ||
                a.submeshes[i].materialId != b.submeshes[i].materialId)
                return false;
        }
        return a.vertices.empty() ||
               std::memcmp(a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(a.vertices[0])) == 0;
    }

    bool AllFinite(const float* values, std::size_t count)
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            if (!std::isfinite(values[i]))
                return false;
        }
        return true;
    }

    void CheckLoadedMesh(const OBJStaticMeshData& mesh)
    {
        if (mesh.vertices.empty() || mesh.indices.empty() || mesh.submeshes.empty())
            InvariantFailure("an accepted mesh is empty");
        if (mesh.indices.size() % 3 != 0)
            InvariantFailure("index count is not a whole number of triangles");
        for (const std::uint32_t index : mesh.indices)
        {
            if (index >= mesh.vertices.size())
                InvariantFailure("an index addresses past the vertex array");
        }

        std::uint64_t expectedStart = 0;
        for (const auto& submesh : mesh.submeshes)
        {
            if (submesh.indexStart != expectedStart || submesh.indexCount == 0 || submesh.indexCount % 3 != 0)
                InvariantFailure("submesh ranges are not contiguous whole triangles");
            if (submesh.materialId < -1)
                InvariantFailure("a submesh has a negative material id other than -1");
            expectedStart += submesh.indexCount;
        }
        if (expectedStart != mesh.indices.size())
            InvariantFailure("submesh ranges do not cover the index buffer");

        for (const auto& vertex : mesh.vertices)
        {
            if (!AllFinite(vertex.position.data(), 3) || !AllFinite(vertex.normal.data(), 3) ||
                !AllFinite(vertex.texCoord.data(), 2))
                InvariantFailure("a vertex holds a non-finite value");
            const double length =
                std::sqrt(double(vertex.normal[0]) * vertex.normal[0] + double(vertex.normal[1]) * vertex.normal[1] +
                          double(vertex.normal[2]) * vertex.normal[2]);
            if (std::fabs(length - 1.0) > 1.0e-3)
                InvariantFailure("a normal is not unit length");
        }
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production loader and tinyobjloader.
extern "C" int SparkFuzzLoadObjStatic(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr && size != 0)
        return 0;

    static const FuzzTree tree;
    WriteFile(tree.model, data, size);

    tree.PlaceCanary();
    OBJStaticMeshData withCanary;
    std::string error;
    const bool loadedWithCanary = Spark::Graphics::Detail::LoadOBJStaticMesh(tree.model, withCanary, error);

    // Only an `mtllib` statement makes the loader open another file, so the
    // second load is needed only when the input contains that keyword.
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    if (text.find("mtllib") != std::string_view::npos)
    {
        tree.RemoveCanary();
        OBJStaticMeshData withoutCanary;
        const bool loadedWithoutCanary = Spark::Graphics::Detail::LoadOBJStaticMesh(tree.model, withoutCanary, error);
        if (!SameResult(loadedWithCanary, withCanary, loadedWithoutCanary, withoutCanary))
            InvariantFailure("the result depends on a file outside the OBJ's directory");
    }

    if (!loadedWithCanary)
    {
        if (!withCanary.vertices.empty() || !withCanary.indices.empty() || !withCanary.submeshes.empty())
            InvariantFailure("a rejected file left mesh data behind");
        return 0;
    }
    CheckLoadedMesh(withCanary);
    return 0;
}

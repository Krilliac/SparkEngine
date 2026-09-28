/**
 * @file OBJStaticMeshLoader.cpp
 * @brief Validated CPU-side Wavefront OBJ static-mesh loading via tinyobjloader.
 */

#include "OBJStaticMeshLoader.h"

#include <tiny_obj_loader.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace Spark::Graphics::Detail
{
    namespace
    {
        constexpr size_t kMaxVertices = 16ull * 1024ull * 1024ull;
        constexpr size_t kMaxIndices = kMaxVertices * 3ull;
        /// Ear clipping costs O(n^2) per polygon, so a 64 KiB file holding one
        /// 2400-corner polygon took a second under ASan and a larger file
        /// grows without bound. Larger polygons are fanned instead. The cap
        /// sits above the repository's largest authored n-gon (74 corners).
        constexpr size_t kMaxEarClipCorners = 96;

        bool IsFinite(const std::vector<tinyobj::real_t>& values)
        {
            for (tinyobj::real_t value : values)
            {
                if (!std::isfinite(value))
                {
                    return false;
                }
            }
            return true;
        }

        /// Strict RFC 3629 UTF-8: no overlong forms, surrogates, or code points
        /// above U+10FFFF. MSVC's path(std::u8string) converts with
        /// MB_ERR_INVALID_CHARS and throws std::system_error on anything else,
        /// so an untrusted `mtllib` name is checked before it becomes a path.
        bool IsValidUtf8(const std::string& text)
        {
            size_t i = 0;
            while (i < text.size())
            {
                const auto lead = static_cast<unsigned char>(text[i]);
                if (lead < 0x80)
                {
                    ++i;
                    continue;
                }

                size_t length = 0;
                unsigned char minSecond = 0x80;
                unsigned char maxSecond = 0xBF;
                if (lead >= 0xC2 && lead <= 0xDF)
                {
                    length = 2;
                }
                else if (lead >= 0xE0 && lead <= 0xEF)
                {
                    length = 3;
                    minSecond = lead == 0xE0 ? 0xA0 : 0x80; // overlong
                    maxSecond = lead == 0xED ? 0x9F : 0xBF; // UTF-16 surrogates
                }
                else if (lead >= 0xF0 && lead <= 0xF4)
                {
                    length = 4;
                    minSecond = lead == 0xF0 ? 0x90 : 0x80; // overlong
                    maxSecond = lead == 0xF4 ? 0x8F : 0xBF; // above U+10FFFF
                }
                else
                {
                    return false;
                }
                if (text.size() - i < length)
                {
                    return false;
                }

                const auto second = static_cast<unsigned char>(text[i + 1]);
                if (second < minSecond || second > maxSecond)
                {
                    return false;
                }
                for (size_t k = 2; k < length; ++k)
                {
                    const auto continuation = static_cast<unsigned char>(text[i + k]);
                    if (continuation < 0x80 || continuation > 0xBF)
                    {
                        return false;
                    }
                }
                i += length;
            }
            return true;
        }

        /// Resolves `mtllib` names only inside the OBJ's own directory.
        /// tinyobjloader's MaterialFileReader joins any name onto the search
        /// path, so `mtllib ../canary.mtl` read files outside the asset tree
        /// and `mtllib ../../../../dev/zero` read an unbounded device.
        class ConfinedMaterialReader final : public tinyobj::MaterialReader
        {
          public:
            explicit ConfinedMaterialReader(std::filesystem::path directory) : m_directory(std::move(directory)) {}

            bool operator()(const std::string& name, std::vector<tinyobj::material_t>* materials,
                            std::map<std::string, int>* materialMap, std::string* warn, std::string* err) override
            {
                const std::optional<std::filesystem::path> resolved = Resolve(name);
                if (!resolved)
                {
                    if (warn)
                    {
                        *warn += "material file [ " + name +
                                 " ] refused: not a bounded regular file inside the OBJ directory\n";
                    }
                    return false;
                }
                std::ifstream stream(*resolved);
                if (!stream)
                {
                    return false;
                }
                tinyobj::LoadMtl(materialMap, materials, &stream, warn, err);
                return true;
            }

          private:
            std::optional<std::filesystem::path> Resolve(const std::string& name) const
            {
                // A backslash is a Windows separator or drive form; OBJ files
                // from any platform name libraries with '/'. A name that is not
                // UTF-8 (a Latin-1 name from an old exporter, or hostile bytes)
                // is refused on every platform alike.
                if (name.empty() || name.find('\\') != std::string::npos || !IsValidUtf8(name))
                {
                    return std::nullopt;
                }
                const std::filesystem::path relative(std::u8string(name.begin(), name.end()));
                if (relative.has_root_name() || relative.has_root_directory())
                {
                    return std::nullopt;
                }
                for (const std::filesystem::path& component : relative)
                {
                    if (component == "..")
                    {
                        return std::nullopt;
                    }
                }

                // weakly_canonical resolves symlinks, so a link inside the
                // directory that points out of it is refused here too.
                std::error_code ec;
                const std::filesystem::path resolved = std::filesystem::weakly_canonical(m_directory / relative, ec);
                if (ec)
                {
                    return std::nullopt;
                }
                const auto [directoryEnd, resolvedEnd] =
                    std::mismatch(m_directory.begin(), m_directory.end(), resolved.begin(), resolved.end());
                (void)resolvedEnd;
                if (directoryEnd != m_directory.end() || !std::filesystem::is_regular_file(resolved, ec) || ec)
                {
                    return std::nullopt;
                }
                const std::uintmax_t bytes = std::filesystem::file_size(resolved, ec);
                if (ec || bytes > kMaxOBJFileBytes)
                {
                    return std::nullopt;
                }
                return resolved;
            }

            std::filesystem::path m_directory; ///< Canonical directory that holds the OBJ.
        };

        std::array<float, 3> Position(const tinyobj::attrib_t& attrib, int index)
        {
            const size_t base = static_cast<size_t>(index) * 3;
            return {static_cast<float>(attrib.vertices[base]), static_cast<float>(attrib.vertices[base + 1]),
                    static_cast<float>(attrib.vertices[base + 2])};
        }

        /// Normalizes in double precision: squaring a large but finite float
        /// component (1e20) overflows float to infinity, which turned authored
        /// normals into zero vectors and face normals into NaN. Every finite
        /// float input stays finite here. Returns false for a near-zero vector.
        bool Normalize(double x, double y, double z, std::array<float, 3>& out)
        {
            const double length = std::sqrt(x * x + y * y + z * z);
            if (!(length > 1.0e-12))
            {
                return false;
            }
            out = {static_cast<float>(x / length), static_cast<float>(y / length), static_cast<float>(z / length)};
            return true;
        }

        std::array<float, 3> FaceNormal(const std::array<float, 3>& p0, const std::array<float, 3>& p1,
                                        const std::array<float, 3>& p2)
        {
            const double e1x = double(p1[0]) - p0[0];
            const double e1y = double(p1[1]) - p0[1];
            const double e1z = double(p1[2]) - p0[2];
            const double e2x = double(p2[0]) - p0[0];
            const double e2y = double(p2[1]) - p0[1];
            const double e2z = double(p2[2]) - p0[2];
            std::array<float, 3> normal{0.0f, 1.0f, 0.0f};
            Normalize(e1y * e2z - e1z * e2y, e1z * e2x - e1x * e2z, e1x * e2y - e1y * e2x, normal);
            return normal;
        }

        /// Triangulates one polygon into exactly (n - 2) triangles of local
        /// corner indices, preserving the polygon's winding. tinyobjloader's
        /// built-in ear clipper silently drops triangles from some convex
        /// Blender n-gons, so the loader parses polygons untriangulated and
        /// clips them here. Ear clipping runs in the plane that drops the
        /// dominant axis of the Newell normal; if no ear can be found (for
        /// example a polygon with collinear runs), the remainder is fanned so
        /// no authored area is discarded. Polygons over kMaxEarClipCorners are
        /// fanned outright.
        void TriangulatePolygon(const std::vector<std::array<float, 3>>& points,
                                std::vector<std::array<size_t, 3>>& triangles)
        {
            triangles.clear();
            const size_t count = points.size();
            if (count < 3)
            {
                return;
            }
            if (count == 3)
            {
                triangles.push_back({0, 1, 2});
                return;
            }

            double nx = 0.0, ny = 0.0, nz = 0.0;
            for (size_t i = 0; i < count; ++i)
            {
                const auto& a = points[i];
                const auto& b = points[(i + 1) % count];
                nx += (double(a[1]) - b[1]) * (double(a[2]) + b[2]);
                ny += (double(a[2]) - b[2]) * (double(a[0]) + b[0]);
                nz += (double(a[0]) - b[0]) * (double(a[1]) + b[1]);
            }
            const double ax = std::fabs(nx), ay = std::fabs(ny), az = std::fabs(nz);
            size_t u = 0, v = 1;
            double sign = nz;
            if (ax >= ay && ax >= az)
            {
                u = 1;
                v = 2;
                sign = nx;
            }
            else if (ay >= az)
            {
                u = 2;
                v = 0;
                sign = ny;
            }

            std::vector<size_t> remaining(count);
            for (size_t i = 0; i < count; ++i)
            {
                remaining[i] = i;
            }

            auto cross2 = [&](size_t a, size_t b, size_t c)
            {
                const double abx = double(points[b][u]) - points[a][u];
                const double aby = double(points[b][v]) - points[a][v];
                const double acx = double(points[c][u]) - points[a][u];
                const double acy = double(points[c][v]) - points[a][v];
                return (abx * acy - aby * acx) * (sign >= 0.0 ? 1.0 : -1.0);
            };

            if (sign != 0.0 && count <= kMaxEarClipCorners)
            {
                while (remaining.size() > 3)
                {
                    bool clipped = false;
                    const size_t n = remaining.size();
                    for (size_t i = 0; i < n; ++i)
                    {
                        const size_t prev = remaining[(i + n - 1) % n];
                        const size_t curr = remaining[i];
                        const size_t next = remaining[(i + 1) % n];
                        if (cross2(prev, curr, next) <= 0.0)
                        {
                            continue; // reflex or collinear corner
                        }
                        bool containsOther = false;
                        for (size_t j = 0; j < n && !containsOther; ++j)
                        {
                            const size_t other = remaining[j];
                            if (other == prev || other == curr || other == next)
                            {
                                continue;
                            }
                            containsOther = cross2(prev, curr, other) >= 0.0 && cross2(curr, next, other) >= 0.0 &&
                                            cross2(next, prev, other) >= 0.0;
                        }
                        if (containsOther)
                        {
                            continue;
                        }
                        triangles.push_back({prev, curr, next});
                        remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(i));
                        clipped = true;
                        break;
                    }
                    if (!clipped)
                    {
                        break;
                    }
                }
            }

            for (size_t i = 1; i + 1 < remaining.size(); ++i)
            {
                triangles.push_back({remaining[0], remaining[i], remaining[i + 1]});
            }
        }
    } // namespace

    bool ValidateOBJIndices(const tinyobj::attrib_t& attrib, const std::vector<tinyobj::shape_t>& shapes,
                            std::string& error)
    {
        const size_t positionCount = attrib.vertices.size() / 3;
        const size_t normalCount = attrib.normals.size() / 3;
        const size_t texCoordCount = attrib.texcoords.size() / 2;

        for (const tinyobj::shape_t& shape : shapes)
        {
            for (const tinyobj::index_t& index : shape.mesh.indices)
            {
                if (index.vertex_index < 0 || static_cast<size_t>(index.vertex_index) >= positionCount)
                {
                    error = "OBJ face references a position index out of range";
                    return false;
                }
                if (index.normal_index >= 0 && static_cast<size_t>(index.normal_index) >= normalCount)
                {
                    error = "OBJ face references a normal index out of range";
                    return false;
                }
                if (index.texcoord_index >= 0 && static_cast<size_t>(index.texcoord_index) >= texCoordCount)
                {
                    error = "OBJ face references a texture coordinate index out of range";
                    return false;
                }
            }
        }
        return true;
    }

    bool LoadOBJStaticMesh(const std::filesystem::path& path, OBJStaticMeshData& meshData, std::string& error)
    {
        meshData = {};
        error.clear();

        if (path.empty())
        {
            error = "OBJ path is empty";
            return false;
        }

        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec) || ec)
        {
            error = "OBJ source is not a regular file";
            return false;
        }
        const std::uintmax_t fileBytes = std::filesystem::file_size(path, ec);
        if (ec || fileBytes > kMaxOBJFileBytes)
        {
            error = "OBJ file exceeds the import size limit";
            return false;
        }
        const std::filesystem::path canonicalPath = std::filesystem::canonical(path, ec);
        if (ec)
        {
            error = "OBJ path cannot be resolved";
            return false;
        }

        try
        {
            std::ifstream objStream(canonicalPath);
            if (!objStream)
            {
                error = "OBJ file cannot be opened";
                return false;
            }

            tinyobj::attrib_t attrib;
            std::vector<tinyobj::shape_t> shapes;
            std::vector<tinyobj::material_t> materials;
            std::string warning;
            std::string parseError;
            ConfinedMaterialReader materialReader(canonicalPath.parent_path());
            // Polygons stay untriangulated here; see TriangulatePolygon.
            if (!tinyobj::LoadObj(&attrib, &shapes, &materials, &warning, &parseError, &objStream, &materialReader,
                                  /*triangulate=*/false))
            {
                error = "tinyobj parse failed";
                if (!parseError.empty())
                {
                    error += ": " + parseError;
                }
                return false;
            }

            if (!IsFinite(attrib.vertices) || !IsFinite(attrib.normals) || !IsFinite(attrib.texcoords))
            {
                error = "OBJ contains non-finite vertex data";
                return false;
            }

            const size_t positionCount = attrib.vertices.size() / 3;
            const size_t normalCount = attrib.normals.size() / 3;
            const size_t texCoordCount = attrib.texcoords.size() / 2;

            // Corners with an authored normal are shared by their exact
            // (position, texcoord, normal) index triple; corners without one
            // get their own vertex because their generated normal is per face.
            std::map<std::array<int, 3>, uint32_t> sharedVertices;

            for (const tinyobj::shape_t& shape : shapes)
            {
                const auto& faceVertexCounts = shape.mesh.num_face_vertices;
                const auto& indices = shape.mesh.indices;
                size_t cursor = 0;
                std::vector<std::array<float, 3>> polygon;
                std::vector<std::array<size_t, 3>> triangles;
                for (size_t face = 0; face < faceVertexCounts.size(); ++face)
                {
                    const size_t cornerCount = faceVertexCounts[face];
                    if (cornerCount < 3 || cursor + cornerCount > indices.size())
                    {
                        error = "OBJ face has fewer than three corners or truncated indices";
                        meshData = {};
                        return false;
                    }

                    polygon.resize(cornerCount);
                    for (size_t corner = 0; corner < cornerCount; ++corner)
                    {
                        const int vertexIndex = indices[cursor + corner].vertex_index;
                        if (vertexIndex < 0 || static_cast<size_t>(vertexIndex) >= positionCount)
                        {
                            error = "OBJ face references a position index out of range";
                            meshData = {};
                            return false;
                        }
                        polygon[corner] = Position(attrib, vertexIndex);
                    }
                    TriangulatePolygon(polygon, triangles);

                    const int materialId = face < shape.mesh.material_ids.size() ? shape.mesh.material_ids[face] : -1;
                    if (meshData.submeshes.empty() || meshData.submeshes.back().materialId != materialId)
                    {
                        OBJStaticSubmesh submesh;
                        submesh.indexStart = static_cast<uint32_t>(meshData.indices.size());
                        submesh.materialId = materialId;
                        meshData.submeshes.push_back(submesh);
                    }

                    for (const std::array<size_t, 3>& triangle : triangles)
                    {
                        bool faceNormalReady = false;
                        std::array<float, 3> faceNormal{};
                        for (size_t corner = 0; corner < 3; ++corner)
                        {
                            const tinyobj::index_t& index = indices[cursor + triangle[corner]];
                            const bool hasNormal =
                                index.normal_index >= 0 && static_cast<size_t>(index.normal_index) < normalCount;
                            const bool hasTexCoord =
                                index.texcoord_index >= 0 && static_cast<size_t>(index.texcoord_index) < texCoordCount;
                            if ((index.normal_index >= 0 && !hasNormal) || (index.texcoord_index >= 0 && !hasTexCoord))
                            {
                                error = "OBJ face references a normal or texture coordinate index out of range";
                                meshData = {};
                                return false;
                            }

                            if (hasNormal)
                            {
                                const std::array<int, 3> key = {index.vertex_index, index.texcoord_index,
                                                                index.normal_index};
                                const auto found = sharedVertices.find(key);
                                if (found != sharedVertices.end())
                                {
                                    meshData.indices.push_back(found->second);
                                    continue;
                                }
                            }

                            if (meshData.vertices.size() >= kMaxVertices || meshData.indices.size() >= kMaxIndices)
                            {
                                error = "OBJ mesh exceeds import limits";
                                meshData = {};
                                return false;
                            }

                            OBJStaticVertex vertex;
                            vertex.position = polygon[triangle[corner]];
                            if (hasNormal)
                            {
                                const size_t base = static_cast<size_t>(index.normal_index) * 3;
                                // OBJ does not require unit normals (Kenney's
                                // repair_tool.obj export writes length 0.5).
                                if (!Normalize(attrib.normals[base], attrib.normals[base + 1], attrib.normals[base + 2],
                                               vertex.normal))
                                {
                                    vertex.normal =
                                        FaceNormal(polygon[triangle[0]], polygon[triangle[1]], polygon[triangle[2]]);
                                }
                            }
                            else
                            {
                                if (!faceNormalReady)
                                {
                                    faceNormal =
                                        FaceNormal(polygon[triangle[0]], polygon[triangle[1]], polygon[triangle[2]]);
                                    faceNormalReady = true;
                                }
                                vertex.normal = faceNormal;
                            }
                            if (hasTexCoord)
                            {
                                const size_t base = static_cast<size_t>(index.texcoord_index) * 2;
                                vertex.texCoord = {static_cast<float>(attrib.texcoords[base]),
                                                   1.0f - static_cast<float>(attrib.texcoords[base + 1])};
                            }

                            const uint32_t newIndex = static_cast<uint32_t>(meshData.vertices.size());
                            meshData.vertices.push_back(vertex);
                            meshData.indices.push_back(newIndex);
                            if (hasNormal)
                            {
                                sharedVertices.emplace(
                                    std::array<int, 3>{index.vertex_index, index.texcoord_index, index.normal_index},
                                    newIndex);
                            }
                        }
                    }
                    cursor += cornerCount;
                }
            }
        }
        catch (const std::bad_alloc&)
        {
            meshData = {};
            error = "out of memory while loading OBJ static mesh";
            return false;
        }
        catch (const std::exception& exception)
        {
            // Backstop: tinyobj and the material reader run on untrusted bytes
            // and no caller catches, so any other throw fails the load closed.
            meshData = {};
            error = std::string("OBJ static mesh load failed: ") + exception.what();
            return false;
        }

        if (meshData.vertices.empty() || meshData.indices.empty())
        {
            meshData = {};
            error = "OBJ contains no triangles";
            return false;
        }

        for (size_t i = 0; i < meshData.submeshes.size(); ++i)
        {
            const uint32_t end = i + 1 < meshData.submeshes.size() ? meshData.submeshes[i + 1].indexStart
                                                                   : static_cast<uint32_t>(meshData.indices.size());
            meshData.submeshes[i].indexCount = end - meshData.submeshes[i].indexStart;
        }
        return true;
    }
} // namespace Spark::Graphics::Detail

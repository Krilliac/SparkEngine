/**
 * @file OBJStaticMeshLoader.cpp
 * @brief Validated CPU-side Wavefront OBJ static-mesh loading via tinyobjloader.
 */

#include "OBJStaticMeshLoader.h"

#include <tiny_obj_loader.h>

#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <new>
#include <system_error>

namespace Spark::Graphics::Detail
{
    namespace
    {
        constexpr size_t kMaxVertices = 16ull * 1024ull * 1024ull;
        constexpr size_t kMaxIndices = kMaxVertices * 3ull;

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

        std::array<float, 3> Position(const tinyobj::attrib_t& attrib, int index)
        {
            const size_t base = static_cast<size_t>(index) * 3;
            return {static_cast<float>(attrib.vertices[base]), static_cast<float>(attrib.vertices[base + 1]),
                    static_cast<float>(attrib.vertices[base + 2])};
        }

        std::array<float, 3> FaceNormal(const std::array<float, 3>& p0, const std::array<float, 3>& p1,
                                        const std::array<float, 3>& p2)
        {
            const float e1x = p1[0] - p0[0];
            const float e1y = p1[1] - p0[1];
            const float e1z = p1[2] - p0[2];
            const float e2x = p2[0] - p0[0];
            const float e2y = p2[1] - p0[1];
            const float e2z = p2[2] - p0[2];
            const std::array<float, 3> n = {e1y * e2z - e1z * e2y, e1z * e2x - e1x * e2z, e1x * e2y - e1y * e2x};
            const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (!(length > 1.0e-12f))
            {
                return {0.0f, 1.0f, 0.0f};
            }
            return {n[0] / length, n[1] / length, n[2] / length};
        }

        /// Triangulates one polygon into exactly (n - 2) triangles of local
        /// corner indices, preserving the polygon's winding. tinyobjloader's
        /// built-in ear clipper silently drops triangles from some convex
        /// Blender n-gons, so the loader parses polygons untriangulated and
        /// clips them here. Ear clipping runs in the plane that drops the
        /// dominant axis of the Newell normal; if no ear can be found (for
        /// example a polygon with collinear runs), the remainder is fanned so
        /// no authored area is discarded.
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

            if (sign != 0.0)
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

        try
        {
            const std::u8string utf8Path = path.u8string();
            const std::u8string utf8Dir = path.parent_path().u8string();

            tinyobj::ObjReaderConfig config;
            config.triangulate = false; // see TriangulatePolygon
            config.mtl_search_path = std::string(utf8Dir.begin(), utf8Dir.end());

            tinyobj::ObjReader reader;
            if (!reader.ParseFromFile(std::string(utf8Path.begin(), utf8Path.end()), config))
            {
                error = "tinyobj parse failed";
                if (!reader.Error().empty())
                {
                    error += ": " + reader.Error();
                }
                return false;
            }

            const tinyobj::attrib_t& attrib = reader.GetAttrib();
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

            for (const tinyobj::shape_t& shape : reader.GetShapes())
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
                                const float nx = static_cast<float>(attrib.normals[base]);
                                const float ny = static_cast<float>(attrib.normals[base + 1]);
                                const float nz = static_cast<float>(attrib.normals[base + 2]);
                                // OBJ does not require unit normals (Kenney's
                                // repair_tool.obj export writes length 0.5).
                                const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
                                vertex.normal =
                                    length > 1.0e-12f
                                        ? std::array<float, 3>{nx / length, ny / length, nz / length}
                                        : FaceNormal(polygon[triangle[0]], polygon[triangle[1]], polygon[triangle[2]]);
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

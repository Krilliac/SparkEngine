/**
 * @file MeshWindowsPrimitives.cpp
 * @brief Procedural primitive generation for the Windows/D3D11 Mesh implementation
 *
 * CreateCube / CreateTriangle / CreatePlane / CreateSphere / CreatePyramid split
 * out of MeshWindows.cpp (which keeps lifecycle, OBJ loading, normal calculation,
 * buffer creation, and rendering). The Linux counterpart lives in MeshLinux.cpp.
 */
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS

#include "Mesh.h"
#include "Utils/Assert.h"
#include "../Utils/Validate.h"
#include <cmath>
#include <vector>

using namespace DirectX;

HRESULT Mesh::CreateCube(float size)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, size > 0.0f, "Cube size must be positive");

    // Clear existing data
    m_vertices.clear();
    m_indices.clear();
    m_submeshes.clear();

    float h = size * 0.5f;

    // 24 vertices (4 per face, 6 faces)
    m_vertices = {
        // Front face (z = +h)
        {{-h, -h, h}, {0, 0, 1}, {0, 1}}, // 0
        {{h, -h, h}, {0, 0, 1}, {1, 1}},  // 1
        {{h, h, h}, {0, 0, 1}, {1, 0}},   // 2
        {{-h, h, h}, {0, 0, 1}, {0, 0}},  // 3

        // Back face (z = -h)
        {{h, -h, -h}, {0, 0, -1}, {0, 1}},  // 4
        {{-h, -h, -h}, {0, 0, -1}, {1, 1}}, // 5
        {{-h, h, -h}, {0, 0, -1}, {1, 0}},  // 6
        {{h, h, -h}, {0, 0, -1}, {0, 0}},   // 7

        // Left face (x = -h)
        {{-h, -h, -h}, {-1, 0, 0}, {0, 1}}, // 8
        {{-h, -h, h}, {-1, 0, 0}, {1, 1}},  // 9
        {{-h, h, h}, {-1, 0, 0}, {1, 0}},   // 10
        {{-h, h, -h}, {-1, 0, 0}, {0, 0}},  // 11

        // Right face (x = +h)
        {{h, -h, h}, {1, 0, 0}, {0, 1}},  // 12
        {{h, -h, -h}, {1, 0, 0}, {1, 1}}, // 13
        {{h, h, -h}, {1, 0, 0}, {1, 0}},  // 14
        {{h, h, h}, {1, 0, 0}, {0, 0}},   // 15

        // Bottom face (y = -h)
        {{-h, -h, -h}, {0, -1, 0}, {0, 1}}, // 16
        {{h, -h, -h}, {0, -1, 0}, {1, 1}},  // 17
        {{h, -h, h}, {0, -1, 0}, {1, 0}},   // 18
        {{-h, -h, h}, {0, -1, 0}, {0, 0}},  // 19

        // Top face (y = +h)
        {{-h, h, h}, {0, 1, 0}, {0, 1}}, // 20
        {{h, h, h}, {0, 1, 0}, {1, 1}},  // 21
        {{h, h, -h}, {0, 1, 0}, {1, 0}}, // 22
        {{-h, h, -h}, {0, 1, 0}, {0, 0}} // 23
    };

    // 36 indices (2 triangles per face, 6 faces)
    m_indices = {// Front face
                 0, 1, 2, 0, 2, 3,
                 // Back face
                 4, 5, 6, 4, 6, 7,
                 // Left face
                 8, 9, 10, 8, 10, 11,
                 // Right face
                 12, 13, 14, 12, 14, 15,
                 // Bottom face
                 16, 17, 18, 16, 18, 19,
                 // Top face
                 20, 21, 22, 20, 22, 23};

    m_vertexCount = static_cast<UINT>(m_vertices.size());
    m_indexCount = static_cast<UINT>(m_indices.size());

    ASSERT_ALWAYS_MSG(m_vertexCount > 0 && m_indexCount > 0, "CreateCube produced empty mesh");

    return CreateBuffers();
}

HRESULT Mesh::CreateTriangle(float size)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, size > 0.0f, "Triangle size must be positive");

    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;

    float h = size * 0.5f;

    // Create a simple triangle
    vertices.emplace_back(XMFLOAT3(0.0f, h, 0.0f),    // Top vertex
                          XMFLOAT3(0.0f, 0.0f, 1.0f), // Normal pointing forward
                          XMFLOAT2(0.5f, 0.0f)        // Texture coordinate
    );

    vertices.emplace_back(XMFLOAT3(-h, -h, 0.0f),     // Bottom left
                          XMFLOAT3(0.0f, 0.0f, 1.0f), // Normal pointing forward
                          XMFLOAT2(0.0f, 1.0f)        // Texture coordinate
    );

    vertices.emplace_back(XMFLOAT3(h, -h, 0.0f),      // Bottom right
                          XMFLOAT3(0.0f, 0.0f, 1.0f), // Normal pointing forward
                          XMFLOAT2(1.0f, 1.0f)        // Texture coordinate
    );

    // Triangle indices
    indices = {0, 1, 2};

    ASSERT_ALWAYS_MSG(!vertices.empty() && !indices.empty(), "CreateTriangle produced empty mesh");

    return CreateFromVertices(vertices, indices);
}

HRESULT Mesh::CreatePlane(float width, float depth)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, width > 0.0f && depth > 0.0f, "Plane dimensions must be positive");

    MeshData md;
    float hw = width * 0.5f, hd = depth * 0.5f;
    XMFLOAT3 pts[4] = {{-hw, 0, -hd}, {+hw, 0, -hd}, {+hw, 0, +hd}, {-hw, 0, +hd}};
    // Proper 0..1 UVs per corner — previously all four corners were (0,0),
    // which collapsed any texture to a single texel (untextured-looking plane).
    XMFLOAT2 uvs[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    XMFLOAT3 n{0, 1, 0};
    unsigned int idxs[6] = {0, 1, 2, 0, 2, 3};

    for (int i = 0; i < 6; ++i)
    {
        md.vertices.emplace_back(pts[idxs[i]], n, uvs[idxs[i]]);
        md.indices.push_back(static_cast<unsigned int>(md.indices.size()));
    }

    ASSERT_ALWAYS_MSG(!md.vertices.empty() && !md.indices.empty(), "CreatePlane produced empty mesh");

    return CreateFromVertices(md.vertices, md.indices);
}

HRESULT Mesh::CreateSphere(float radius, int slices, int stacks)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, radius > 0.0f, "Sphere radius must be positive");
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, slices >= 3 && stacks >= 2,
                      "Sphere needs at least 3 slices and 2 stacks");

    MeshData md;
    for (int i = 0; i <= stacks; ++i)
    {
        float v = i / static_cast<float>(stacks);
        float phi = v * XM_PI;
        for (int j = 0; j <= slices; ++j)
        {
            float u = j / static_cast<float>(slices);
            float theta = u * XM_2PI;

            XMFLOAT3 pos{radius * sinf(phi) * cosf(theta), radius * cosf(phi), radius * sinf(phi) * sinf(theta)};
            XMVECTOR nVec = XMVector3Normalize(XMLoadFloat3(&pos));
            XMFLOAT3 n;
            XMStoreFloat3(&n, nVec);

            md.vertices.emplace_back(pos, n, XMFLOAT2(u, v));
        }
    }

    for (int i = 0; i < stacks; ++i)
    {
        for (int j = 0; j < slices; ++j)
        {
            int a = i * (slices + 1) + j;
            int b = a + slices + 1;
            md.indices.insert(md.indices.end(), {static_cast<unsigned int>(a), static_cast<unsigned int>(b),
                                                 static_cast<unsigned int>(a + 1), static_cast<unsigned int>(b),
                                                 static_cast<unsigned int>(b + 1), static_cast<unsigned int>(a + 1)});
        }
    }

    ASSERT_ALWAYS_MSG(!md.vertices.empty() && !md.indices.empty(), "CreateSphere produced empty mesh");

    return CreateFromVertices(md.vertices, md.indices);
}

HRESULT Mesh::CreatePyramid(float size, float height)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, size > 0.0f && height > 0.0f,
                      "Pyramid size and height must be positive");

    // Clear existing data
    m_vertices.clear();
    m_indices.clear();

    float h = size * 0.5f;

    // 5 vertices: 4 for the base + 1 for the apex
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;

    // Base vertices (square base in XZ plane at y=0)
    vertices.emplace_back(XMFLOAT3(-h, 0.0f, -h), XMFLOAT3(0.0f, -1.0f, 0.0f), XMFLOAT2(0.0f, 1.0f)); // 0: bottom-left
    vertices.emplace_back(XMFLOAT3(h, 0.0f, -h), XMFLOAT3(0.0f, -1.0f, 0.0f), XMFLOAT2(1.0f, 1.0f));  // 1: bottom-right
    vertices.emplace_back(XMFLOAT3(h, 0.0f, h), XMFLOAT3(0.0f, -1.0f, 0.0f), XMFLOAT2(1.0f, 0.0f));   // 2: top-right
    vertices.emplace_back(XMFLOAT3(-h, 0.0f, h), XMFLOAT3(0.0f, -1.0f, 0.0f), XMFLOAT2(0.0f, 0.0f));  // 3: top-left

    // Apex vertex (at the top)
    vertices.emplace_back(XMFLOAT3(0.0f, height, 0.0f), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(0.5f, 0.5f)); // 4: apex

    // Base (bottom face) - 2 triangles
    indices.insert(indices.end(), {0, 2, 1}); // Triangle 1
    indices.insert(indices.end(), {0, 3, 2}); // Triangle 2

    // Side faces - 4 triangles
    // Front face (negative Z)
    indices.insert(indices.end(), {0, 1, 4});
    // Right face (positive X)
    indices.insert(indices.end(), {1, 2, 4});
    // Back face (positive Z)
    indices.insert(indices.end(), {2, 3, 4});
    // Left face (negative X)
    indices.insert(indices.end(), {3, 0, 4});

    // Compute proper normals for the side faces
    // For each triangle, calculate face normal and assign to all vertices of that triangle
    for (size_t i = 6; i < indices.size(); i += 3) // Skip base triangles (indices 0-5)
    {
        unsigned int i0 = indices[i];
        unsigned int i1 = indices[i + 1];
        unsigned int i2 = indices[i + 2];

        XMVECTOR v0 = XMLoadFloat3(&vertices[i0].Position);
        XMVECTOR v1 = XMLoadFloat3(&vertices[i1].Position);
        XMVECTOR v2 = XMLoadFloat3(&vertices[i2].Position);

        XMVECTOR edge1 = v1 - v0;
        XMVECTOR edge2 = v2 - v0;
        XMVECTOR normal = XMVector3Normalize(XMVector3Cross(edge1, edge2));

        XMFLOAT3 n;
        XMStoreFloat3(&n, normal);

        // Assign the same normal to all vertices of this triangle
        vertices[i0].Normal = n;
        vertices[i1].Normal = n;
        vertices[i2].Normal = n;
    }

    ASSERT_ALWAYS_MSG(!vertices.empty() && !indices.empty(), "CreatePyramid produced empty mesh");

    return CreateFromVertices(vertices, indices);
}

#endif // SPARK_PLATFORM_WINDOWS

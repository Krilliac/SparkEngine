// Mesh.cpp
#include "Mesh.h"
#include "Utils/Assert.h"
//#include "Utils/Debug.h"
#include <tiny_obj_loader.h>
#include <DirectXMath.h>
#include <fstream>
#include <filesystem>   // C++17 for path handling
#include <cmath>
#include <iostream>

using namespace DirectX;

Mesh::Mesh() {
    std::wcout << L"[INFO] Mesh constructed." << std::endl;
}
Mesh::~Mesh() {
    std::wcout << L"[INFO] Mesh destructor called." << std::endl;
    Shutdown();
}

HRESULT Mesh::Initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    std::wcout << L"[OPERATION] Mesh::Initialize called." << std::endl;
    ASSERT(device && context);
    m_device = device;
    m_context = context;
    std::wcout << L"[INFO] Mesh initialized with device/context." << std::endl;
    return S_OK;
}

void Mesh::Shutdown() {
    std::wcout << L"[OPERATION] Mesh::Shutdown called." << std::endl;
    if (m_ib) { m_ib->Release(); m_ib = nullptr; }
    if (m_vb) { m_vb->Release(); m_vb = nullptr; }
    m_vertices.clear();
    m_indices.clear();
    m_vertexCount = m_indexCount = 0;
    m_device = nullptr;
    m_context = nullptr;
    std::wcout << L"[INFO] Mesh shutdown complete." << std::endl;
}

bool Mesh::LoadFromFile(const std::wstring& path) {
    std::wcout << L"[OPERATION] Mesh::LoadFromFile called. path=" << path << std::endl;
    ASSERT_ALWAYS_MSG(!path.empty(), "Mesh::LoadFromFile – empty path");

    // Convert wide path to UTF-8 for tinyobjloader
    auto u8path_u8 = std::filesystem::path(path).u8string();                                    // basic_string<char8_t>
    std::string u8Path(u8path_u8.begin(), u8path_u8.end());                                     // convert to std::string

    // Debug: report attempt
    std::wcerr << L"[DEBUG] Attempting to load mesh from: " << path << std::endl;

    // Reader config
    tinyobj::ObjReader reader;
    tinyobj::ObjReaderConfig cfg;
    {
        auto parent_u8 = std::filesystem::path(path).parent_path().u8string();
        std::string mtlSearch(parent_u8.begin(), parent_u8.end());
        cfg.mtl_search_path = mtlSearch;                                                         // now a std::string
    }

    // Replace the fallback section in LoadFromFile with this:
    if (!reader.ParseFromFile(u8Path, cfg))
    {
        // Report tinyobj error
        if (!reader.Error().empty())
        {
            std::string err = "tinyobj error: " + reader.Error() + "\n";
            OutputDebugStringA(err.c_str());
            std::cerr << err;
        }

        // Do NOT create placeholder here - let the calling code handle it
        std::wcerr << L"[DEBUG] Failed to load mesh from file." << std::endl;
        return false; // Return false so placeholder creation happens in calling code
    }

    // Report tinyobj warnings
    if (!reader.Warning().empty())
    {
        std::string warn = "tinyobj warning: " + reader.Warning() + "\n";
        OutputDebugStringA(warn.c_str());
        std::cerr << warn;
    }

    // Parse geometry
    const auto& attrib = reader.GetAttrib();
    const auto& shapes = reader.GetShapes();

    std::vector<Vertex> verts;
    std::vector<unsigned int> inds;
    verts.reserve(attrib.vertices.size() / 3);

    for (const auto& shape : shapes)
    {
        for (const auto& idx : shape.mesh.indices)
        {
            Vertex v{};
            // Position
            v.Position = {
                attrib.vertices[3 * idx.vertex_index + 0],
                attrib.vertices[3 * idx.vertex_index + 1],
                attrib.vertices[3 * idx.vertex_index + 2]
            };
            // Normal
            if (idx.normal_index >= 0)
            {
                v.Normal = {
                    attrib.normals[3 * idx.normal_index + 0],
                    attrib.normals[3 * idx.normal_index + 1],
                    attrib.normals[3 * idx.normal_index + 2]
                };
            }
            else
            {
                v.Normal = { 0, 1, 0 };
            }
            // TexCoord
            if (idx.texcoord_index >= 0)
            {
                v.TexCoord = {
                    attrib.texcoords[2 * idx.texcoord_index + 0],
                    1.0f - attrib.texcoords[2 * idx.texcoord_index + 1]
                };
            }
            else
            {
                v.TexCoord = { 0, 0 };
            }

            verts.push_back(v);
            inds.push_back(static_cast<unsigned int>(inds.size()));
        }
    }

    // Ensure we got geometry
    ASSERT_ALWAYS_MSG(!verts.empty() && !inds.empty(), "tinyobj produced empty mesh data");

    m_vertices = std::move(verts);
    m_indices = std::move(inds);
    m_vertexCount = static_cast<UINT>(m_vertices.size());
    m_indexCount = static_cast<UINT>(m_indices.size());

    // Recompute normals if any were zero
    bool anyZero = std::any_of(
        m_vertices.begin(), m_vertices.end(),
        [](const Vertex& v) {
            return v.Normal.x == 0 && v.Normal.y == 0 && v.Normal.z == 0;
        });
    if (anyZero)
        CalculateNormals();

    // Create GPU buffers
    HRESULT hr = CreateBuffers();
    if (FAILED(hr))
    {
        std::wcerr << L"[ERROR] CreateBuffers failed in LoadFromFile!" << std::endl;
        return false;
    }

    std::wcout << L"[INFO] Mesh loaded from file: " << path << std::endl;
    return true;
}

HRESULT Mesh::CreateFromVertices(const std::vector<Vertex>& verts,
    const std::vector<unsigned int>& inds)
{
    std::wcout << L"[OPERATION] Mesh::CreateFromVertices called. verts=" << verts.size() << L" inds=" << inds.size() << std::endl;
    ASSERT(!verts.empty() && !inds.empty());

    m_vertices = verts;
    m_indices = inds;
    m_vertexCount = static_cast<UINT>(verts.size());
    m_indexCount = static_cast<UINT>(inds.size());

    CalculateNormals();
    return CreateBuffers();
}

HRESULT Mesh::CreateCube(float size)
{
    std::wcout << L"[OPERATION] Mesh::CreateCube called. size=" << size << std::endl;
    ASSERT(size > 0.0f);

    // Clear existing data
    m_vertices.clear();
    m_indices.clear();

    float h = size * 0.5f;

    // 24 vertices (4 per face, 6 faces)
    m_vertices = {
        // Front face (z = +h)
        {{-h, -h,  h}, {0, 0, 1}, {0, 1}},  // 0
        {{ h, -h,  h}, {0, 0, 1}, {1, 1}},  // 1
        {{ h,  h,  h}, {0, 0, 1}, {1, 0}},  // 2
        {{-h,  h,  h}, {0, 0, 1}, {0, 0}},  // 3

        // Back face (z = -h)  
        {{ h, -h, -h}, {0, 0, -1}, {0, 1}}, // 4
        {{-h, -h, -h}, {0, 0, -1}, {1, 1}}, // 5
        {{-h,  h, -h}, {0, 0, -1}, {1, 0}}, // 6
        {{ h,  h, -h}, {0, 0, -1}, {0, 0}}, // 7

        // Left face (x = -h)
        {{-h, -h, -h}, {-1, 0, 0}, {0, 1}}, // 8
        {{-h, -h,  h}, {-1, 0, 0}, {1, 1}}, // 9
        {{-h,  h,  h}, {-1, 0, 0}, {1, 0}}, // 10
        {{-h,  h, -h}, {-1, 0, 0}, {0, 0}}, // 11

        // Right face (x = +h)
        {{ h, -h,  h}, {1, 0, 0}, {0, 1}},  // 12
        {{ h, -h, -h}, {1, 0, 0}, {1, 1}},  // 13
        {{ h,  h, -h}, {1, 0, 0}, {1, 0}},  // 14
        {{ h,  h,  h}, {1, 0, 0}, {0, 0}},  // 15

        // Bottom face (y = -h)
        {{-h, -h, -h}, {0, -1, 0}, {0, 1}}, // 16
        {{ h, -h, -h}, {0, -1, 0}, {1, 1}}, // 17
        {{ h, -h,  h}, {0, -1, 0}, {1, 0}}, // 18
        {{-h, -h,  h}, {0, -1, 0}, {0, 0}}, // 19

        // Top face (y = +h)
        {{-h,  h,  h}, {0, 1, 0}, {0, 1}},  // 20
        {{ h,  h,  h}, {0, 1, 0}, {1, 1}},  // 21
        {{ h,  h, -h}, {0, 1, 0}, {1, 0}},  // 22
        {{-h,  h, -h}, {0, 1, 0}, {0, 0}}   // 23
    };

    // 36 indices (2 triangles per face, 6 faces)
    m_indices = {
        // Front face
        0, 1, 2,  0, 2, 3,
        // Back face
        4, 5, 6,  4, 6, 7,
        // Left face  
        8, 9, 10,  8, 10, 11,
        // Right face
        12, 13, 14,  12, 14, 15,
        // Bottom face
        16, 17, 18,  16, 18, 19,
        // Top face
        20, 21, 22,  20, 22, 23
    };

    m_vertexCount = static_cast<UINT>(m_vertices.size());
    m_indexCount = static_cast<UINT>(m_indices.size());

    std::wcout << L"[INFO] Cube mesh created. Vertices=" << m_vertexCount << L" Indices=" << m_indexCount << std::endl;

    ASSERT_ALWAYS_MSG(m_vertexCount > 0 && m_indexCount > 0, "CreateCube produced empty mesh");

    // Create GPU buffers
    HRESULT hr = CreateBuffers();
    if (FAILED(hr)) {
        std::wcerr << L"[ERROR] CreateBuffers failed in CreateCube!" << std::endl;
        return hr;
    }

    std::wcout << L"[DEBUG] CreateCube completed successfully" << std::endl;
    return S_OK;
}

HRESULT Mesh::CreateTriangle(float size)
{
    ASSERT(size > 0.0f);

    std::wcout << L"[OPERATION] Mesh::CreateTriangle called. size=" << size << std::endl;

    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;

    float h = size * 0.5f;

    // Create a simple triangle
    vertices.emplace_back(
        XMFLOAT3(0.0f, h, 0.0f),      // Top vertex
        XMFLOAT3(0.0f, 0.0f, 1.0f),   // Normal pointing forward
        XMFLOAT2(0.5f, 0.0f)          // Texture coordinate
    );

    vertices.emplace_back(
        XMFLOAT3(-h, -h, 0.0f),       // Bottom left
        XMFLOAT3(0.0f, 0.0f, 1.0f),   // Normal pointing forward
        XMFLOAT2(0.0f, 1.0f)          // Texture coordinate
    );

    vertices.emplace_back(
        XMFLOAT3(h, -h, 0.0f),        // Bottom right
        XMFLOAT3(0.0f, 0.0f, 1.0f),   // Normal pointing forward
        XMFLOAT2(1.0f, 1.0f)          // Texture coordinate
    );

    // Triangle indices
    indices = { 0, 1, 2 };

    std::wcout << L"[INFO] Triangle mesh created." << std::endl;

    ASSERT_ALWAYS_MSG(!vertices.empty() && !indices.empty(), "CreateTriangle produced empty mesh");

    return CreateFromVertices(vertices, indices);
}

HRESULT Mesh::CreatePlane(float width, float depth)
{
    ASSERT(width > 0.0f && depth > 0.0f);

    std::wcout << L"[OPERATION] Mesh::CreatePlane called. width=" << width << L" depth=" << depth << std::endl;

    MeshData md;
    float hw = width * 0.5f, hd = depth * 0.5f;
    XMFLOAT3 pts[4] = { {-hw,0,-hd},{+hw,0,-hd},{+hw,0,+hd},{-hw,0,+hd} };
    XMFLOAT3 n{ 0,1,0 };
    unsigned int idxs[6] = { 0,1,2, 0,2,3 };

    for (int i = 0; i < 6; ++i)
    {
        md.vertices.emplace_back(pts[idxs[i]], n, XMFLOAT2(0, 0));
        md.indices.push_back(static_cast<unsigned int>(md.indices.size()));
    }

    ASSERT_ALWAYS_MSG(!md.vertices.empty() && !md.indices.empty(),
        "CreatePlane produced empty mesh");

    std::wcout << L"[INFO] Plane mesh created." << std::endl;

    return CreateFromVertices(md.vertices, md.indices);
}

HRESULT Mesh::CreateSphere(float radius, int slices, int stacks)
{
    ASSERT(radius > 0.0f);
    ASSERT(slices >= 3 && stacks >= 2);

    std::wcout << L"[OPERATION] Mesh::CreateSphere called. radius=" << radius << L" slices=" << slices << L" stacks=" << stacks << std::endl;

    MeshData md;
    for (int i = 0; i <= stacks; ++i)
    {
        float v = i / static_cast<float>(stacks);
        float phi = v * XM_PI;
        for (int j = 0; j <= slices; ++j)
        {
            float u = j / static_cast<float>(slices);
            float theta = u * XM_2PI;

            XMFLOAT3 pos{
                radius * sinf(phi) * cosf(theta),
                radius * cosf(phi),
                radius * sinf(phi) * sinf(theta)
            };
            XMVECTOR nVec = XMVector3Normalize(XMLoadFloat3(&pos));
            XMFLOAT3 n; XMStoreFloat3(&n, nVec);

            md.vertices.emplace_back(pos, n, XMFLOAT2(u, v));
        }
    }

    for (int i = 0; i < stacks; ++i)
    {
        for (int j = 0; j < slices; ++j)
        {
            int a = i * (slices + 1) + j;
            int b = a + slices + 1;
            md.indices.insert(md.indices.end(), {
                static_cast<unsigned int>(a),
                static_cast<unsigned int>(b),
                static_cast<unsigned int>(a + 1),
                static_cast<unsigned int>(b),
                static_cast<unsigned int>(b + 1),
                static_cast<unsigned int>(a + 1)
                });
        }
    }

    ASSERT_ALWAYS_MSG(!md.vertices.empty() && !md.indices.empty(),
        "CreateSphere produced empty mesh");

    std::wcout << L"[INFO] Sphere mesh created." << std::endl;

    return CreateFromVertices(md.vertices, md.indices);
}

HRESULT Mesh::CreatePyramid(float size, float height)
{
    ASSERT(size > 0.0f && height > 0.0f);

    std::wcout << L"[OPERATION] Mesh::CreatePyramid called. size=" << size << L" height=" << height << std::endl;

    // Clear existing data
    m_vertices.clear();
    m_indices.clear();

    float h = size * 0.5f;

    // 5 vertices: 4 for the base + 1 for the apex
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;

    // Base vertices (square base in XZ plane at y=0)
    vertices.emplace_back(XMFLOAT3(-h, 0.0f, -h), XMFLOAT3(0.0f, -1.0f, 0.0f), XMFLOAT2(0.0f, 1.0f)); // 0: bottom-left
    vertices.emplace_back(XMFLOAT3( h, 0.0f, -h), XMFLOAT3(0.0f, -1.0f, 0.0f), XMFLOAT2(1.0f, 1.0f)); // 1: bottom-right
    vertices.emplace_back(XMFLOAT3( h, 0.0f,  h), XMFLOAT3(0.0f, -1.0f, 0.0f), XMFLOAT2(1.0f, 0.0f)); // 2: top-right
    vertices.emplace_back(XMFLOAT3(-h, 0.0f,  h), XMFLOAT3(0.0f, -1.0f, 0.0f), XMFLOAT2(0.0f, 0.0f)); // 3: top-left

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

    std::wcout << L"[INFO] Pyramid base mesh created with " << vertices.size() << L" vertices and " << indices.size() << L" indices." << std::endl;

    // Now compute proper normals for the side faces
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

    std::wcout << L"[INFO] Pyramid mesh created successfully." << std::endl;

    return CreateFromVertices(vertices, indices);
}

void Mesh::CalculateNormals() {
    // **FIXED: Removed excessive logging**
    ASSERT(!m_vertices.empty() && !m_indices.empty());

    for (size_t i = 0; i + 2 < m_indices.size(); i += 3)
    {
        unsigned i0 = m_indices[i], i1 = m_indices[i + 1], i2 = m_indices[i + 2];
        XMVECTOR v0 = XMLoadFloat3(&m_vertices[i0].Position);
        XMVECTOR v1 = XMLoadFloat3(&m_vertices[i1].Position);
        XMVECTOR v2 = XMLoadFloat3(&m_vertices[i2].Position);

        XMVECTOR n = XMVector3Normalize(XMVector3Cross(v1 - v0, v2 - v0));
        XMFLOAT3 nf; XMStoreFloat3(&nf, n);

        m_vertices[i0].Normal = nf;
        m_vertices[i1].Normal = nf;
        m_vertices[i2].Normal = nf;
    }

    // **ONLY log normal calculations occasionally for debugging**
    static int normalCalcCount = 0;
    if (++normalCalcCount % 5 == 0) { // Every 5th normal calculation
        std::wcout << L"[DEBUG] Calculated normals " << normalCalcCount << L" times" << std::endl;
    }
}

HRESULT Mesh::CreateBuffers() {
    // **FIXED: Removed excessive logging**
    ASSERT(m_device);
    ASSERT(!m_vertices.empty() && !m_indices.empty());

    // Release existing buffers before creating new ones to prevent COM leaks
    if (m_vb) { m_vb->Release(); m_vb = nullptr; }
    if (m_ib) { m_ib->Release(); m_ib = nullptr; }

    // Vertex buffer
    D3D11_BUFFER_DESC vbd{};
    vbd.Usage = D3D11_USAGE_DEFAULT;
    vbd.ByteWidth = static_cast<UINT>(m_vertices.size() * sizeof(Vertex));
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vsd{ m_vertices.data(), 0, 0 };

    HRESULT hr = m_device->CreateBuffer(&vbd, &vsd, &m_vb);
    ASSERT_MSG(SUCCEEDED(hr), "CreateBuffer (VB) failed");
    if (FAILED(hr)) return hr;

    // Index buffer
    D3D11_BUFFER_DESC ibd{};
    ibd.Usage = D3D11_USAGE_DEFAULT;
    ibd.ByteWidth = static_cast<UINT>(m_indices.size() * sizeof(unsigned int));
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    ibd.CPUAccessFlags = 0;
    D3D11_SUBRESOURCE_DATA isd{};
    isd.pSysMem = m_indices.data();

    hr = m_device->CreateBuffer(&ibd, &isd, &m_ib);
    ASSERT_MSG(SUCCEEDED(hr), "CreateBuffer (IB) failed");
    if (FAILED(hr)) return hr;

    // **ONLY log buffer creation occasionally for debugging**
    static int bufferCreateCount = 0;
    if (++bufferCreateCount % 10 == 0) { // Every 10th buffer creation
        std::wcout << L"[DEBUG] Created " << bufferCreateCount << L" mesh buffers" << std::endl;
    }
    
    return S_OK;
}

void Mesh::Render(ID3D11DeviceContext* ctx) {
    // **FIXED: Removed per-frame logging that was causing severe performance issues**
    ASSERT(ctx && m_vb && m_ib && m_indexCount > 0);

    UINT stride = sizeof(Vertex), offset = 0;
    ctx->IASetVertexBuffers(0, 1, &m_vb, &stride, &offset);
    ctx->IASetIndexBuffer(m_ib, DXGI_FORMAT_R32_UINT, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    
    // **ENSURE PROPER RENDERING STATE**
    ctx->DrawIndexed(m_indexCount, 0, 0);

    // **ONLY log rendering statistics occasionally for debugging**
    static int renderCallCount = 0;
    if (++renderCallCount % 3600 == 0) { // Every 60 seconds at 60fps
        std::wcout << L"[DEBUG] Mesh rendered " << renderCallCount << L" times. IndexCount=" << m_indexCount << std::endl;
    }
}
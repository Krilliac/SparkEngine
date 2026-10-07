#include "Game/PlaneObject.h"
#include "Graphics/MeshTypes.h"
#include "Core/Platform.h"
#include "Utils/Assert.h"
#include "Utils/Validate.h"
#include <string>
#include <iostream>
#include "Utils/LogMacros.h"

PlaneObject::PlaneObject(float width, float depth) : m_width(width), m_depth(depth)
{
    SPARK_LOG_INFO(Spark::LogCategory::Game, "PlaneObject constructed (width=%.1f, depth=%.1f)", width, depth);
    std::wcout << L"[INFO] PlaneObject constructed. width=" << width << L" depth=" << depth << std::endl;
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, width > 0.f && depth > 0.f, "Plane dimensions must be positive");
    SetName("Plane_" + std::to_string(GetID()));
}

HRESULT PlaneObject::Initialize(ID3D11Device* d, ID3D11DeviceContext* c)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Game);
    std::wcout << L"[OPERATION] PlaneObject::Initialize called. width=" << m_width << L" depth=" << m_depth
               << std::endl;
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, d);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, c);
    return GameObject::Initialize(d, c);
}

void PlaneObject::CreateMesh()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Game);
    std::wcout << L"[OPERATION] PlaneObject::CreateMesh called. width=" << m_width << L" depth=" << m_depth
               << std::endl;
    if (!m_mesh)
    {
        m_mesh = std::make_unique<Mesh>();
        std::wcout << L"[INFO] Mesh created for PlaneObject." << std::endl;
    }
    HRESULT hr = m_mesh->Initialize(m_device, m_context);
    std::wcout << L"[INFO] Mesh initialized for PlaneObject. HR=0x" << std::hex << hr << std::dec << std::endl;
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, SUCCEEDED(hr), "Mesh initialization failed");
    bool loaded = false;
    if (!m_modelPath.empty())
    {
        loaded = m_mesh->LoadFromFile(m_modelPath);
        std::wcout << L"[INFO] Mesh loaded from file for PlaneObject. loaded=" << loaded << std::endl;
    }
    if (!loaded)
    {
        // Ground faces +Y. Keep the generic Mesh plane's opposite winding for
        // the sprite callers that rotate it into the screen-facing plane.
        const float halfWidth = m_width * 0.5f;
        const float halfDepth = m_depth * 0.5f;
        const XMFLOAT3 positions[] = {{-halfWidth, 0, -halfDepth},
                                      {halfWidth, 0, -halfDepth},
                                      {halfWidth, 0, halfDepth},
                                      {-halfWidth, 0, halfDepth}};
        const DirectX::XMFLOAT2 uvs[] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
        const unsigned int corners[] = {0, 2, 1, 0, 3, 2};
        MeshData plane;
        for (const unsigned int corner : corners)
        {
            plane.vertices.emplace_back(positions[corner], XMFLOAT3{0, 1, 0}, uvs[corner]);
            plane.indices.push_back(static_cast<unsigned int>(plane.indices.size()));
        }
        hr = m_mesh->CreateFromVertices(plane.vertices, plane.indices);
        if (FAILED(hr))
        {
            hr = m_mesh->CreateCube(m_width);
        }
        if (FAILED(hr))
        {
            hr = m_mesh->CreateTriangle(m_width);
        }
        std::wcout << L"[INFO] Procedural plane mesh created. HR=0x" << std::hex << hr << std::dec << std::endl;
        SPARK_REQUIRE_MSG(Spark::LogCategory::Game, SUCCEEDED(hr), "Failed to create procedural plane mesh");
    }
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, m_mesh && m_mesh->GetVertexCount() > 0 && m_mesh->GetIndexCount() > 0,
                      "Plane mesh must have vertices and indices after loading/creation");
}

#include "Core/Platform.h"
#include "RampObject.h"
#include <string>
#include <iostream>

using namespace std;

RampObject::RampObject(float length, float height) : m_length(length), m_height(height)
{
    std::wcout << L"[INFO] RampObject constructed. length=" << length << L" height=" << height << std::endl;
    ASSERT_MSG(length > 0.f && height > 0.f, "Ramp dimensions must be positive");
    SetName("Ramp_" + std::to_string(GetID()));
}

HRESULT RampObject::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    std::wcout << L"[OPERATION] RampObject::Initialize called. length=" << m_length << L" height=" << m_height
               << std::endl;
    ASSERT(device != nullptr && context != nullptr);
    return GameObject::Initialize(device, context);
}

void RampObject::CreateMesh()
{
    std::wcout << L"[OPERATION] RampObject::CreateMesh called. length=" << m_length << L" height=" << m_height
               << std::endl;
    if (!m_mesh)
    {
        m_mesh = std::make_unique<Mesh>();
        std::wcout << L"[INFO] Mesh created for RampObject." << std::endl;
    }
    HRESULT hr = m_mesh->Initialize(m_device, m_context);
    std::wcout << L"[INFO] Mesh initialized for RampObject. HR=0x" << std::hex << hr << std::dec << std::endl;
    ASSERT_MSG(SUCCEEDED(hr), "Mesh initialization failed");
    bool loaded = false;
    if (!m_modelPath.empty())
    {
        loaded = m_mesh->LoadFromFile(m_modelPath);
        std::wcout << L"[INFO] Mesh loaded from file for RampObject. loaded=" << loaded << std::endl;
    }
    if (!loaded)
    {
        hr = m_mesh->CreateCube(m_length);
        if (FAILED(hr))
        {
            hr = m_mesh->CreateTriangle(m_length);
        }
        if (FAILED(hr))
        {
            hr = m_mesh->CreatePlane(m_length, m_height);
        }
        std::wcout << L"[INFO] Procedural ramp mesh created. HR=0x" << std::hex << hr << std::dec << std::endl;
        ASSERT_MSG(SUCCEEDED(hr), "Failed to create procedural ramp mesh");
    }
    ASSERT_MSG(m_mesh && m_mesh->GetVertexCount() > 0 && m_mesh->GetIndexCount() > 0,
               "Ramp mesh must have vertices and indices after loading/creation");
}

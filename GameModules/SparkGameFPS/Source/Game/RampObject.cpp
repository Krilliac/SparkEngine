#include "Core/Platform.h"
#include "RampObject.h"
#include "Utils/Validate.h"
#include <string>
#include <iostream>
#include "Utils/LogMacros.h"

using namespace std;

RampObject::RampObject(float length, float height) : m_length(length), m_height(height)
{
    SPARK_LOG_INFO(Spark::LogCategory::Game, "RampObject constructed (length=%.1f, height=%.1f)", length, height);
    std::wcout << L"[INFO] RampObject constructed. length=" << length << L" height=" << height << std::endl;
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, length > 0.f && height > 0.f, "Ramp dimensions must be positive");
    SetName("Ramp_" + std::to_string(GetID()));
}

HRESULT RampObject::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Game);
    std::wcout << L"[OPERATION] RampObject::Initialize called. length=" << m_length << L" height=" << m_height
               << std::endl;
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, device);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, context);
    return GameObject::Initialize(device, context);
}

void RampObject::CreateMesh()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Game);
    std::wcout << L"[OPERATION] RampObject::CreateMesh called. length=" << m_length << L" height=" << m_height
               << std::endl;
    if (!m_mesh)
    {
        m_mesh = std::make_unique<Mesh>();
        std::wcout << L"[INFO] Mesh created for RampObject." << std::endl;
    }
    HRESULT hr = m_mesh->Initialize(m_device, m_context);
    std::wcout << L"[INFO] Mesh initialized for RampObject. HR=0x" << std::hex << hr << std::dec << std::endl;
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, SUCCEEDED(hr), "Mesh initialization failed");
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
        SPARK_REQUIRE_MSG(Spark::LogCategory::Game, SUCCEEDED(hr), "Failed to create procedural ramp mesh");
    }
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, m_mesh && m_mesh->GetVertexCount() > 0 && m_mesh->GetIndexCount() > 0,
                      "Ramp mesh must have vertices and indices after loading/creation");
}

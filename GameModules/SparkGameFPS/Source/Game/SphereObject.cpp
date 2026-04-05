#include "SphereObject.h"
#include "Core/Platform.h"
// SphereObject.cpp
#include "Utils/Assert.h"
#include "Utils/Validate.h"
#include <iostream>
#include "Utils/LogMacros.h"

SphereObject::SphereObject(float radius, int slices, int stacks) : m_radius(radius), m_slices(slices), m_stacks(stacks)
{
    SPARK_LOG_INFO(Spark::LogCategory::Game, "SphereObject constructed (radius=%.1f)", radius);
    std::wcout << L"[INFO] SphereObject constructed. radius=" << radius << L" slices=" << slices << L" stacks="
               << stacks << std::endl;
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, radius > 0.0f, "Sphere radius must be positive");
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, slices >= 3, "Sphere slices must be >= 3");
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, stacks >= 2, "Sphere stacks must be >= 2");
    SetName("Sphere_" + std::to_string(GetID()));
}

HRESULT SphereObject::Initialize(ID3D11Device* d, ID3D11DeviceContext* c)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Game);
    std::wcout << L"[OPERATION] SphereObject::Initialize called. radius=" << m_radius << L" slices=" << m_slices
               << L" stacks=" << m_stacks << std::endl;
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, d);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, c);
    return GameObject::Initialize(d, c);
}

void SphereObject::CreateMesh()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Game);
    std::wcout << L"[OPERATION] SphereObject::CreateMesh called. radius=" << m_radius << L" slices=" << m_slices
               << L" stacks=" << m_stacks << std::endl;
    if (!m_mesh)
    {
        m_mesh = std::make_unique<Mesh>();
        std::wcout << L"[INFO] Mesh created for SphereObject." << std::endl;
    }
    HRESULT hr = m_mesh->Initialize(m_device, m_context);
    std::wcout << L"[INFO] Mesh initialized for SphereObject. HR=0x" << std::hex << hr << std::dec << std::endl;
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, SUCCEEDED(hr), "Mesh initialization failed");
    bool loaded = false;
    if (!m_modelPath.empty())
    {
        loaded = m_mesh->LoadFromFile(m_modelPath);
        std::wcout << L"[INFO] Mesh loaded from file for SphereObject. loaded=" << loaded << std::endl;
    }
    if (!loaded)
    {
        hr = m_mesh->CreateSphere(m_radius, m_slices, m_stacks);
        if (FAILED(hr))
        {
            hr = m_mesh->CreateCube(m_radius);
        }
        if (FAILED(hr))
        {
            hr = m_mesh->CreatePlane(m_radius, m_radius);
        }
        std::wcout << L"[INFO] Procedural sphere mesh created. HR=0x" << std::hex << hr << std::dec << std::endl;
        SPARK_REQUIRE_MSG(Spark::LogCategory::Game, SUCCEEDED(hr), "Failed to create procedural sphere mesh");
    }
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, m_mesh && m_mesh->GetVertexCount() > 0 && m_mesh->GetIndexCount() > 0,
                      "Sphere mesh must have vertices and indices after loading/creation");
}

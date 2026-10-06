#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include "GameObject.h"
#include "../Utils/MathUtils.h"
#include "../Utils/Assert.h"
#include "../Utils/Validate.h"
#include "../Graphics/GraphicsEngine.h"
#include "../Graphics/ProjectAssetPath.h"
#include "../Core/EngineContext.h"
#include <filesystem>
#include <iostream>

using namespace DirectX;

// Definition and initialization of the static member
std::atomic<UINT> GameObject::s_nextID{1};

GameObject::GameObject()
    : m_position{0, 0, 0}, m_rotation{0, 0, 0}, m_scale{1, 1, 1}, m_worldMatrix(XMMatrixIdentity()),
      m_worldMatrixDirty(true), m_active(true), m_visible(true), m_id(s_nextID++),
      m_name("GameObject_" + std::to_string(m_id))
{
}

GameObject::~GameObject()
{
    Shutdown();
}

HRESULT GameObject::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Game);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, device);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, context);
    SPARK_LOG_INFO(Spark::LogCategory::Game, "GameObject::Initialize called. ID=%u Name=%s", m_id, m_name.c_str());
    m_device = device;
    m_context = context;
    m_mesh = std::make_unique<Mesh>();
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, m_mesh);
    HRESULT hr = m_mesh->Initialize(device, context);
    SPARK_LOG_INFO(Spark::LogCategory::Game, "Mesh initialized for GameObject ID=%u HR=0x%08lX", m_id,
                   static_cast<long>(hr));
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, SUCCEEDED(hr), "Mesh::Initialize failed");
    if (FAILED(hr))
    {
        std::wcerr << L"[ERROR] Mesh::Initialize failed for GameObject ID=" << m_id << L" Name=" << m_name.c_str()
                   << std::endl;
        return hr;
    }
    CreateMesh();
    SPARK_LOG_INFO(Spark::LogCategory::Game, "GameObject::Initialize complete. ID=%u Name=%s", m_id, m_name.c_str());
    return S_OK;
}

void GameObject::Shutdown()
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Game);
    SPARK_LOG_INFO(Spark::LogCategory::Game, "GameObject::Shutdown called. ID=%u Name=%s", m_id, m_name.c_str());
    if (m_mesh)
        m_mesh->Shutdown();
    m_mesh.reset();
    m_device = nullptr;
    m_context = nullptr;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "GameObject shutdown complete. ID=%u Name=%s", m_id, m_name.c_str());
}

void GameObject::Update(float dt)
{
    // **FIXED: Removed per-frame logging that was causing performance issues**
    if (m_worldMatrixDirty)
        UpdateWorldMatrix();
}

void GameObject::Render(const XMMATRIX& view, const XMMATRIX& projection)
{
    // **FIXED: Removed per-frame logging that was causing severe performance issues**
    if (!m_visible || !m_mesh)
    {
        return; // No logging for performance - this happens frequently
    }

    if (m_worldMatrixDirty)
        UpdateWorldMatrix();

    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, m_mesh);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, m_device);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, m_context);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, m_mesh->GetVertexCount() > 0 && m_mesh->GetIndexCount() > 0,
                      "Mesh has no vertices or indices to render");

    // Get graphics engine via EngineContext
    GraphicsEngine* graphics = EngineContext::Get() ? EngineContext::Get()->GetGraphics() : nullptr;

    PrepareBasicMaterialRender(graphics, m_worldMatrix, view, projection);

    m_mesh->Render(m_context);
}

void GameObject::PrepareBasicMaterialRender(GraphicsEngine* graphics, const XMMATRIX& world, const XMMATRIX& view,
                                            const XMMATRIX& projection)
{
    if (!graphics)
        return;

    // A fresh basic batch resets t0/t1/t2 to the white/flat/rough defaults,
    // so an invalid or absent authored material cannot inherit a prior
    // object's texture. The project root is supplied by trusted game code,
    // never by scene or model content.
    graphics->SetBasicShaders();
    const GraphicsEngine::BasicMaterial* material = nullptr;
    const bool supportedMaterialPath =
        (m_materialPath.starts_with("Assets/Materials/") || m_materialPath.starts_with("Assets\\Materials\\")) &&
        m_materialPath.ends_with(".json");
    if (supportedMaterialPath && !m_materialProjectRoot.empty())
        material = graphics->GetOrLoadBasicMaterial(m_materialPath, m_materialProjectRoot);

    if (material)
    {
        graphics->UpdateBasicConstants(world, view, projection, XMFLOAT4(1, 1, 1, 1), material->tiling);
        graphics->SetBasicTexture(material->srv.Get());
        graphics->SetBasicMaterialTextures(material->normalSrv.Get(), material->roughnessSrv.Get());
    }
    else
    {
        graphics->UpdateBasicConstants(world, view, projection);
    }
}

bool GameObject::SetMaterialProjectRoot(std::string_view projectRootUtf8)
{
    m_materialProjectRoot.clear();
    try
    {
        const auto suppliedRoot = std::filesystem::u8path(projectRootUtf8.begin(), projectRootUtf8.end());
        if (!suppliedRoot.is_absolute())
            return false;
    }
    catch (const std::filesystem::filesystem_error&)
    {
        return false;
    }

    const auto root = Spark::CanonicalizeFilesystemPath(projectRootUtf8);
    if (!root)
        return false;

    // Preserve the native spelling for filesystem operations; cacheKey folds
    // case on Windows and is suitable for identity only, not an OS path.
    const std::u8string nativeUtf8 = root->nativePath.generic_u8string();
    const std::string nativeRoot(reinterpret_cast<const char*>(nativeUtf8.data()), nativeUtf8.size());
    const auto assets = Spark::ResolveProjectAssetPath(nativeRoot, "Assets");
    std::error_code error;
    if (!assets || !std::filesystem::is_directory(assets->nativePath, error) || error)
        return false;

    m_materialProjectRoot = nativeRoot;
    return true;
}

void GameObject::SetPosition(const XMFLOAT3& pos)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z),
                      "Invalid position vector");
    m_position = pos;
    m_worldMatrixDirty = true;
}

void GameObject::SetRotation(const XMFLOAT3& rot)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, std::isfinite(rot.x) && std::isfinite(rot.y) && std::isfinite(rot.z),
                      "Invalid rotation vector");
    m_rotation = rot;
    m_worldMatrixDirty = true;
}

void GameObject::SetScale(const XMFLOAT3& scl)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, scl.x > 0 && scl.y > 0 && scl.z > 0, "Scale must be positive");
    m_scale = scl;
    m_worldMatrixDirty = true;
}

void GameObject::Translate(const XMFLOAT3& d)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, std::isfinite(d.x) && std::isfinite(d.y) && std::isfinite(d.z),
                      "Invalid translation delta");
    m_position.x += d.x;
    m_position.y += d.y;
    m_position.z += d.z;
    m_worldMatrixDirty = true;
}

void GameObject::Rotate(const XMFLOAT3& d)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, std::isfinite(d.x) && std::isfinite(d.y) && std::isfinite(d.z),
                      "Invalid rotation delta");
    m_rotation.x += d.x;
    m_rotation.y += d.y;
    m_rotation.z += d.z;
    m_worldMatrixDirty = true;
}

void GameObject::Scale(const XMFLOAT3& d)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, d.x > 0 && d.y > 0 && d.z > 0, "Scale factors must be positive");
    m_scale.x *= d.x;
    m_scale.y *= d.y;
    m_scale.z *= d.z;
    m_worldMatrixDirty = true;
}

XMMATRIX GameObject::GetWorldMatrix() const
{
    if (m_worldMatrixDirty)
        UpdateWorldMatrix();
    return m_worldMatrix;
}

XMFLOAT3 GameObject::GetForward() const
{
    XMMATRIX rot = XMMatrixRotationRollPitchYaw(m_rotation.x, m_rotation.y, m_rotation.z);
    XMVECTOR fwd = XMVector3TransformCoord(XMVectorSet(0, 0, 1, 0), rot);
    XMFLOAT3 out;
    XMStoreFloat3(&out, fwd);
    return out;
}

XMFLOAT3 GameObject::GetRight() const
{
    XMMATRIX rot = XMMatrixRotationRollPitchYaw(m_rotation.x, m_rotation.y, m_rotation.z);
    XMVECTOR rt = XMVector3TransformCoord(XMVectorSet(1, 0, 0, 0), rot);
    XMFLOAT3 out;
    XMStoreFloat3(&out, rt);
    return out;
}

XMFLOAT3 GameObject::GetUp() const
{
    XMMATRIX rot = XMMatrixRotationRollPitchYaw(m_rotation.x, m_rotation.y, m_rotation.z);
    XMVECTOR up = XMVector3TransformCoord(XMVectorSet(0, 1, 0, 0), rot);
    XMFLOAT3 out;
    XMStoreFloat3(&out, up);
    return out;
}

float GameObject::GetDistanceFrom(const GameObject& other) const
{
    return GetDistanceFrom(other.GetPosition());
}

float GameObject::GetDistanceFrom(const XMFLOAT3& pos) const
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z),
                      "Invalid distance calculation position");
    float dx = m_position.x - pos.x;
    float dy = m_position.y - pos.y;
    float dz = m_position.z - pos.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

void GameObject::CreateMesh()
{
    // **FIXED: Reduced excessive logging**
    if (!m_mesh)
    {
        m_mesh = std::make_unique<Mesh>();
    }
    HRESULT hr = m_mesh->Initialize(m_device, m_context);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, SUCCEEDED(hr), "Mesh initialization failed");
    bool loaded = false;
    if (!m_modelPath.empty())
    {
        loaded = m_mesh->LoadFromFile(std::wstring(m_modelPath.begin(), m_modelPath.end()));
    }
    if (!loaded)
    {
        hr = m_mesh->CreateCube(1.0f);
        if (FAILED(hr))
        {
            hr = m_mesh->CreateTriangle(1.0f);
        }
        if (FAILED(hr))
        {
            hr = m_mesh->CreatePlane(2.0f, 2.0f);
        }
        SPARK_REQUIRE_MSG(Spark::LogCategory::Game, SUCCEEDED(hr), "Failed to create procedural mesh");
    }
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, m_mesh && m_mesh->GetVertexCount() > 0 && m_mesh->GetIndexCount() > 0,
                      "Mesh must have vertices and indices after loading/creation");
    m_worldMatrixDirty = true;
    m_name = "GameObject_" + std::to_string(m_id);

    // **ONLY log mesh creation occasionally for debugging**
    static int meshCreateCount = 0;
    if (++meshCreateCount % 10 == 0)
    { // Every 10th mesh creation
    }

    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, !m_name.empty(), "GameObject name unexpected empty");
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, m_device);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, m_context);
}

void GameObject::UpdateWorldMatrix() const
{
    XMMATRIX S = XMMatrixScaling(m_scale.x, m_scale.y, m_scale.z);
    XMMATRIX R = XMMatrixRotationRollPitchYaw(m_rotation.x, m_rotation.y, m_rotation.z);
    XMMATRIX T = XMMatrixTranslation(m_position.x, m_position.y, m_position.z);
    m_worldMatrix = S * R * T;
    m_worldMatrixDirty = false;
}

#pragma once
#include "../Core/Platform.h"

#include "GameObject.h"
#include "PlaceholderMesh.h"
#include "../Projectiles/WeaponStats.h"
#include "Primitives.h"
#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>

using DirectX::XMFLOAT3;
using DirectX::XMMATRIX;

class PlaneObject : public GameObject
{
public:
    PlaneObject(float width = 10.0f, float depth = 10.0f);
    ~PlaneObject() override = default;

    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context) override;
    void     Update(float dt) override { GameObject::Update(dt); }
    void     Render(const XMMATRIX& v, const XMMATRIX& p) override
    {
        GameObject::Render(v, p);
    }

    // No special hit behaviour for basic plane
    void OnHit(GameObject*) override {}
    void OnHitWorld(const XMFLOAT3&, const XMFLOAT3&) override {}

protected:
    void CreateMesh() override;

private:
    float        m_width;
    float        m_depth;
    std::wstring m_modelPath{ L"Assets/Models/Plane.obj" };
};
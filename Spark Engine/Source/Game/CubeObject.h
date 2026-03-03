#pragma once
#include "../Core/Platform.h"

#include "GameObject.h"
#include "PlaceholderMesh.h"
#include "Primitives.h"
#include "Utils/Assert.h"        // custom assert system
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>

using DirectX::XMFLOAT3;
using DirectX::XMMATRIX;

class CubeObject : public GameObject
{
public:
    explicit CubeObject(float size = 1.0f);
    ~CubeObject() override = default;

    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context) override;
    void    Update(float dt) override { GameObject::Update(dt); }
    void    Render(const XMMATRIX& v, const XMMATRIX& p) override
    {
        GameObject::Render(v, p);
    }

    void OnHit(GameObject*) override {}
    void OnHitWorld(const XMFLOAT3&, const XMFLOAT3&) override {}

protected:
    void CreateMesh() override;

private:
    float         m_size;
    std::wstring  m_modelPath = L"Assets/Models/Cube.obj";
};
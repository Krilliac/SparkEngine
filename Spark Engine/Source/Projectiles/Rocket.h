// Rocket.h
#pragma once

#include "Projectile.h"
#include "Utils/Assert.h"
#include <DirectXMath.h>

using DirectX::XMFLOAT3;
using DirectX::XMMATRIX;

class Rocket : public Projectile
{
public:
    Rocket();
    ~Rocket() override = default;

    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context) override;
    void    Update(float deltaTime) override;
    void    Render(const XMMATRIX& view, const XMMATRIX& projection) override;
    void    Fire(const XMFLOAT3& startPosition, const XMFLOAT3& direction, float speed) override;

    void OnHit(GameObject* target) override;
    void OnHitWorld(const XMFLOAT3& hitPoint, const XMFLOAT3& normal) override;

private:
    void Explode(const XMFLOAT3& position);

    float m_explosionRadius;
    bool  m_hasExploded;
};
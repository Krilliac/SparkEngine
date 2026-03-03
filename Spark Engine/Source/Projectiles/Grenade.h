// Grenade.h
#pragma once
#include "../Core/Platform.h"

#include "Projectile.h"
#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS

using DirectX::XMMATRIX;
using DirectX::XMFLOAT3;

class Grenade : public Projectile
{
public:
    Grenade();
    ~Grenade() override = default;

    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context) override;
    void    Update(float deltaTime) override;
    void    Render(const XMMATRIX& view, const XMMATRIX& projection) override;
    void    Fire(const XMFLOAT3& startPosition, const XMFLOAT3& direction, float speed) override;

private:
    void Explode();

    float m_fuseTime;
    float m_explosionRadius;
    bool  m_hasExploded;
};
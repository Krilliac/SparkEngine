/**
 * @file MeshTypes.cpp
 * @brief Preserve engine assertion behavior for the public native Vertex constructor.
 */

#include <Spark/MeshTypes.h>
#include "Utils/Assert.h"

#include <cmath>

Vertex::Vertex(const XMFLOAT3& p, const XMFLOAT3& n, const XMFLOAT2& t) : Position(p), Normal(n), TexCoord(t)
{
    ASSERT(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
    ASSERT(std::isfinite(n.x) && std::isfinite(n.y) && std::isfinite(n.z));
    ASSERT(std::isfinite(t.x) && std::isfinite(t.y));
}

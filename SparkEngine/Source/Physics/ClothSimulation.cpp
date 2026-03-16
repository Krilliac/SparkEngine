/**
 * @file ClothSimulation.cpp
 * @brief Implementation of the position-based cloth and soft body simulation
 */

#include "ClothSimulation.h"
#include "../Utils/Validate.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace Spark::Physics
{

    const std::vector<ClothParticle> ClothSimulation::s_emptyParticles;

    ClothSimulation::ClothSimulation() = default;

    uint32_t ClothSimulation::CreateCloth(const ClothDescriptor& desc)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Physics);
        SPARK_WARN_IF(Spark::LogCategory::Physics, desc.width <= 0 || desc.height <= 0,
                      "ClothDescriptor has non-positive dimensions");
        SPARK_WARN_IF(Spark::LogCategory::Physics, desc.mass <= 0.0f, "ClothDescriptor has non-positive mass");
        if (desc.width <= 0 || desc.height <= 0)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Physics, "ClothDescriptor has zero-size dimensions (%d x %d)",
                            desc.width, desc.height);
            return 0;
        }
        uint32_t id = m_nextId++;
        ClothInstance cloth;
        cloth.id = id;
        cloth.width = desc.width;
        cloth.height = desc.height;
        cloth.damping = desc.damping;
        cloth.solverIterations = desc.solverIterations;

        float particleMass = desc.mass / static_cast<float>(desc.width * desc.height);

        // Create particles in a grid
        cloth.particles.resize(static_cast<size_t>(desc.width * desc.height));
        for (int y = 0; y < desc.height; ++y)
        {
            for (int x = 0; x < desc.width; ++x)
            {
                size_t idx = static_cast<size_t>(y * desc.width + x);
                auto& p = cloth.particles[idx];
                p.position.x = desc.origin.x + static_cast<float>(x) * desc.spacing;
                p.position.y = desc.origin.y;
                p.position.z = desc.origin.z + static_cast<float>(y) * desc.spacing;
                p.prevPosition = p.position;
                p.inverseMass = (particleMass > 0.0f) ? (1.0f / particleMass) : 0.0f;
            }
        }

        // Create structural constraints (horizontal + vertical)
        for (int y = 0; y < desc.height; ++y)
        {
            for (int x = 0; x < desc.width; ++x)
            {
                uint32_t idx = static_cast<uint32_t>(y * desc.width + x);

                // Horizontal
                if (x < desc.width - 1)
                {
                    ClothConstraint c;
                    c.particleA = idx;
                    c.particleB = idx + 1;
                    c.restLength = desc.spacing;
                    c.stiffness = desc.stiffness;
                    cloth.constraints.push_back(c);
                }

                // Vertical
                if (y < desc.height - 1)
                {
                    ClothConstraint c;
                    c.particleA = idx;
                    c.particleB = idx + static_cast<uint32_t>(desc.width);
                    c.restLength = desc.spacing;
                    c.stiffness = desc.stiffness;
                    cloth.constraints.push_back(c);
                }

                // Shear diagonal
                if (x < desc.width - 1 && y < desc.height - 1)
                {
                    ClothConstraint c;
                    c.particleA = idx;
                    c.particleB = idx + static_cast<uint32_t>(desc.width) + 1;
                    c.restLength = desc.spacing * 1.414f;
                    c.stiffness = desc.stiffness * 0.8f;
                    cloth.constraints.push_back(c);
                }

                // Bending (skip one)
                if (x < desc.width - 2)
                {
                    ClothConstraint c;
                    c.particleA = idx;
                    c.particleB = idx + 2;
                    c.restLength = desc.spacing * 2.0f;
                    c.stiffness = desc.bendStiffness;
                    cloth.constraints.push_back(c);
                }
                if (y < desc.height - 2)
                {
                    ClothConstraint c;
                    c.particleA = idx;
                    c.particleB = idx + static_cast<uint32_t>(desc.width * 2);
                    c.restLength = desc.spacing * 2.0f;
                    c.stiffness = desc.bendStiffness;
                    cloth.constraints.push_back(c);
                }
            }
        }

        m_clothInstances[id] = std::move(cloth);
        return id;
    }

    void ClothSimulation::DestroyCloth(uint32_t clothId)
    {
        m_clothInstances.erase(clothId);
    }

    void ClothSimulation::PinParticle(uint32_t clothId, uint32_t particleIndex, const DirectX::XMFLOAT3& position)
    {
        auto it = m_clothInstances.find(clothId);
        if (it == m_clothInstances.end() || particleIndex >= it->second.particles.size())
        {
            return;
        }
        auto& p = it->second.particles[particleIndex];
        p.pinned = true;
        p.inverseMass = 0.0f;
        p.position = position;
        p.prevPosition = position;
    }

    void ClothSimulation::UnpinParticle(uint32_t clothId, uint32_t particleIndex)
    {
        auto it = m_clothInstances.find(clothId);
        if (it == m_clothInstances.end() || particleIndex >= it->second.particles.size())
        {
            return;
        }
        auto& p = it->second.particles[particleIndex];
        p.pinned = false;
        p.inverseMass = 1.0f;
    }

    void ClothSimulation::SetWind(uint32_t clothId, const DirectX::XMFLOAT3& wind)
    {
        auto it = m_clothInstances.find(clothId);
        if (it != m_clothInstances.end())
        {
            it->second.wind = wind;
        }
    }

    void ClothSimulation::AddCollider(uint32_t clothId, const ClothCollider& collider)
    {
        auto it = m_clothInstances.find(clothId);
        if (it != m_clothInstances.end())
        {
            it->second.colliders.push_back(collider);
        }
    }

    void ClothSimulation::Update(float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Physics);
        SPARK_WARN_IF(Spark::LogCategory::Physics, deltaTime < 0.0f,
                      "ClothSimulation::Update called with negative deltaTime");
        for (auto& [id, cloth] : m_clothInstances)
        {
            if (cloth.enabled)
            {
                SimulateCloth(cloth, deltaTime);
            }
        }
    }

    const std::vector<ClothParticle>& ClothSimulation::GetParticles(uint32_t clothId) const
    {
        auto it = m_clothInstances.find(clothId);
        if (it != m_clothInstances.end())
        {
            return it->second.particles;
        }
        return s_emptyParticles;
    }

    std::pair<int, int> ClothSimulation::GetClothDimensions(uint32_t clothId) const
    {
        auto it = m_clothInstances.find(clothId);
        if (it != m_clothInstances.end())
        {
            return {it->second.width, it->second.height};
        }
        return {0, 0};
    }

    void ClothSimulation::SimulateCloth(ClothInstance& cloth, float deltaTime)
    {
        SPARK_WARN_IF(Spark::LogCategory::Physics, deltaTime <= 0.0f,
                      "ClothSimulation::SimulateCloth called with non-positive deltaTime");
        ApplyForces(cloth, deltaTime);
        IntegratePositions(cloth, deltaTime);

        for (int i = 0; i < cloth.solverIterations; ++i)
        {
            SolveConstraints(cloth);
        }

        HandleCollisions(cloth);
    }

    void ClothSimulation::ApplyForces(ClothInstance& cloth, float deltaTime)
    {
        (void)deltaTime;
        for (auto& p : cloth.particles)
        {
            if (p.pinned)
            {
                continue;
            }
            // Gravity
            p.acceleration.x = cloth.gravity.x + cloth.wind.x;
            p.acceleration.y = cloth.gravity.y + cloth.wind.y;
            p.acceleration.z = cloth.gravity.z + cloth.wind.z;
        }
    }

    void ClothSimulation::IntegratePositions(ClothInstance& cloth, float deltaTime)
    {
        float dt2 = deltaTime * deltaTime;
        float dampFactor = 1.0f - cloth.damping;

        for (auto& p : cloth.particles)
        {
            if (p.pinned)
            {
                continue;
            }

            // Verlet integration
            DirectX::XMFLOAT3 temp = p.position;
            p.position.x += (p.position.x - p.prevPosition.x) * dampFactor + p.acceleration.x * dt2;
            p.position.y += (p.position.y - p.prevPosition.y) * dampFactor + p.acceleration.y * dt2;
            p.position.z += (p.position.z - p.prevPosition.z) * dampFactor + p.acceleration.z * dt2;
            p.prevPosition = temp;

            // Update velocity for external queries
            p.velocity.x = (p.position.x - p.prevPosition.x) / deltaTime;
            p.velocity.y = (p.position.y - p.prevPosition.y) / deltaTime;
            p.velocity.z = (p.position.z - p.prevPosition.z) / deltaTime;
        }
    }

    void ClothSimulation::SolveConstraints(ClothInstance& cloth)
    {
        for (const auto& c : cloth.constraints)
        {
            auto& pA = cloth.particles[c.particleA];
            auto& pB = cloth.particles[c.particleB];

            float dx = pB.position.x - pA.position.x;
            float dy = pB.position.y - pA.position.y;
            float dz = pB.position.z - pA.position.z;
            float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

            if (dist < 1e-6f)
            {
                continue;
            }

            float diff = (dist - c.restLength) / dist;
            float totalInvMass = pA.inverseMass + pB.inverseMass;
            if (totalInvMass < 1e-6f)
            {
                continue;
            }

            float correction = diff * c.stiffness;
            float ratioA = pA.inverseMass / totalInvMass;
            float ratioB = pB.inverseMass / totalInvMass;

            if (!pA.pinned)
            {
                pA.position.x += dx * correction * ratioA;
                pA.position.y += dy * correction * ratioA;
                pA.position.z += dz * correction * ratioA;
            }
            if (!pB.pinned)
            {
                pB.position.x -= dx * correction * ratioB;
                pB.position.y -= dy * correction * ratioB;
                pB.position.z -= dz * correction * ratioB;
            }
        }
    }

    void ClothSimulation::HandleCollisions(ClothInstance& cloth)
    {
        for (const auto& collider : cloth.colliders)
        {
            for (auto& p : cloth.particles)
            {
                if (p.pinned)
                {
                    continue;
                }

                if (collider.type == ClothCollider::Type::Sphere)
                {
                    float dx = p.position.x - collider.position.x;
                    float dy = p.position.y - collider.position.y;
                    float dz = p.position.z - collider.position.z;
                    float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

                    if (dist < collider.radius && dist > 1e-6f)
                    {
                        float inv = 1.0f / dist;
                        p.position.x = collider.position.x + dx * inv * collider.radius;
                        p.position.y = collider.position.y + dy * inv * collider.radius;
                        p.position.z = collider.position.z + dz * inv * collider.radius;
                    }
                }
                else if (collider.type == ClothCollider::Type::Plane)
                {
                    float dot = (p.position.x - collider.position.x) * collider.normal.x +
                                (p.position.y - collider.position.y) * collider.normal.y +
                                (p.position.z - collider.position.z) * collider.normal.z;

                    if (dot < 0.0f)
                    {
                        p.position.x -= collider.normal.x * dot;
                        p.position.y -= collider.normal.y * dot;
                        p.position.z -= collider.normal.z * dot;
                    }
                }
            }
        }
    }

    std::string ClothSimulation::Console_GetStatus() const
    {
        std::ostringstream oss;
        oss << "=== Cloth Simulation ===\n";
        oss << "Instances: " << m_clothInstances.size() << "\n";
        for (const auto& [id, cloth] : m_clothInstances)
        {
            oss << "  Cloth " << id << ": " << cloth.particles.size() << " particles, " << cloth.constraints.size()
                << " constraints, " << cloth.colliders.size() << " colliders" << (cloth.enabled ? "" : " [DISABLED]")
                << "\n";
        }
        return oss.str();
    }

} // namespace Spark::Physics

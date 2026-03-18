/**
 * @file PhysicsConsoleOps.cpp
 * @brief Console integration methods for PhysicsSystem
 *
 * All Console_* methods for runtime physics management via console.
 * Split from PhysicsSystem.cpp for maintainability.
 */

#include "PhysicsSystem.h"
#include "../Utils/SparkConsole.h"
#include <sstream>

// ============================================================================
// CONSOLE INTEGRATION METHODS
// ============================================================================

void PhysicsSystem::Console_EnableDebugDraw(bool enabled)
{
    EnableDebugDraw(enabled);
    Spark::SimpleConsole::GetInstance().LogSuccess("Physics debug draw " +
                                                   std::string(enabled ? "enabled" : "disabled"));
}

void PhysicsSystem::Console_PausePhysics(bool paused)
{
    m_paused = paused;
    Spark::SimpleConsole::GetInstance().LogSuccess("Physics simulation " + std::string(paused ? "paused" : "resumed"));
}

void PhysicsSystem::Console_SetTimeStep(float timeStep)
{
    SetTimeStep(timeStep);
    Spark::SimpleConsole::GetInstance().LogSuccess("Physics time step set to: " + std::to_string(timeStep));
}

std::string PhysicsSystem::Console_Raycast(float originX, float originY, float originZ, float dirX, float dirY,
                                           float dirZ, float maxDistance)
{
    XMFLOAT3 origin = {originX, originY, originZ};
    XMFLOAT3 direction = {dirX, dirY, dirZ};

    // Normalize direction
    XMVECTOR dirVector = XMLoadFloat3(&direction);
    dirVector = XMVector3Normalize(dirVector);
    XMStoreFloat3(&direction, dirVector);

    RaycastHit hit = Raycast(origin, direction, maxDistance);

    std::stringstream ss;
    if (hit.hasHit)
    {
        ss << "Raycast HIT:\n";
        ss << "Hit Point: (" << hit.point.x << ", " << hit.point.y << ", " << hit.point.z << ")\n";
        ss << "Hit Normal: (" << hit.normal.x << ", " << hit.normal.y << ", " << hit.normal.z << ")\n";
        ss << "Distance: " << hit.distance << "\n";
        if (hit.body)
        {
            ss << "Hit Body: " << hit.body->GetName() << "\n";
        }
    }
    else
    {
        ss << "Raycast MISS - No objects hit";
    }

    return ss.str();
}

void PhysicsSystem::Console_Reset()
{
    RemoveAllBodies();
    m_constraints.clear();
    SetGravity({0.0f, -9.8f, 0.0f});
    m_paused = false;

    Spark::SimpleConsole::GetInstance().LogSuccess("Physics system reset complete");
}

PhysicsSystem::PhysicsMetrics PhysicsSystem::Console_GetMetrics() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_metrics;
}

std::string PhysicsSystem::Console_ListBodies() const
{
    std::stringstream ss;
    ss << "=== Physics Bodies (" << m_bodies.size() << ") ===\n";

    for (const auto& body : m_bodies)
    {
        if (body)
        {
            ss << body->GetName() << " - " << PhysicsBodyTypeToString(body->GetType());
            auto pos = body->GetPosition();
            ss << " at (" << pos.x << ", " << pos.y << ", " << pos.z << ")\n";
        }
    }

    return ss.str();
}

std::string PhysicsSystem::Console_GetBodyInfo(const std::string& name) const
{
    auto it = m_namedBodies.find(name);
    if (it != m_namedBodies.end() && it->second)
    {
        return it->second->GetInfo();
    }

    return "Physics body not found: " + name;
}

bool PhysicsSystem::Console_CreateBody(const std::string& name, const std::string& type, float x, float y, float z)
{
    PhysicsBodyDesc desc;
    desc.name = name;
    desc.position = {x, y, z};
    desc.type = StringToPhysicsBodyType(type);
    desc.shape.type = CollisionShapeType::Box; // Default to box
    desc.mass = (desc.type == PhysicsBodyType::Static) ? 0.0f : 1.0f;

    auto body = CreateBody(desc);
    return body != nullptr;
}

bool PhysicsSystem::Console_RemoveBody(const std::string& name)
{
    auto it = m_namedBodies.find(name);
    if (it != m_namedBodies.end())
    {
        RemoveBody(it->second);
        return true;
    }

    return false;
}

void PhysicsSystem::Console_SetGravity(float x, float y, float z)
{
    SetGravity({x, y, z});
    Spark::SimpleConsole::GetInstance().LogSuccess("Gravity set to (" + std::to_string(x) + ", " + std::to_string(y) +
                                                   ", " + std::to_string(z) + ")");
}

void PhysicsSystem::Console_SetBodyProperty(const std::string& name, const std::string& property, float value)
{
    auto it = m_namedBodies.find(name);
    if (it != m_namedBodies.end() && it->second)
    {
        it->second->Console_SetProperty(property, value);
        Spark::SimpleConsole::GetInstance().LogSuccess("Set " + property + " = " + std::to_string(value) + " for " +
                                                       name);
    }
    else
    {
        Spark::SimpleConsole::GetInstance().LogError("Physics body not found: " + name);
    }
}

void PhysicsSystem::Console_ApplyForce(const std::string& name, float x, float y, float z)
{
    auto it = m_namedBodies.find(name);
    if (it != m_namedBodies.end() && it->second)
    {
        it->second->ApplyForce({x, y, z});
        Spark::SimpleConsole::GetInstance().LogSuccess("Applied force (" + std::to_string(x) + ", " +
                                                       std::to_string(y) + ", " + std::to_string(z) + ") to " + name);
    }
    else
    {
        Spark::SimpleConsole::GetInstance().LogError("Physics body not found: " + name);
    }
}

void PhysicsSystem::Console_ApplyImpulse(const std::string& name, float x, float y, float z)
{
    auto it = m_namedBodies.find(name);
    if (it != m_namedBodies.end() && it->second)
    {
        it->second->ApplyImpulse({x, y, z});
        Spark::SimpleConsole::GetInstance().LogSuccess("Applied impulse (" + std::to_string(x) + ", " +
                                                       std::to_string(y) + ", " + std::to_string(z) + ") to " + name);
    }
    else
    {
        Spark::SimpleConsole::GetInstance().LogError("Physics body not found: " + name);
    }
}

// ============================================================================
// UTILITY FUNCTIONS IMPLEMENTATION - MISSING GLOBAL FUNCTIONS
// ============================================================================

std::string PhysicsBodyTypeToString(PhysicsBodyType type)
{
    switch (type)
    {
    case PhysicsBodyType::Static:
        return "Static";
    case PhysicsBodyType::Kinematic:
        return "Kinematic";
    case PhysicsBodyType::Dynamic:
        return "Dynamic";
    default:
        return "Unknown";
    }
}

PhysicsBodyType StringToPhysicsBodyType(const std::string& str)
{
    if (str == "Static" || str == "static")
        return PhysicsBodyType::Static;
    if (str == "Kinematic" || str == "kinematic")
        return PhysicsBodyType::Kinematic;
    if (str == "Dynamic" || str == "dynamic")
        return PhysicsBodyType::Dynamic;
    return PhysicsBodyType::Dynamic; // Default
}

std::string CollisionShapeTypeToString(CollisionShapeType type)
{
    switch (type)
    {
    case CollisionShapeType::Box:
        return "Box";
    case CollisionShapeType::Sphere:
        return "Sphere";
    case CollisionShapeType::Capsule:
        return "Capsule";
    case CollisionShapeType::Cylinder:
        return "Cylinder";
    case CollisionShapeType::Cone:
        return "Cone";
    case CollisionShapeType::Mesh:
        return "Mesh";
    case CollisionShapeType::ConvexHull:
        return "ConvexHull";
    case CollisionShapeType::Heightfield:
        return "Heightfield";
    case CollisionShapeType::Compound:
        return "Compound";
    default:
        return "Unknown";
    }
}

CollisionShapeType StringToCollisionShapeType(const std::string& str)
{
    if (str == "Box" || str == "box")
        return CollisionShapeType::Box;
    if (str == "Sphere" || str == "sphere")
        return CollisionShapeType::Sphere;
    if (str == "Capsule" || str == "capsule")
        return CollisionShapeType::Capsule;
    if (str == "Cylinder" || str == "cylinder")
        return CollisionShapeType::Cylinder;
    if (str == "Cone" || str == "cone")
        return CollisionShapeType::Cone;
    if (str == "Mesh" || str == "mesh")
        return CollisionShapeType::Mesh;
    if (str == "ConvexHull" || str == "convexhull")
        return CollisionShapeType::ConvexHull;
    if (str == "Heightfield" || str == "heightfield")
        return CollisionShapeType::Heightfield;
    if (str == "Compound" || str == "compound")
        return CollisionShapeType::Compound;
    return CollisionShapeType::Box; // Default
}

std::string ConstraintTypeToString(ConstraintType type)
{
    switch (type)
    {
    case ConstraintType::Point2Point:
        return "Point2Point";
    case ConstraintType::Hinge:
        return "Hinge";
    case ConstraintType::Slider:
        return "Slider";
    case ConstraintType::ConeTwist:
        return "ConeTwist";
    case ConstraintType::Generic6DOF:
        return "Generic6DOF";
    case ConstraintType::Fixed:
        return "Fixed";
    default:
        return "Unknown";
    }
}

ConstraintType StringToConstraintType(const std::string& str)
{
    if (str == "Point2Point" || str == "point2point")
        return ConstraintType::Point2Point;
    if (str == "Hinge" || str == "hinge")
        return ConstraintType::Hinge;
    if (str == "Slider" || str == "slider")
        return ConstraintType::Slider;
    if (str == "ConeTwist" || str == "conetwist")
        return ConstraintType::ConeTwist;
    if (str == "Generic6DOF" || str == "generic6dof")
        return ConstraintType::Generic6DOF;
    if (str == "Fixed" || str == "fixed")
        return ConstraintType::Fixed;
    return ConstraintType::Fixed; // Default
}

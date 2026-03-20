#include "../Core/Platform.h"
/**
 * @file PhysicsSystemQueries.cpp
 * @brief Body/constraint management and spatial queries for PhysicsSystem
 * @author Spark Engine Team
 * @date 2025
 *
 * Extracted from PhysicsSystem.cpp. Contains body creation/removal, all
 * constraint creation methods, raycasting, overlap queries, sweep tests,
 * debug rendering, material management, and metrics retrieval.
 */

#include "PhysicsSystem.h"
#include "Utils/Assert.h"
#include "../Utils/SparkConsole.h"
#include "../Utils/Validate.h"

#include <BulletCollision/CollisionDispatch/btGhostObject.h>
#include <btBulletDynamicsCommon.h>

#include <algorithm>
#include <mutex>

using namespace DirectX;

// ============================================================================
// BODY CREATION / REMOVAL
// ============================================================================

std::shared_ptr<PhysicsBody> PhysicsSystem::CreateBody(const PhysicsBodyDesc& desc)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Physics);
    SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Physics, m_dynamicsWorld, nullptr);

    // Create collision shape
    btCollisionShape* shape = CreateCollisionShape(desc.shape);
    if (!shape)
        return nullptr;

    // Determine mass (0 for static and kinematic bodies)
    float mass = desc.mass;
    if (desc.type == PhysicsBodyType::Static || desc.isKinematic)
    {
        mass = 0.0f;
    }

    // Calculate local inertia
    btVector3 localInertia(0, 0, 0);
    if (mass > 0.0f)
    {
        shape->calculateLocalInertia(mass, localInertia);
    }

    // Create initial transform
    btTransform startTransform;
    startTransform.setIdentity();
    startTransform.setOrigin(ToBullet(desc.position));
    startTransform.setRotation(ToBulletQuaternion(desc.rotation));

    // Create motion state
    btDefaultMotionState* motionState = new btDefaultMotionState(startTransform);

    // Create rigid body construction info
    btRigidBody::btRigidBodyConstructionInfo rbInfo(mass, motionState, shape, localInertia);
    rbInfo.m_friction = desc.material.friction;
    rbInfo.m_restitution = desc.material.restitution;
    rbInfo.m_linearDamping = desc.material.linearDamping;
    rbInfo.m_angularDamping = desc.material.angularDamping;

    // Create the Bullet rigid body
    btRigidBody* bulletBody = new btRigidBody(rbInfo);

    // Set kinematic flags
    if (desc.isKinematic)
    {
        bulletBody->setCollisionFlags(bulletBody->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
        bulletBody->setActivationState(DISABLE_DEACTIVATION);
    }

    // Set trigger flags
    if (desc.isTrigger)
    {
        bulletBody->setCollisionFlags(bulletBody->getCollisionFlags() | btCollisionObject::CF_NO_CONTACT_RESPONSE);
    }

    // Set initial velocities
    if (mass > 0.0f)
    {
        bulletBody->setLinearVelocity(ToBullet(desc.linearVelocity));
        bulletBody->setAngularVelocity(ToBullet(desc.angularVelocity));
    }

    // Add to dynamics world with collision filtering from desc
    m_dynamicsWorld->addRigidBody(bulletBody, desc.collisionGroup, desc.collisionMask);

    // Create the PhysicsBody wrapper (inherits group/mask from desc)
    auto body = std::make_shared<PhysicsBody>(desc, bulletBody);
    body->SetCollisionGroup(desc.collisionGroup);
    body->SetCollisionMask(desc.collisionMask);

    // Store user data pointer on the bullet body for lookup during collision
    bulletBody->setUserPointer(body.get());

    m_bodies.push_back(body);

    if (!desc.name.empty())
    {
        m_namedBodies[desc.name] = body;
    }

    Spark::SimpleConsole::GetInstance().LogInfo("Created physics body: " + desc.name);
    return body;
}

void PhysicsSystem::RemoveBody(std::shared_ptr<PhysicsBody> body)
{
    if (!body)
        return;

    // Remove from Bullet world
    if (m_dynamicsWorld && body->GetBulletBody())
    {
        m_dynamicsWorld->removeRigidBody(body->GetBulletBody());
    }

    // Remove from named bodies
    const std::string& name = body->GetName();
    if (!name.empty())
    {
        m_namedBodies.erase(name);
    }

    // Remove from bodies list
    auto it = std::find(m_bodies.begin(), m_bodies.end(), body);
    if (it != m_bodies.end())
    {
        m_bodies.erase(it);
    }
}

void PhysicsSystem::RemoveAllBodies()
{
    if (m_dynamicsWorld)
    {
        for (auto& body : m_bodies)
        {
            if (body && body->GetBulletBody())
            {
                m_dynamicsWorld->removeRigidBody(body->GetBulletBody());
            }
        }
    }

    m_bodies.clear();
    m_namedBodies.clear();
}

// ============================================================================
// CONSTRAINT CREATION
// ============================================================================

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreateHingeConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                                        std::shared_ptr<PhysicsBody> bodyB,
                                                                        const XMFLOAT3& pivotA, const XMFLOAT3& pivotB,
                                                                        const XMFLOAT3& axisA, const XMFLOAT3& axisB)
{
    if (!m_dynamicsWorld || !bodyA || !bodyB)
        return nullptr;
    if (!bodyA->GetBulletBody() || !bodyB->GetBulletBody())
        return nullptr;

    btHingeConstraint* hingeConstraint =
        new btHingeConstraint(*bodyA->GetBulletBody(), *bodyB->GetBulletBody(), ToBullet(pivotA), ToBullet(pivotB),
                              ToBullet(axisA), ToBullet(axisB));

    m_dynamicsWorld->addConstraint(hingeConstraint, true);

    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::Hinge, hingeConstraint);
    m_constraints.push_back(constraint);

    return constraint;
}

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreateSliderConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                                         std::shared_ptr<PhysicsBody> bodyB,
                                                                         const XMMATRIX& frameA, const XMMATRIX& frameB)
{
    if (!m_dynamicsWorld || !bodyA || !bodyB)
        return nullptr;
    if (!bodyA->GetBulletBody() || !bodyB->GetBulletBody())
        return nullptr;

    // Convert XMMATRIX frames to btTransform
    btTransform btFrameA, btFrameB;

    XMVECTOR scaleA, rotA, transA;
    XMMatrixDecompose(&scaleA, &rotA, &transA, frameA);
    XMFLOAT3 posA;
    XMStoreFloat3(&posA, transA);
    XMFLOAT4 quatA;
    XMStoreFloat4(&quatA, rotA);
    btFrameA.setOrigin(btVector3(posA.x, posA.y, posA.z));
    btFrameA.setRotation(btQuaternion(quatA.x, quatA.y, quatA.z, quatA.w));

    XMVECTOR scaleB, rotB, transB;
    XMMatrixDecompose(&scaleB, &rotB, &transB, frameB);
    XMFLOAT3 posB;
    XMStoreFloat3(&posB, transB);
    XMFLOAT4 quatB;
    XMStoreFloat4(&quatB, rotB);
    btFrameB.setOrigin(btVector3(posB.x, posB.y, posB.z));
    btFrameB.setRotation(btQuaternion(quatB.x, quatB.y, quatB.z, quatB.w));

    btSliderConstraint* sliderConstraint =
        new btSliderConstraint(*bodyA->GetBulletBody(), *bodyB->GetBulletBody(), btFrameA, btFrameB, true);

    m_dynamicsWorld->addConstraint(sliderConstraint, true);

    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::Slider, sliderConstraint);
    m_constraints.push_back(constraint);

    return constraint;
}

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreateFixedConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                                        std::shared_ptr<PhysicsBody> bodyB,
                                                                        const XMMATRIX& frameA, const XMMATRIX& frameB)
{
    if (!m_dynamicsWorld || !bodyA || !bodyB)
        return nullptr;
    if (!bodyA->GetBulletBody() || !bodyB->GetBulletBody())
        return nullptr;

    // Convert XMMATRIX frames to btTransform
    btTransform btFrameA, btFrameB;

    XMVECTOR scaleA, rotA, transA;
    XMMatrixDecompose(&scaleA, &rotA, &transA, frameA);
    XMFLOAT3 posA;
    XMStoreFloat3(&posA, transA);
    XMFLOAT4 quatA;
    XMStoreFloat4(&quatA, rotA);
    btFrameA.setOrigin(btVector3(posA.x, posA.y, posA.z));
    btFrameA.setRotation(btQuaternion(quatA.x, quatA.y, quatA.z, quatA.w));

    XMVECTOR scaleB, rotB, transB;
    XMMatrixDecompose(&scaleB, &rotB, &transB, frameB);
    XMFLOAT3 posB;
    XMStoreFloat3(&posB, transB);
    XMFLOAT4 quatB;
    XMStoreFloat4(&quatB, rotB);
    btFrameB.setOrigin(btVector3(posB.x, posB.y, posB.z));
    btFrameB.setRotation(btQuaternion(quatB.x, quatB.y, quatB.z, quatB.w));

    btFixedConstraint* fixedConstraint =
        new btFixedConstraint(*bodyA->GetBulletBody(), *bodyB->GetBulletBody(), btFrameA, btFrameB);

    m_dynamicsWorld->addConstraint(fixedConstraint, true);

    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::Fixed, fixedConstraint);
    m_constraints.push_back(constraint);

    return constraint;
}

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreatePoint2PointConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                                              std::shared_ptr<PhysicsBody> bodyB,
                                                                              const XMFLOAT3& pivotA,
                                                                              const XMFLOAT3& pivotB)
{
    if (!m_dynamicsWorld || !bodyA)
        return nullptr;
    if (!bodyA->GetBulletBody())
        return nullptr;

    btPoint2PointConstraint* p2pConstraint = nullptr;

    if (bodyB && bodyB->GetBulletBody())
    {
        // Two-body constraint
        p2pConstraint = new btPoint2PointConstraint(*bodyA->GetBulletBody(), *bodyB->GetBulletBody(), ToBullet(pivotA),
                                                    ToBullet(pivotB));
    }
    else
    {
        // Single-body constraint anchored to world
        p2pConstraint = new btPoint2PointConstraint(*bodyA->GetBulletBody(), ToBullet(pivotA));
    }

    m_dynamicsWorld->addConstraint(p2pConstraint, true);

    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::Point2Point, p2pConstraint);
    m_constraints.push_back(constraint);

    return constraint;
}

std::shared_ptr<PhysicsConstraint> PhysicsSystem::CreateConeTwistConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                                            std::shared_ptr<PhysicsBody> bodyB,
                                                                            const XMMATRIX& frameA,
                                                                            const XMMATRIX& frameB, float swingSpan1,
                                                                            float swingSpan2, float twistSpan)
{
    if (!m_dynamicsWorld || !bodyA || !bodyB)
        return nullptr;
    if (!bodyA->GetBulletBody() || !bodyB->GetBulletBody())
        return nullptr;

    // Convert XMMATRIX frames to btTransform
    btTransform btFrameA, btFrameB;

    XMVECTOR scaleA, rotA, transA;
    XMMatrixDecompose(&scaleA, &rotA, &transA, frameA);
    XMFLOAT3 posA;
    XMStoreFloat3(&posA, transA);
    XMFLOAT4 quatA;
    XMStoreFloat4(&quatA, rotA);
    btFrameA.setOrigin(btVector3(posA.x, posA.y, posA.z));
    btFrameA.setRotation(btQuaternion(quatA.x, quatA.y, quatA.z, quatA.w));

    XMVECTOR scaleB, rotB, transB;
    XMMatrixDecompose(&scaleB, &rotB, &transB, frameB);
    XMFLOAT3 posB;
    XMStoreFloat3(&posB, transB);
    XMFLOAT4 quatB;
    XMStoreFloat4(&quatB, rotB);
    btFrameB.setOrigin(btVector3(posB.x, posB.y, posB.z));
    btFrameB.setRotation(btQuaternion(quatB.x, quatB.y, quatB.z, quatB.w));

    btConeTwistConstraint* coneTwist =
        new btConeTwistConstraint(*bodyA->GetBulletBody(), *bodyB->GetBulletBody(), btFrameA, btFrameB);

    coneTwist->setLimit(swingSpan1, swingSpan2, twistSpan);

    m_dynamicsWorld->addConstraint(coneTwist, true);

    auto constraint = std::make_shared<PhysicsConstraint>(ConstraintType::ConeTwist, coneTwist);
    m_constraints.push_back(constraint);

    return constraint;
}

void PhysicsSystem::RemoveConstraint(std::shared_ptr<PhysicsConstraint> constraint)
{
    if (!constraint)
        return;

    if (m_dynamicsWorld && constraint->GetBulletConstraint())
    {
        m_dynamicsWorld->removeConstraint(constraint->GetBulletConstraint());
    }

    auto it = std::find(m_constraints.begin(), m_constraints.end(), constraint);
    if (it != m_constraints.end())
    {
        m_constraints.erase(it);
    }
}

// ============================================================================
// RAYCASTING AND OVERLAP QUERIES
// ============================================================================

RaycastHit PhysicsSystem::Raycast(const XMFLOAT3& origin, const XMFLOAT3& direction, float maxDistance)
{
    RaycastHit hit;
    hit.hasHit = false;

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.raycastCount++;
    }

    if (!m_dynamicsWorld)
        return hit;

    btVector3 from = ToBullet(origin);
    btVector3 to = from + ToBullet(direction) * maxDistance;

    btCollisionWorld::ClosestRayResultCallback callback(from, to);
    m_dynamicsWorld->rayTest(from, to, callback);

    if (callback.hasHit())
    {
        hit.hasHit = true;
        hit.point = FromBullet(callback.m_hitPointWorld);
        hit.normal = FromBullet(callback.m_hitNormalWorld);

        btVector3 diff = callback.m_hitPointWorld - from;
        hit.distance = diff.length();

        // Find the PhysicsBody wrapper
        const btCollisionObject* hitObj = callback.m_collisionObject;
        if (hitObj)
        {
            hit.body = static_cast<PhysicsBody*>(hitObj->getUserPointer());
            if (hit.body)
            {
                hit.userData = hit.body->GetUserData();
            }
        }
    }

    return hit;
}

std::vector<RaycastHit> PhysicsSystem::RaycastAll(const XMFLOAT3& origin, const XMFLOAT3& direction, float maxDistance)
{
    std::vector<RaycastHit> hits;

    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        m_metrics.raycastCount++;
    }

    if (!m_dynamicsWorld)
        return hits;

    btVector3 from = ToBullet(origin);
    btVector3 to = from + ToBullet(direction) * maxDistance;

    btCollisionWorld::AllHitsRayResultCallback callback(from, to);
    m_dynamicsWorld->rayTest(from, to, callback);

    if (callback.hasHit())
    {
        for (int i = 0; i < callback.m_hitPointWorld.size(); i++)
        {
            RaycastHit hit;
            hit.hasHit = true;
            hit.point = FromBullet(callback.m_hitPointWorld[i]);
            hit.normal = FromBullet(callback.m_hitNormalWorld[i]);

            btVector3 diff = callback.m_hitPointWorld[i] - from;
            hit.distance = diff.length();

            const btCollisionObject* hitObj = callback.m_collisionObjects[i];
            if (hitObj)
            {
                hit.body = static_cast<PhysicsBody*>(hitObj->getUserPointer());
                if (hit.body)
                {
                    hit.userData = hit.body->GetUserData();
                }
            }

            hits.push_back(hit);
        }
    }

    return hits;
}

bool PhysicsSystem::SphereOverlap(const XMFLOAT3& center, float radius, std::vector<PhysicsBody*>& results)
{
    results.clear();

    if (!m_dynamicsWorld)
        return false;

    btSphereShape sphereShape(radius);
    btGhostObject ghostObject;
    ghostObject.setCollisionShape(&sphereShape);

    btTransform transform;
    transform.setIdentity();
    transform.setOrigin(ToBullet(center));
    ghostObject.setWorldTransform(transform);

    // Use contactTest to find overlapping objects
    struct ContactCallback : public btCollisionWorld::ContactResultCallback
    {
        std::vector<PhysicsBody*>& m_results;

        ContactCallback(std::vector<PhysicsBody*>& results) : m_results(results) {}

        btScalar addSingleResult(btManifoldPoint& cp, const btCollisionObjectWrapper* colObj0Wrap, int partId0,
                                 int index0, const btCollisionObjectWrapper* colObj1Wrap, int partId1,
                                 int index1) override
        {
            const btCollisionObject* obj = colObj1Wrap->getCollisionObject();
            if (obj)
            {
                PhysicsBody* body = static_cast<PhysicsBody*>(obj->getUserPointer());
                if (body)
                {
                    // Avoid duplicates
                    if (std::find(m_results.begin(), m_results.end(), body) == m_results.end())
                    {
                        m_results.push_back(body);
                    }
                }
            }
            return btScalar(1.0);
        }
    };

    ContactCallback callback(results);
    m_dynamicsWorld->contactTest(&ghostObject, callback);

    return !results.empty();
}

bool PhysicsSystem::BoxOverlap(const XMFLOAT3& center, const XMFLOAT3& halfExtents, std::vector<PhysicsBody*>& results)
{
    results.clear();

    if (!m_dynamicsWorld)
        return false;

    btBoxShape boxShape(ToBullet(halfExtents));
    btGhostObject ghostObject;
    ghostObject.setCollisionShape(&boxShape);

    btTransform transform;
    transform.setIdentity();
    transform.setOrigin(ToBullet(center));
    ghostObject.setWorldTransform(transform);

    struct ContactCallback : public btCollisionWorld::ContactResultCallback
    {
        std::vector<PhysicsBody*>& m_results;

        ContactCallback(std::vector<PhysicsBody*>& results) : m_results(results) {}

        btScalar addSingleResult(btManifoldPoint& cp, const btCollisionObjectWrapper* colObj0Wrap, int partId0,
                                 int index0, const btCollisionObjectWrapper* colObj1Wrap, int partId1,
                                 int index1) override
        {
            const btCollisionObject* obj = colObj1Wrap->getCollisionObject();
            if (obj)
            {
                PhysicsBody* body = static_cast<PhysicsBody*>(obj->getUserPointer());
                if (body)
                {
                    if (std::find(m_results.begin(), m_results.end(), body) == m_results.end())
                    {
                        m_results.push_back(body);
                    }
                }
            }
            return btScalar(1.0);
        }
    };

    ContactCallback callback(results);
    m_dynamicsWorld->contactTest(&ghostObject, callback);

    return !results.empty();
}

// ============================================================================
// SHAPE CASTING (SWEEP TESTS)
// ============================================================================

RaycastHit PhysicsSystem::SphereCast(float radius, const XMFLOAT3& from, const XMFLOAT3& to)
{
    RaycastHit hit;
    hit.hasHit = false;

    if (!m_dynamicsWorld)
        return hit;

    btSphereShape sphereShape(radius);

    btTransform fromTransform;
    fromTransform.setIdentity();
    fromTransform.setOrigin(ToBullet(from));

    btTransform toTransform;
    toTransform.setIdentity();
    toTransform.setOrigin(ToBullet(to));

    btCollisionWorld::ClosestConvexResultCallback callback(ToBullet(from), ToBullet(to));
    callback.m_collisionFilterGroup = 1;
    callback.m_collisionFilterMask = 0xFFFF;

    m_dynamicsWorld->convexSweepTest(&sphereShape, fromTransform, toTransform, callback);

    if (callback.hasHit())
    {
        hit.hasHit = true;
        hit.point = FromBullet(callback.m_hitPointWorld);
        hit.normal = FromBullet(callback.m_hitNormalWorld);

        btVector3 diff = callback.m_hitPointWorld - ToBullet(from);
        hit.distance = diff.length();

        const btCollisionObject* hitObj = callback.m_hitCollisionObject;
        if (hitObj)
        {
            hit.body = static_cast<PhysicsBody*>(hitObj->getUserPointer());
            if (hit.body)
            {
                hit.userData = hit.body->GetUserData();
            }
        }
    }

    return hit;
}

RaycastHit PhysicsSystem::BoxCast(const XMFLOAT3& halfExtents, const XMFLOAT3& from, const XMFLOAT3& to)
{
    RaycastHit hit;
    hit.hasHit = false;

    if (!m_dynamicsWorld)
        return hit;

    btBoxShape boxShape(ToBullet(halfExtents));

    btTransform fromTransform;
    fromTransform.setIdentity();
    fromTransform.setOrigin(ToBullet(from));

    btTransform toTransform;
    toTransform.setIdentity();
    toTransform.setOrigin(ToBullet(to));

    btCollisionWorld::ClosestConvexResultCallback callback(ToBullet(from), ToBullet(to));
    callback.m_collisionFilterGroup = 1;
    callback.m_collisionFilterMask = 0xFFFF;

    m_dynamicsWorld->convexSweepTest(&boxShape, fromTransform, toTransform, callback);

    if (callback.hasHit())
    {
        hit.hasHit = true;
        hit.point = FromBullet(callback.m_hitPointWorld);
        hit.normal = FromBullet(callback.m_hitNormalWorld);

        btVector3 diff = callback.m_hitPointWorld - ToBullet(from);
        hit.distance = diff.length();

        const btCollisionObject* hitObj = callback.m_hitCollisionObject;
        if (hitObj)
        {
            hit.body = static_cast<PhysicsBody*>(hitObj->getUserPointer());
            if (hit.body)
            {
                hit.userData = hit.body->GetUserData();
            }
        }
    }

    return hit;
}

RaycastHit PhysicsSystem::CapsuleCast(float radius, float height, const XMFLOAT3& from, const XMFLOAT3& to)
{
    RaycastHit hit;
    hit.hasHit = false;

    if (!m_dynamicsWorld)
        return hit;

    btCapsuleShape capsuleShape(radius, height);

    btTransform fromTransform;
    fromTransform.setIdentity();
    fromTransform.setOrigin(ToBullet(from));

    btTransform toTransform;
    toTransform.setIdentity();
    toTransform.setOrigin(ToBullet(to));

    btCollisionWorld::ClosestConvexResultCallback callback(ToBullet(from), ToBullet(to));
    callback.m_collisionFilterGroup = 1;
    callback.m_collisionFilterMask = 0xFFFF;

    m_dynamicsWorld->convexSweepTest(&capsuleShape, fromTransform, toTransform, callback);

    if (callback.hasHit())
    {
        hit.hasHit = true;
        hit.point = FromBullet(callback.m_hitPointWorld);
        hit.normal = FromBullet(callback.m_hitNormalWorld);

        btVector3 diff = callback.m_hitPointWorld - ToBullet(from);
        hit.distance = diff.length();

        const btCollisionObject* hitObj = callback.m_hitCollisionObject;
        if (hitObj)
        {
            hit.body = static_cast<PhysicsBody*>(hitObj->getUserPointer());
            if (hit.body)
            {
                hit.userData = hit.body->GetUserData();
            }
        }
    }

    return hit;
}

// ============================================================================
// DEBUG RENDERING
// ============================================================================

void PhysicsSystem::SetDebugDrawMode(int mode)
{
    if (!m_dynamicsWorld)
        return;

    m_debugDrawEnabled = (mode != 0);

    if (m_dynamicsWorld->getDebugDrawer())
    {
        m_dynamicsWorld->getDebugDrawer()->setDebugMode(mode);
    }
}

void PhysicsSystem::RenderDebug()
{
    if (!m_dynamicsWorld || !m_debugDrawEnabled)
        return;

    m_dynamicsWorld->debugDrawWorld();
}

// ============================================================================
// MATERIAL MANAGEMENT
// ============================================================================

void PhysicsSystem::RegisterMaterial(const std::string& name, const PhysicsMaterial& material)
{
    m_materials[name] = material;
}

const PhysicsMaterial* PhysicsSystem::GetMaterial(const std::string& name) const
{
    auto it = m_materials.find(name);
    return (it != m_materials.end()) ? &it->second : nullptr;
}

// ============================================================================
// METRICS
// ============================================================================

PhysicsSystem::PhysicsMetrics PhysicsSystem::GetMetrics() const
{
    std::lock_guard<std::mutex> lock(m_metricsMutex);
    return m_metrics;
}

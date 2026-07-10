/**
 * @file TFWorldCollision.cpp
 * @brief Scene-file -> static Jolt box bodies + the shared capsule move resolver.
 *
 * See TFWorldCollision.h for the terrain/object split and the determinism
 * contract. Everything here must stay byte-identical in behavior between the
 * dedicated server and the predicting client: same file parse, same float
 * math, same query parameters.
 */
#include "World/TFWorldCollision.h"

#include "Game/TFMovementModel.h" // kTFPawnRadiusM / kTFPawnHeightM

#include "Physics/PhysicsSystem.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <unordered_map>

namespace Terrafront
{

    namespace
    {

        constexpr size_t kMaxBodies = 512;       // safety cap (scene ships 109 objects)
        constexpr float kMinHalfExtentM = 0.05f; // degenerate-AABB floor

        /// Parse "a,b,c" into out[3]. Missing components keep their prior value.
        void ParseFloat3(const std::string& v, float out[3])
        {
            const char* c = v.c_str();
            char* end = nullptr;
            for (int i = 0; i < 3; ++i)
            {
                out[i] = std::strtof(c, &end);
                if (end == c)
                    return; // malformed tail: keep defaults for the rest
                c = end;
                while (*c == ',' || *c == ' ')
                    ++c;
            }
        }

    } // namespace

    TFWorldCollision::TFWorldCollision() = default;
    TFWorldCollision::~TFWorldCollision()
    {
        Shutdown();
    }

    // -----------------------------------------------------------------------
    // Scene parse (same INI dialect TFWorldSetup::ParseTerrainParams reads)
    // -----------------------------------------------------------------------

    bool TFWorldCollision::ParseScene(const std::string& path, std::vector<SceneObj>& out)
    {
        std::ifstream f(path);
        if (!f.is_open())
            return false;

        std::string line;
        bool inObject = false;
        SceneObj cur;

        auto flush = [&]()
        {
            if (inObject && (cur.type == "cube" || cur.type == "Cube" || cur.type == "model" || cur.type == "Model"))
                out.push_back(cur);
            cur = SceneObj{};
        };

        while (std::getline(f, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (line.empty() || line[0] == '#' || line[0] == ';')
                continue;
            if (line.front() == '[' && line.back() == ']')
            {
                flush();
                inObject = (line == "[Object]");
                continue;
            }
            if (!inObject)
                continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string key = line.substr(0, eq);
            const std::string val = line.substr(eq + 1);

            if (key == "type")
                cur.type = val;
            else if (key == "name")
                cur.name = val;
            else if (key == "model")
                cur.model = val;
            else if (key == "position")
                ParseFloat3(val, cur.pos);
            else if (key == "scale")
                ParseFloat3(val, cur.scale);
            else if (key == "rotation")
                ParseFloat3(val, cur.rotDeg);
        }
        flush();
        return true;
    }

    bool TFWorldCollision::ObjLocalAabb(const std::string& objPath, float outMin[3], float outMax[3])
    {
        std::ifstream f(objPath);
        if (!f.is_open())
            return false;

        bool any = false;
        std::string line;
        while (std::getline(f, line))
        {
            if (line.size() < 3 || line[0] != 'v' || line[1] != ' ')
                continue;
            float v[3] = {0.0f, 0.0f, 0.0f};
            ParseFloat3(line.substr(2), v);
            if (!any)
            {
                outMin[0] = outMax[0] = v[0];
                outMin[1] = outMax[1] = v[1];
                outMin[2] = outMax[2] = v[2];
                any = true;
            }
            else
            {
                for (int i = 0; i < 3; ++i)
                {
                    outMin[i] = std::min(outMin[i], v[i]);
                    outMax[i] = std::max(outMax[i], v[i]);
                }
            }
        }
        return any;
    }

    // -----------------------------------------------------------------------
    // Build
    // -----------------------------------------------------------------------

    bool TFWorldCollision::Build(TFGameContext& ctx, const std::string& scenePath)
    {
        Shutdown();

        ::PhysicsSystem* physics = ctx.engine ? ctx.engine->GetPhysics() : nullptr;
        // Jolt-live probe: the no-Jolt stub returns valid dummy bodies, so the
        // only trustworthy signal is the Jolt world itself (TFVehiclePhysics
        // uses the identical probe).
        if (!physics || !physics->GetJoltSystem())
        {
            m_physics = nullptr;
            SPARK_LOG_INFO(Spark::LogCategory::Game,
                           "[TF] world collision: no live Jolt world - terrain-clamp-only movement");
            return false;
        }

        std::vector<SceneObj> objs;
        if (!ParseScene(scenePath, objs) || objs.empty())
        {
            m_physics = nullptr;
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] world collision: cannot parse %s - no static bodies built",
                           scenePath.c_str());
            return false;
        }
        m_physics = physics;

        // Local AABB per unique OBJ (streamed once; deterministic file order).
        std::unordered_map<std::string, std::pair<std::array<float, 3>, std::array<float, 3>>> objAabbs;

        size_t created = 0;
        for (const SceneObj& o : objs)
        {
            if (m_bodies.size() >= kMaxBodies)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] world collision: body cap %zu reached", kMaxBodies);
                break;
            }

            // Local AABB: unit cube for primitives (SceneManager instantiates
            // CubeObject(1.0f): centered, full size 1), OBJ vertex bounds for
            // models. Missing OBJ falls back to the same unit cube the engine's
            // placeholder mesh path uses.
            float lmin[3] = {-0.5f, -0.5f, -0.5f};
            float lmax[3] = {0.5f, 0.5f, 0.5f};
            if (o.type == "model" || o.type == "Model")
            {
                auto it = objAabbs.find(o.model);
                if (it == objAabbs.end())
                {
                    float mn[3], mx[3];
                    if (ObjLocalAabb(o.model, mn, mx))
                        it = objAabbs
                                 .emplace(o.model, std::make_pair(std::array<float, 3>{mn[0], mn[1], mn[2]},
                                                                  std::array<float, 3>{mx[0], mx[1], mx[2]}))
                                 .first;
                    else
                        SPARK_LOG_WARN(Spark::LogCategory::Game,
                                       "[TF] world collision: OBJ %s unreadable - unit-cube fallback for %s",
                                       o.model.c_str(), o.name.c_str());
                }
                if (it != objAabbs.end())
                {
                    for (int i = 0; i < 3; ++i)
                    {
                        lmin[i] = it->second.first[i];
                        lmax[i] = it->second.second[i];
                    }
                }
            }

            // World AABB: transform the 8 scaled local corners by the node's
            // Euler-degree rotation (scene content is Y-only: 0..315 deg) and
            // translation, then take min/max. Exact for 90-degree steps,
            // conservatively larger for diagonal barriers - fine for blocking.
            const float rx = o.rotDeg[0] * 0.01745329252f;
            const float ry = o.rotDeg[1] * 0.01745329252f;
            const float rz = o.rotDeg[2] * 0.01745329252f;
            const float cx = std::cos(rx), sx = std::sin(rx);
            const float cyaw = std::cos(ry), syaw = std::sin(ry);
            const float cz = std::cos(rz), sz = std::sin(rz);

            float wmin[3] = {0, 0, 0}, wmax[3] = {0, 0, 0};
            for (int corner = 0; corner < 8; ++corner)
            {
                float p[3] = {(corner & 1 ? lmax[0] : lmin[0]) * o.scale[0],
                              (corner & 2 ? lmax[1] : lmin[1]) * o.scale[1],
                              (corner & 4 ? lmax[2] : lmin[2]) * o.scale[2]};
                // R = Ry(yaw) * Rx(pitch) * Rz(roll), applied to column vector.
                float q[3] = {p[0] * cz - p[1] * sz, p[0] * sz + p[1] * cz, p[2]};          // roll (Z)
                float r[3] = {q[0], q[1] * cx - q[2] * sx, q[1] * sx + q[2] * cx};          // pitch (X)
                float w[3] = {r[0] * cyaw + r[2] * syaw, r[1], -r[0] * syaw + r[2] * cyaw}; // yaw (Y)
                for (int i = 0; i < 3; ++i)
                    w[i] += o.pos[i];
                if (corner == 0)
                {
                    for (int i = 0; i < 3; ++i)
                    {
                        wmin[i] = w[i];
                        wmax[i] = w[i];
                    }
                }
                else
                {
                    for (int i = 0; i < 3; ++i)
                    {
                        wmin[i] = std::min(wmin[i], w[i]);
                        wmax[i] = std::max(wmax[i], w[i]);
                    }
                }
            }

            PhysicsBodyDesc desc;
            desc.type = PhysicsBodyType::Static;
            desc.mass = 0.0f;
            desc.position = {(wmin[0] + wmax[0]) * 0.5f, (wmin[1] + wmax[1]) * 0.5f, (wmin[2] + wmax[2]) * 0.5f};
            desc.shape.type = CollisionShapeType::Box;
            desc.shape.dimensions = {std::max((wmax[0] - wmin[0]) * 0.5f, kMinHalfExtentM),
                                     std::max((wmax[1] - wmin[1]) * 0.5f, kMinHalfExtentM),
                                     std::max((wmax[2] - wmin[2]) * 0.5f, kMinHalfExtentM)};
            desc.material.friction = 0.7f;
            desc.material.restitution = 0.0f;
            // desc.collisionGroup defaults to WorldStatic but set it explicitly
            // anyway (engine fact: never rely on the default).
            desc.collisionGroup = CollisionLayers::WorldStatic;
            desc.collisionMask = CollisionLayers::All;
            desc.name = "TF_WCol_" + (o.name.empty() ? std::string("anon") : o.name);

            std::shared_ptr<PhysicsBody> body = m_physics->CreateBody(desc);
            if (!body)
                continue;
            // Defensive: push the gameplay layer bits onto the wrapper so the
            // filtered queries always see them (mirrors TFVehiclePhysics).
            body->SetCollisionGroup(CollisionLayers::WorldStatic);
            body->SetCollisionMask(CollisionLayers::All);
            m_bodies.push_back(std::move(body));
            ++created;
        }

        // Bulk static-body creation done — compact the Jolt broadphase once so
        // the first movement sweeps don't pay the unsorted-tree cost.
        m_physics->OptimizeBroadPhase();

        SPARK_LOG_INFO(Spark::LogCategory::Game,
                       "[TF] world collision: %zu static bodies from %s (terrain stays analytic)", created,
                       scenePath.c_str());
        return created > 0;
    }

    void TFWorldCollision::Shutdown()
    {
        if (m_physics)
        {
            for (auto& body : m_bodies)
            {
                if (body)
                    m_physics->RemoveBody(body);
            }
        }
        m_bodies.clear();
        m_physics = nullptr;
    }

    // -----------------------------------------------------------------------
    // Shared move resolver (server integrator + client prediction)
    // -----------------------------------------------------------------------

    void TFWorldCollision::ResolveMove(const float prevPos[3], float pos[3], float vel[3]) const
    {
        if (!IsActive())
            return;

        float remX = pos[0] - prevPos[0];
        float remZ = pos[2] - prevPos[2];
        if (remX * remX + remZ * remZ < 1.0e-10f)
            return;

        // Swept capsule: bottom lifted kTFStepUpM above the feet so standing on
        // plateau-flush mesa tops (and stepping over low curbs) never registers
        // as a wall hit; top at the pawn head. CapsuleCastFiltered's `height` is
        // the cylindrical section (total = height + 2 * radius).
        const float capTotal = kTFPawnHeightM - kTFStepUpM;          // 1.45
        const float capCyl = capTotal - 2.0f * kTFPawnRadiusM;       // 0.65
        const float centerY = pos[1] + kTFStepUpM + capTotal * 0.5f; // feet + 1.075

        float curX = prevPos[0];
        float curZ = prevPos[2];

        for (int iter = 0; iter < kTFSlideIters; ++iter)
        {
            const float len = std::sqrt(remX * remX + remZ * remZ);
            if (len < 1.0e-5f)
                break;

            const XMFLOAT3 from{curX, centerY, curZ};
            const XMFLOAT3 to{curX + remX, centerY, curZ + remZ};
            const RaycastHit hit =
                m_physics->CapsuleCastFiltered(kTFPawnRadiusM, capCyl, from, to, CollisionLayers::MovementMask);
            if (!hit.hasHit)
            {
                curX += remX;
                curZ += remZ;
                break;
            }

            const float dirX = remX / len;
            const float dirZ = remZ / len;
            const float travel = std::clamp(hit.distance - kTFMoveSkinM, 0.0f, len);
            curX += dirX * travel;
            curZ += dirZ * travel;

            // Slide the remainder along the surface (horizontal normal only;
            // shape-cast normals are true outward unit normals as of today).
            float nX = hit.normal.x;
            float nZ = hit.normal.z;
            const float nLen = std::sqrt(nX * nX + nZ * nZ);
            if (nLen < 1.0e-4f)
                break; // floor/ceiling contact: nothing to slide along
            nX /= nLen;
            nZ /= nLen;

            float leftX = dirX * (len - travel);
            float leftZ = dirZ * (len - travel);
            const float into = leftX * nX + leftZ * nZ;
            if (into < 0.0f)
            {
                leftX -= into * nX;
                leftZ -= into * nZ;
            }
            remX = leftX;
            remZ = leftZ;

            // Kill the velocity component pushing into the surface so the next
            // tick does not re-accelerate into the wall.
            const float vInto = vel[0] * nX + vel[2] * nZ;
            if (vInto < 0.0f)
            {
                vel[0] -= vInto * nX;
                vel[2] -= vInto * nZ;
            }
        }

        pos[0] = curX;
        pos[2] = curZ;
    }

} // namespace Terrafront

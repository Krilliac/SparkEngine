/**
 * @file RacingVehicleSystem.cpp
 * @brief Racing vehicle roster, driver input latching, the shared fixed step, and persistence restore
 *
 * The Jolt chassis itself (construction, per-tick drive forces, pose read-back) lives in
 * RacingVehicleChassis.cpp.
 */

#include "RacingVehicleSystem.h"
#include "Physics/PhysicsBody.h"
#include "Physics/PhysicsSystem.h"
#include "Physics/VehiclePhysics.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

namespace Racing
{
    RacingVehicleSystem::RacingVehicleSystem() = default;

    RacingVehicleSystem::~RacingVehicleSystem()
    {
        Shutdown();
    }

    bool RacingVehicleSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        PhysicsSystem* physics = context ? context->GetPhysics() : nullptr;
        if (!physics || !physics->GetJoltSystem())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "Racing vehicle system needs the engine's live Jolt PhysicsSystem; none is available");
            Spark::SimpleConsole::GetInstance().LogError(
                "[Racing Vehicle] No live Jolt physics world: Racing vehicles cannot be simulated");
            return false;
        }

        m_physics = physics;
        m_stepCount = 0;
        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Racing vehicle system initialized on the shared Jolt world");
        Spark::SimpleConsole::GetInstance().LogInfo(
            "[Racing Vehicle] Vehicle system initialized (Jolt vehicles, 6 vehicle types)");
        return true;
    }

    void RacingVehicleSystem::Update(float deltaTime)
    {
        if (!m_initialized || !std::isfinite(deltaTime) || deltaTime <= 0.0f)
            return;

        for (auto& vehicle : m_vehicles)
        {
            if (!vehicle.isActive)
                continue;
            if (vehicle.boostTimer > 0.0f)
                vehicle.boostTimer = std::max(0.0f, vehicle.boostTimer - deltaTime);
            vehicle.nitro = std::clamp(vehicle.nitro, 0.0f, 1.0f);
        }
    }

    void RacingVehicleSystem::FixedUpdate(float fixedDeltaTime)
    {
        if (!m_initialized || !m_physics || !std::isfinite(fixedDeltaTime) || fixedDeltaTime <= 0.0f)
            return;

        for (auto& vehicle : m_vehicles)
        {
            auto it = m_chassis.find(vehicle.id);
            if (vehicle.isActive && it != m_chassis.end())
                DriveChassis(vehicle, it->second);
        }

        // The Racing process's single physics step (module-driven stepping contract): one tick of the
        // engine fixed timestep, so the world advances in lockstep with the module's fixed update.
        if (m_physics->GetTimeStep() != fixedDeltaTime)
            m_physics->SetTimeStep(fixedDeltaTime);
        m_stepCount += m_physics->StepFixed(1, 1.0f);

        for (auto& vehicle : m_vehicles)
        {
            auto it = m_chassis.find(vehicle.id);
            if (vehicle.isActive && it != m_chassis.end())
                ReadBackChassis(vehicle, it->second, fixedDeltaTime);
        }
    }

    void RacingVehicleSystem::Shutdown()
    {
        // Constraints go before their bodies, and both before the engine tears the world down.
        for (auto& [id, chassis] : m_chassis)
        {
            chassis.vehicle.reset();
            if (m_physics && chassis.body)
                m_physics->RemoveBody(chassis.body);
        }
        m_chassis.clear();
        m_vehicles.clear();
        m_nextId = 1;
        m_physics = nullptr;
        m_initialized = false;
    }

    uint32_t RacingVehicleSystem::CreateVehicle(const std::string& name, VehicleType type, bool isPlayer,
                                                const VehiclePose& pose)
    {
        if (!m_initialized)
            return 0;

        VehicleInstance vehicle{};
        vehicle.id = m_nextId++;
        vehicle.name = name;
        vehicle.type = type;
        vehicle.baseStats = GetDefaultStats(type);
        vehicle.isPlayer = isPlayer;
        if (!BuildChassis(vehicle, pose, 0.0f))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "Racing vehicle %s: Jolt chassis creation failed", name.c_str());
            return 0;
        }
        m_vehicles.push_back(vehicle);
        SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Racing vehicle created: %s (id=%u, player=%s)", name.c_str(),
                        vehicle.id, isPlayer ? "yes" : "no");
        return vehicle.id;
    }

    void RacingVehicleSystem::RemoveVehicle(uint32_t id)
    {
        DestroyChassis(id);
        auto it =
            std::find_if(m_vehicles.begin(), m_vehicles.end(), [id](const VehicleInstance& v) { return v.id == id; });
        if (it != m_vehicles.end())
            m_vehicles.erase(it);
    }

    void RacingVehicleSystem::ApplyInput(float throttle, float brake, float steer, bool nitroPressed, bool driftPressed,
                                         float deltaTime)
    {
        VehicleInstance* player = GetPlayerVehicle();
        if (!player)
            return;

        ApplyInputInternal(*player, throttle, brake, steer, nitroPressed, driftPressed, deltaTime);
    }

    void RacingVehicleSystem::ApplyInputToVehicle(uint32_t vehicleId, float throttle, float brake, float steer,
                                                  bool nitroPressed, bool driftPressed, float deltaTime)
    {
        VehicleInstance* vehicle = GetVehicle(vehicleId);
        if (!vehicle || !vehicle->isActive)
            return;

        ApplyInputInternal(*vehicle, throttle, brake, steer, nitroPressed, driftPressed, deltaTime);
    }

    void RacingVehicleSystem::NeutralizeVehicle(uint32_t vehicleId)
    {
        VehicleInstance* vehicle = GetVehicle(vehicleId);
        if (!vehicle)
            return;

        DestroyChassis(vehicleId);
        vehicle->speed = 0.0f;
        vehicle->rpm = 0.0f;
        vehicle->steerAngle = 0.0f;
        vehicle->throttleInput = 0.0f;
        vehicle->brakeInput = 0.0f;
        vehicle->driftState = DriftState::None;
        vehicle->driftCharge = 0.0f;
        vehicle->boostTimer = 0.0f;
        vehicle->strandedTime = 0.0f;
    }

    bool RacingVehicleSystem::HasChassis(uint32_t vehicleId) const
    {
        return m_chassis.find(vehicleId) != m_chassis.end();
    }

    void RacingVehicleSystem::ApplyInputInternal(VehicleInstance& vehicle, float throttle, float brake, float steer,
                                                 bool nitroPressed, bool driftPressed, float deltaTime)
    {
        if (!std::isfinite(deltaTime) || deltaTime <= 0.0f)
            return;
        deltaTime = std::min(deltaTime, 0.1f);

        // Latch the command; the next fixed ticks hand it to the Jolt controller.
        vehicle.throttleInput = std::isfinite(throttle) ? std::clamp(throttle, 0.0f, 1.0f) : 0.0f;
        vehicle.brakeInput = std::isfinite(brake) ? std::clamp(brake, 0.0f, 1.0f) : 0.0f;
        steer = std::isfinite(steer) ? std::clamp(steer, -1.0f, 1.0f) : 0.0f;
        vehicle.steerAngle = steer;

        if (nitroPressed && vehicle.nitro > 0.0f)
        {
            vehicle.nitro = std::max(0.0f, vehicle.nitro - 0.6f * deltaTime);
            vehicle.boostTimer = std::max(vehicle.boostTimer, 0.5f);
        }

        UpdateDriftState(vehicle, steer, driftPressed, deltaTime);
    }

    VehicleInstance* RacingVehicleSystem::GetVehicle(uint32_t id)
    {
        auto it =
            std::find_if(m_vehicles.begin(), m_vehicles.end(), [id](const VehicleInstance& v) { return v.id == id; });
        return it != m_vehicles.end() ? &(*it) : nullptr;
    }

    const VehicleInstance* RacingVehicleSystem::GetVehicle(uint32_t id) const
    {
        auto it =
            std::find_if(m_vehicles.begin(), m_vehicles.end(), [id](const VehicleInstance& v) { return v.id == id; });
        return it != m_vehicles.end() ? &(*it) : nullptr;
    }

    bool RacingVehicleSystem::ValidateSnapshot(const std::vector<VehicleInstance>& vehicles)
    {
        std::unordered_set<uint32_t> ids;
        ids.reserve(vehicles.size());
        size_t playerCount = 0;
        for (const VehicleInstance& vehicle : vehicles)
        {
            const VehicleStats& stats = vehicle.baseStats;
            if (vehicle.id == 0 || vehicle.id == std::numeric_limits<uint32_t>::max() || vehicle.name.empty() ||
                vehicle.name.size() > 256 || vehicle.type >= VehicleType::Count ||
                vehicle.driftState >= DriftState::Count || vehicle.currentSurface >= SurfaceType::Count ||
                !std::isfinite(stats.maxSpeed) || !std::isfinite(stats.acceleration) ||
                !std::isfinite(stats.handling) || !std::isfinite(stats.braking) || !std::isfinite(stats.weight) ||
                !std::isfinite(stats.durability) || !std::isfinite(stats.driftBonus) || stats.maxSpeed <= 0.0f ||
                stats.acceleration < 0.0f || stats.handling < 0.0f || stats.braking < 0.0f || stats.weight <= 0.0f ||
                stats.durability <= 0.0f || stats.driftBonus < 0.0f || !std::isfinite(vehicle.positionX) ||
                !std::isfinite(vehicle.positionY) || !std::isfinite(vehicle.positionZ) ||
                !std::isfinite(vehicle.heading) || !std::isfinite(vehicle.speed) || !std::isfinite(vehicle.rpm) ||
                !std::isfinite(vehicle.steerAngle) || !std::isfinite(vehicle.driftCharge) ||
                !std::isfinite(vehicle.boostTimer) || !std::isfinite(vehicle.nitro) || !std::isfinite(vehicle.damage) ||
                vehicle.speed < 0.0f || vehicle.rpm < 0.0f || vehicle.driftCharge < 0.0f ||
                vehicle.driftCharge > 1.0f || vehicle.boostTimer < 0.0f || vehicle.nitro < 0.0f ||
                vehicle.nitro > 1.0f || vehicle.damage < 0.0f || !ids.insert(vehicle.id).second)
            {
                return false;
            }
            playerCount += vehicle.isPlayer ? 1u : 0u;
            if (playerCount > 1)
                return false;
        }
        return true;
    }

    bool RacingVehicleSystem::RestoreState(const std::vector<VehicleInstance>& vehicles)
    {
        if (!m_initialized || !ValidateSnapshot(vehicles))
            return false;

        while (!m_chassis.empty())
            DestroyChassis(m_chassis.begin()->first);

        m_vehicles = vehicles;
        uint32_t nextId = 1;
        for (VehicleInstance& vehicle : m_vehicles)
        {
            nextId = std::max(nextId, vehicle.id + 1);
            vehicle.throttleInput = 0.0f;
            vehicle.brakeInput = 0.0f;
            vehicle.strandedTime = 0.0f;
            if (!vehicle.isActive)
                continue;
            const VehiclePose pose{vehicle.positionX, vehicle.positionY, vehicle.positionZ, vehicle.heading};
            if (!BuildChassis(vehicle, pose, vehicle.speed))
            {
                while (!m_chassis.empty())
                    DestroyChassis(m_chassis.begin()->first);
                m_vehicles.clear();
                return false;
            }
        }
        m_nextId = nextId;
        return true;
    }

    VehicleInstance* RacingVehicleSystem::GetPlayerVehicle()
    {
        auto it =
            std::find_if(m_vehicles.begin(), m_vehicles.end(), [](const VehicleInstance& v) { return v.isPlayer; });
        return it != m_vehicles.end() ? &(*it) : nullptr;
    }

    VehicleStats RacingVehicleSystem::GetDefaultStats(VehicleType type)
    {
        switch (type)
        {
        case VehicleType::SportsCar:
            return {220.0f, 0.75f, 0.80f, 0.75f, 1300.0f, 100.0f, 1.0f};
        case VehicleType::MuscleCar:
            return {240.0f, 0.70f, 0.55f, 0.60f, 1600.0f, 120.0f, 0.8f};
        case VehicleType::SuperCar:
            return {300.0f, 0.85f, 0.75f, 0.80f, 1200.0f, 70.0f, 1.1f};
        case VehicleType::OffRoad:
            return {160.0f, 0.60f, 0.65f, 0.70f, 1800.0f, 150.0f, 0.7f};
        case VehicleType::Formula:
            return {280.0f, 0.90f, 0.95f, 0.90f, 700.0f, 50.0f, 1.2f};
        case VehicleType::Kart:
            return {120.0f, 0.95f, 0.85f, 0.65f, 300.0f, 60.0f, 1.5f};
        default:
            return {};
        }
    }

    float RacingVehicleSystem::GetSurfaceGrip(SurfaceType surface)
    {
        switch (surface)
        {
        case SurfaceType::Asphalt:
            return 1.0f;
        case SurfaceType::Dirt:
            return 0.65f;
        case SurfaceType::Grass:
            return 0.45f;
        case SurfaceType::Sand:
            return 0.35f;
        case SurfaceType::Ice:
            return 0.15f;
        case SurfaceType::Gravel:
            return 0.55f;
        default:
            return 1.0f;
        }
    }

    std::string RacingVehicleSystem::GetVehicleListString() const
    {
        std::string result = "Vehicles (" + std::to_string(m_vehicles.size()) + "):\n";
        for (const auto& v : m_vehicles)
        {
            result += "  [" + std::to_string(v.id) + "] " + v.name;
            result += " | Speed: " + std::to_string(static_cast<int>(v.speed)) + " km/h";
            result += " | Damage: " + std::to_string(static_cast<int>(v.damage));
            if (v.isPlayer)
                result += " (PLAYER)";
            result += "\n";
        }
        return result;
    }

    void RacingVehicleSystem::UpdateDriftState(VehicleInstance& vehicle, float steer, bool driftPressed, float dt)
    {
        float absSteer = std::abs(steer);

        switch (vehicle.driftState)
        {
        case DriftState::None:
            if (driftPressed && absSteer > 0.5f && vehicle.speed > 60.0f)
                vehicle.driftState = DriftState::Initiating;
            break;

        case DriftState::Initiating:
            vehicle.driftState = DriftState::Drifting;
            vehicle.driftCharge = 0.0f;
            break;

        case DriftState::Drifting:
            // Charge boost while drifting
            vehicle.driftCharge += vehicle.baseStats.driftBonus * dt;
            vehicle.driftCharge = std::min(vehicle.driftCharge, 1.0f);

            if (!driftPressed || vehicle.speed < 30.0f)
            {
                // Release drift — apply boost proportional to charge
                if (vehicle.driftCharge > 0.3f)
                {
                    vehicle.boostTimer = vehicle.driftCharge * 2.0f;
                    vehicle.driftState = DriftState::Boosting;
                }
                else
                {
                    vehicle.driftState = DriftState::None;
                }
                vehicle.driftCharge = 0.0f;
            }
            break;

        case DriftState::Boosting:
            if (vehicle.boostTimer <= 0.0f)
                vehicle.driftState = DriftState::None;
            break;

        case DriftState::Count:
            break;
        }
    }

    void RacingVehicleSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!ImGui::CollapsingHeader("Racing Vehicles"))
            return;

        ImGui::Text("Active Vehicles: %zu | Jolt chassis: %zu | Physics ticks: %llu", m_vehicles.size(),
                    m_chassis.size(), static_cast<unsigned long long>(m_stepCount));
        ImGui::Separator();

        for (const auto& v : m_vehicles)
        {
            std::string label = v.name + (v.isPlayer ? " (PLAYER)" : "");
            if (ImGui::TreeNode(label.c_str()))
            {
                ImGui::Text("Speed: %.1f km/h", v.speed);
                ImGui::Text("RPM: %.0f", v.rpm);
                ImGui::Text("Position: (%.1f, %.1f, %.1f)", v.positionX, v.positionY, v.positionZ);
                ImGui::Text("Heading: %.2f rad", v.heading);
                ImGui::Text("Damage: %.1f / %.1f", v.damage, v.baseStats.durability);
                ImGui::Text("Nitro: %.0f%%", v.nitro * 100.0f);
                ImGui::Text("Drift Charge: %.0f%%", v.driftCharge * 100.0f);
                ImGui::TreePop();
            }
        }
#endif
    }

} // namespace Racing

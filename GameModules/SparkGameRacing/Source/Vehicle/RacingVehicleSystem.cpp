/**
 * @file RacingVehicleSystem.cpp
 * @brief Physics-based vehicle driving model
 */

#include "RacingVehicleSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

namespace Racing
{

    bool RacingVehicleSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        m_initialized = true;

        auto& console = Spark::SimpleConsole::GetInstance();
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Racing vehicle system initialized (6 vehicle types)");
        console.LogInfo("[Racing Vehicle] Vehicle system initialized (6 vehicle types)");
        return true;
    }

    void RacingVehicleSystem::Update(float deltaTime)
    {
        if (!m_initialized)
            return;

        for (auto& vehicle : m_vehicles)
        {
            if (vehicle.isActive)
                UpdateVehiclePhysics(vehicle, deltaTime);
        }
    }

    void RacingVehicleSystem::FixedUpdate(float fixedDeltaTime)
    {
        if (!m_initialized)
            return;

        // Fixed-step physics integration for deterministic behavior
        for (auto& vehicle : m_vehicles)
        {
            if (!vehicle.isActive)
                continue;

            // Apply surface-dependent grip to steering
            float grip = GetSurfaceGrip(vehicle.currentSurface);
            float effectiveHandling = vehicle.baseStats.handling * grip;

            // Integrate position from speed and heading
            float speedMs = vehicle.speed / 3.6f; // km/h to m/s
            vehicle.positionX += std::sin(vehicle.heading) * speedMs * fixedDeltaTime;
            vehicle.positionZ += std::cos(vehicle.heading) * speedMs * fixedDeltaTime;

            // Apply steering (yaw rate inversely proportional to speed for stability)
            float yawRate = vehicle.steerAngle * effectiveHandling * 2.0f;
            if (vehicle.speed > 10.0f)
                yawRate *= 50.0f / vehicle.speed; // Reduce turn rate at high speed
            vehicle.heading += yawRate * fixedDeltaTime;
        }
    }

    void RacingVehicleSystem::Shutdown()
    {
        m_vehicles.clear();
        m_nextId = 1;
        m_initialized = false;
    }

    uint32_t RacingVehicleSystem::CreateVehicle(const std::string& name, VehicleType type, bool isPlayer)
    {
        VehicleInstance vehicle{};
        vehicle.id = m_nextId++;
        vehicle.name = name;
        vehicle.type = type;
        vehicle.baseStats = GetDefaultStats(type);
        vehicle.isPlayer = isPlayer;
        m_vehicles.push_back(vehicle);
        SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Racing vehicle created: %s (id=%u, player=%s)", name.c_str(),
                        vehicle.id, isPlayer ? "yes" : "no");
        return vehicle.id;
    }

    void RacingVehicleSystem::RemoveVehicle(uint32_t id)
    {
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

        vehicle->speed = 0.0f;
        vehicle->rpm = 0.0f;
        vehicle->steerAngle = 0.0f;
        vehicle->driftState = DriftState::None;
        vehicle->driftCharge = 0.0f;
        vehicle->boostTimer = 0.0f;
    }

    void RacingVehicleSystem::ApplyInputInternal(VehicleInstance& vehicle, float throttle, float brake, float steer,
                                                 bool nitroPressed, bool driftPressed, float deltaTime)
    {
        if (!std::isfinite(deltaTime) || deltaTime <= 0.0f)
            return;
        deltaTime = std::min(deltaTime, 0.1f);

        // Clamp inputs
        throttle = std::clamp(throttle, 0.0f, 1.0f);
        brake = std::clamp(brake, 0.0f, 1.0f);
        steer = std::clamp(steer, -1.0f, 1.0f);

        vehicle.steerAngle = steer;

        // Acceleration
        float accelForce = throttle * vehicle.baseStats.acceleration * 100.0f;
        float brakeForce = brake * vehicle.baseStats.braking * 150.0f;
        float drag = vehicle.speed * 0.5f;

        // Damage reduces effective acceleration
        const float durability = std::max(vehicle.baseStats.durability, 1.0f);
        const float damagePenalty = 1.0f - std::min(vehicle.damage / durability, 0.5f);
        accelForce *= damagePenalty;

        // Boost from nitro or drift
        float boostMultiplier = 1.0f;
        if (vehicle.boostTimer > 0.0f)
            boostMultiplier = 1.3f;

        vehicle.speed += (accelForce * boostMultiplier - brakeForce - drag) * deltaTime;
        vehicle.speed = std::clamp(vehicle.speed, 0.0f, vehicle.baseStats.maxSpeed * boostMultiplier);

        // Nitro
        if (nitroPressed && vehicle.nitro > 0.0f)
        {
            vehicle.nitro = std::max(0.0f, vehicle.nitro - 0.6f * deltaTime);
            vehicle.boostTimer = 0.5f;
        }

        // RPM mapping (simplified: proportional to speed fraction)
        const float maxSpeed = std::max(vehicle.baseStats.maxSpeed, 1.0f);
        vehicle.rpm = (vehicle.speed / maxSpeed) * 8000.0f;

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

    void RacingVehicleSystem::UpdateVehiclePhysics(VehicleInstance& vehicle, float dt)
    {
        // Decay boost timer
        if (vehicle.boostTimer > 0.0f)
            vehicle.boostTimer = std::max(0.0f, vehicle.boostTimer - dt);

        // Natural speed decay from drag (non-player vehicles rely on AI input)
        if (!vehicle.isPlayer)
        {
            float drag = vehicle.speed * 0.3f * dt;
            vehicle.speed = std::max(0.0f, vehicle.speed - drag);
        }

        // Clamp nitro
        vehicle.nitro = std::clamp(vehicle.nitro, 0.0f, 1.0f);
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

    void RacingVehicleSystem::ApplyDamage(VehicleInstance& vehicle, float amount)
    {
        vehicle.damage += amount;
        vehicle.damage = std::min(vehicle.damage, vehicle.baseStats.durability);
    }

    void RacingVehicleSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!ImGui::CollapsingHeader("Racing Vehicles"))
            return;

        ImGui::Text("Active Vehicles: %zu", m_vehicles.size());
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

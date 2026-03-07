#include "Core/Platform.h"
#include "VehicleSystem.h"
#include "Player.h"
#include "Input/InputManager.h"
#include "Utils/MathUtils.h"
#include "Utils/Assert.h"
#include <algorithm>
#include <cmath>
#include <sstream>

using namespace DirectX;

namespace Spark {

// ============================================================================
// Vehicle Implementation
// ============================================================================

Vehicle::Vehicle()
    : m_definition{}
{
    SetName("Vehicle");
    m_definition.name = "Default Vehicle";
}

Vehicle::Vehicle(const VehicleDefinition& def)
    : m_definition(def)
    , m_health(def.maxHealth)
    , m_armor(def.maxArmor)
    , m_seats(def.seats)
{
    SetName(def.name);
}

HRESULT Vehicle::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    return GameObject::Initialize(device, context);
}

void Vehicle::Update(float dt)
{
    if (IsDestroyed()) return;

    if (m_definition.isAerial) {
        UpdateAerialVehicle(dt);
    } else {
        UpdateGroundVehicle(dt);
        ApplyGravity(dt);
        CheckGroundCollision();
    }

    UpdateWeapons(dt);

    // Apply velocity
    XMFLOAT3 pos = GetPosition();
    pos.x += m_velocity.x * dt;
    pos.y += m_velocity.y * dt;
    pos.z += m_velocity.z * dt;
    SetPosition(pos);

    // Update seated player positions
    for (size_t i = 0; i < m_seats.size(); ++i) {
        if (m_seats[i].isOccupied && m_seats[i].occupant) {
            XMFLOAT3 seatWorldPos = {
                pos.x + m_seats[i].localOffset.x,
                pos.y + m_seats[i].localOffset.y,
                pos.z + m_seats[i].localOffset.z
            };
            m_seats[i].occupant->SetPosition(seatWorldPos);
        }
    }

    // Engine sound
    m_engineSoundTimer += dt;

    GameObject::Update(dt);
}

void Vehicle::Render(const XMMATRIX& view, const XMMATRIX& proj)
{
    if (IsDestroyed()) return;
    GameObject::Render(view, proj);
}

void Vehicle::OnHit(GameObject* target)
{
    // Vehicle collision with another object
    TakeDamage(5.0f);
}

void Vehicle::OnHitWorld(const XMFLOAT3& hitPoint, const XMFLOAT3& normal)
{
    // Bounce/stop on world collision
    m_velocity.x *= 0.5f;
    m_velocity.z *= 0.5f;
}

void Vehicle::SetThrottle(float throttle)
{
    m_throttle = std::max(-1.0f, std::min(1.0f, throttle));
}

void Vehicle::SetSteering(float steering)
{
    m_steering = std::max(-1.0f, std::min(1.0f, steering));
}

void Vehicle::SetBrake(bool braking)
{
    m_braking = braking;
}

void Vehicle::SetVerticalThrust(float thrust)
{
    if (m_definition.isAerial) {
        m_verticalThrust = std::max(-1.0f, std::min(1.0f, thrust));
    }
}

void Vehicle::FireWeapon(int seatIndex)
{
    if (seatIndex < 0 || seatIndex >= (int)m_seats.size()) return;
    if (m_seats[seatIndex].role != VehicleSeatRole::GUNNER &&
        m_seats[seatIndex].role != VehicleSeatRole::DRIVER) return;
    if (m_weaponCooldown > 0.0f) return;

    m_weaponCooldown = 1.0f / m_seats[seatIndex].weaponFireRate;
    // Projectile spawning would be done through the projectile pool
}

bool Vehicle::EnterSeat(Player* player, int seatIndex)
{
    if (!player || IsDestroyed()) return false;

    // If no specific seat, find the first available
    if (seatIndex < 0) {
        seatIndex = GetFirstAvailableSeat();
    }

    if (seatIndex < 0 || seatIndex >= (int)m_seats.size()) return false;
    if (m_seats[seatIndex].isOccupied) return false;

    m_seats[seatIndex].occupant = player;
    m_seats[seatIndex].isOccupied = true;

    return true;
}

bool Vehicle::ExitSeat(Player* player)
{
    if (!player) return false;

    for (auto& seat : m_seats) {
        if (seat.occupant == player) {
            seat.occupant = nullptr;
            seat.isOccupied = false;
            return true;
        }
    }
    return false;
}

int Vehicle::GetFirstAvailableSeat() const
{
    // Prefer driver seat first
    int driverSeat = GetDriverSeatIndex();
    if (driverSeat >= 0 && !m_seats[driverSeat].isOccupied) {
        return driverSeat;
    }

    // Then any other seat
    for (int i = 0; i < (int)m_seats.size(); ++i) {
        if (!m_seats[i].isOccupied) return i;
    }
    return -1;
}

int Vehicle::GetDriverSeatIndex() const
{
    for (int i = 0; i < (int)m_seats.size(); ++i) {
        if (m_seats[i].role == VehicleSeatRole::DRIVER ||
            m_seats[i].role == VehicleSeatRole::PILOT) {
            return i;
        }
    }
    return -1;
}

XMFLOAT3 Vehicle::GetExitPosition(int seatIndex) const
{
    XMFLOAT3 pos = GetPosition();
    if (seatIndex >= 0 && seatIndex < (int)m_seats.size()) {
        // Exit to the side of the vehicle
        float exitSide = (seatIndex % 2 == 0) ? -1.0f : 1.0f;
        XMFLOAT3 right = GetRight();
        pos.x += right.x * m_definition.dimensions.x * exitSide;
        pos.y += 1.0f; // Slightly above ground
        pos.z += right.z * m_definition.dimensions.x * exitSide;
    }
    return pos;
}

bool Vehicle::IsOccupied() const
{
    for (const auto& seat : m_seats) {
        if (seat.isOccupied) return true;
    }
    return false;
}

bool Vehicle::ContainsPlayer(const Player* player) const
{
    if (!player) return false;
    for (const auto& seat : m_seats) {
        if (seat.occupant == player) return true;
    }
    return false;
}

int Vehicle::GetPlayerSeatIndex(const Player* player) const
{
    if (!player) return -1;
    for (int i = 0; i < (int)m_seats.size(); ++i) {
        if (m_seats[i].occupant == player) return i;
    }
    return -1;
}

void Vehicle::TakeDamage(float damage)
{
    if (IsDestroyed()) return;

    // Armor absorbs some damage
    float armorAbsorbed = std::min(damage * 0.3f, m_armor);
    m_armor -= armorAbsorbed;
    float remaining = damage - armorAbsorbed;

    m_health = std::max(0.0f, m_health - remaining);

    if (m_health <= 0.0f) {
        Explode();
    }
}

void Vehicle::Repair(float amount)
{
    m_health = std::min(m_definition.maxHealth, m_health + amount);
}

void Vehicle::CreateMesh()
{
    // Create a box mesh scaled to vehicle dimensions
    if (m_mesh) {
        XMFLOAT3 dims = m_definition.dimensions;
        m_mesh->CreateCube(1.0f);
        SetScale(dims);
    }
}

void Vehicle::UpdateGroundVehicle(float dt)
{
    const auto& handling = m_definition.handling;

    // Braking
    if (m_braking) {
        float brakeDecel = handling.brakeForce * dt;
        if (m_currentSpeed > 0) {
            m_currentSpeed = std::max(0.0f, m_currentSpeed - brakeDecel);
        } else {
            m_currentSpeed = std::min(0.0f, m_currentSpeed + brakeDecel);
        }
    }

    // Acceleration
    if (m_throttle > 0) {
        float maxSpd = handling.maxSpeed;
        m_currentSpeed = std::min(maxSpd, m_currentSpeed + handling.acceleration * m_throttle * dt);
    } else if (m_throttle < 0) {
        float maxRev = -handling.reverseSpeed;
        m_currentSpeed = std::max(maxRev, m_currentSpeed + handling.acceleration * m_throttle * dt);
    }

    // Friction
    if (std::abs(m_throttle) < 0.01f && !m_braking) {
        m_currentSpeed *= handling.friction;
        if (std::abs(m_currentSpeed) < 0.1f) m_currentSpeed = 0.0f;
    }

    // Steering (only when moving)
    if (std::abs(m_currentSpeed) > 0.5f) {
        float turnAmount = m_steering * handling.turnRate * dt;
        // Reduce turning at high speed
        float speedFactor = 1.0f - (std::abs(m_currentSpeed) / handling.maxSpeed) * 0.5f;
        turnAmount *= speedFactor;

        XMFLOAT3 rot = GetRotation();
        rot.y += turnAmount;
        SetRotation(rot);
    }

    // Apply speed to velocity
    XMFLOAT3 forward = GetForward();
    m_velocity.x = forward.x * m_currentSpeed;
    m_velocity.z = forward.z * m_currentSpeed;
}

void Vehicle::UpdateAerialVehicle(float dt)
{
    const auto& handling = m_definition.handling;

    // Forward/backward thrust
    if (m_throttle > 0) {
        m_currentSpeed = std::min(handling.maxSpeed,
                                  m_currentSpeed + handling.acceleration * m_throttle * dt);
    } else if (m_throttle < 0) {
        m_currentSpeed = std::max(-handling.reverseSpeed,
                                  m_currentSpeed + handling.acceleration * m_throttle * dt);
    } else {
        m_currentSpeed *= 0.98f; // Air drag
    }

    // Vertical thrust
    if (m_verticalThrust > 0) {
        m_velocity.y += handling.liftForce * m_verticalThrust * dt;
    } else if (m_verticalThrust < 0) {
        m_velocity.y += handling.liftForce * m_verticalThrust * dt * 0.5f;
    }

    // Auto-hover stability
    if (std::abs(m_verticalThrust) < 0.1f) {
        m_velocity.y *= handling.hoverStability;
    }

    // Altitude limit
    XMFLOAT3 pos = GetPosition();
    if (pos.y > handling.maxAltitude) {
        m_velocity.y = std::min(0.0f, m_velocity.y);
    }

    // Yaw steering
    XMFLOAT3 rot = GetRotation();
    rot.y += m_steering * handling.turnRate * dt;

    // Pitch from throttle
    float targetPitch = -m_throttle * 0.3f;
    m_pitch += (targetPitch - m_pitch) * handling.pitchRate * dt;
    rot.x = m_pitch;

    // Roll from steering
    float targetRoll = -m_steering * 0.4f;
    m_roll += (targetRoll - m_roll) * handling.rollRate * dt;
    rot.z = m_roll;

    SetRotation(rot);

    // Apply speed to velocity
    XMFLOAT3 forward = GetForward();
    m_velocity.x = forward.x * m_currentSpeed;
    m_velocity.z = forward.z * m_currentSpeed;

    // Gentle gravity pull for aerial (not full gravity)
    m_velocity.y -= 2.0f * dt;
}

void Vehicle::UpdateWeapons(float dt)
{
    if (m_weaponCooldown > 0.0f) {
        m_weaponCooldown -= dt;
    }
}

void Vehicle::ApplyGravity(float dt)
{
    m_velocity.y -= 20.0f * dt;
    if (m_velocity.y < -50.0f) m_velocity.y = -50.0f;
}

void Vehicle::CheckGroundCollision()
{
    XMFLOAT3 pos = GetPosition();
    if (pos.y <= 0.0f && m_velocity.y <= 0.0f) {
        pos.y = 0.0f;
        SetPosition(pos);
        m_velocity.y = 0.0f;
    }
}

void Vehicle::Explode()
{
    m_health = 0.0f;
    SetActive(false);
    SetVisible(false);

    // Eject all occupants
    for (auto& seat : m_seats) {
        if (seat.isOccupied && seat.occupant) {
            XMFLOAT3 ejectPos = GetExitPosition(0);
            ejectPos.y += 2.0f; // Launch upward
            seat.occupant->SetPosition(ejectPos);
            seat.occupant->TakeDamage(m_definition.explosionDamage * 0.5f);
            seat.occupant = nullptr;
            seat.isOccupied = false;
        }
    }

    if (m_destructionCallback) {
        m_destructionCallback(this);
    }
}

// ============================================================================
// VehicleSystem Implementation
// ============================================================================

VehicleSystem::VehicleSystem()
{
}

bool VehicleSystem::Initialize()
{
    InitVehicleDefinitions();
    return true;
}

void VehicleSystem::Update(float deltaTime)
{
    // Remove destroyed vehicles after a delay
    m_vehicles.erase(
        std::remove_if(m_vehicles.begin(), m_vehicles.end(),
            [](const std::unique_ptr<Vehicle>& v) {
                return v->IsDestroyed() && !v->IsActive();
            }),
        m_vehicles.end());

    for (auto& vehicle : m_vehicles) {
        if (vehicle && vehicle->IsActive()) {
            vehicle->Update(deltaTime);
        }
    }
}

const VehicleDefinition& VehicleSystem::GetDefinition(VehicleType type) const
{
    auto it = m_definitions.find(static_cast<int>(type));
    if (it != m_definitions.end()) {
        return it->second;
    }
    return m_defaultDefinition;
}

Vehicle* VehicleSystem::SpawnVehicle(VehicleType type, const XMFLOAT3& position,
                                      ID3D11Device* device, ID3D11DeviceContext* context)
{
    if ((int)m_vehicles.size() >= MAX_VEHICLES) return nullptr;

    const auto& def = GetDefinition(type);
    auto vehicle = std::make_unique<Vehicle>(def);
    vehicle->Initialize(device, context);
    vehicle->SetPosition(position);

    Vehicle* ptr = vehicle.get();
    m_vehicles.push_back(std::move(vehicle));
    return ptr;
}

void VehicleSystem::RemoveVehicle(Vehicle* vehicle)
{
    m_vehicles.erase(
        std::remove_if(m_vehicles.begin(), m_vehicles.end(),
            [vehicle](const std::unique_ptr<Vehicle>& v) {
                return v.get() == vehicle;
            }),
        m_vehicles.end());
}

Vehicle* VehicleSystem::FindNearestVehicle(const XMFLOAT3& position, float maxRange) const
{
    Vehicle* nearest = nullptr;
    float nearestDist = maxRange;

    for (const auto& vehicle : m_vehicles) {
        if (!vehicle || vehicle->IsDestroyed()) continue;
        float dist = vehicle->GetDistanceFrom(position);
        if (dist < nearestDist) {
            nearestDist = dist;
            nearest = vehicle.get();
        }
    }
    return nearest;
}

std::string VehicleSystem::Console_ListVehicles() const
{
    std::stringstream ss;
    ss << "Active Vehicles (" << m_vehicles.size() << "/" << MAX_VEHICLES << "):\n";
    for (size_t i = 0; i < m_vehicles.size(); ++i) {
        const auto& v = m_vehicles[i];
        ss << "  [" << i << "] " << v->GetVehicleName()
           << " HP:" << v->GetHealth() << "/" << v->GetMaxHealth()
           << " Pos:(" << v->GetPosition().x << "," << v->GetPosition().y
           << "," << v->GetPosition().z << ")"
           << (v->IsOccupied() ? " [OCCUPIED]" : " [EMPTY]")
           << "\n";
    }
    return ss.str();
}

bool VehicleSystem::Console_SpawnVehicle(const std::string& type, float x, float y, float z,
                                          ID3D11Device* device, ID3D11DeviceContext* context)
{
    VehicleType vType = VehicleType::BUGGY;
    if (type == "buggy") vType = VehicleType::BUGGY;
    else if (type == "tank") vType = VehicleType::TANK;
    else if (type == "apc") vType = VehicleType::APC;
    else if (type == "motorcycle") vType = VehicleType::MOTORCYCLE;
    else if (type == "truck") vType = VehicleType::TRUCK;
    else if (type == "helicopter") vType = VehicleType::HELICOPTER;
    else if (type == "jet") vType = VehicleType::JET;
    else if (type == "dropship") vType = VehicleType::DROPSHIP;
    else if (type == "drone") vType = VehicleType::DRONE;
    else return false;

    return SpawnVehicle(vType, {x, y, z}, device, context) != nullptr;
}

void VehicleSystem::InitVehicleDefinitions()
{
    InitBuggy();
    InitTank();
    InitAPC();
    InitMotorcycle();
    InitTruck();
    InitHelicopter();
    InitJet();
    InitDropship();
    InitDrone();
}

void VehicleSystem::InitBuggy()
{
    VehicleDefinition def;
    def.type = VehicleType::BUGGY;
    def.name = "Harasser Buggy";
    def.description = "Fast light attack vehicle with a mounted gun";
    def.maxHealth = 400.0f;
    def.maxArmor = 50.0f;
    def.explosionDamage = 150.0f;
    def.explosionRadius = 8.0f;
    def.dimensions = {2.0f, 1.5f, 3.5f};
    def.isAerial = false;

    def.handling.maxSpeed = 30.0f;
    def.handling.reverseSpeed = 10.0f;
    def.handling.acceleration = 20.0f;
    def.handling.brakeForce = 30.0f;
    def.handling.turnRate = 2.5f;
    def.handling.friction = 0.96f;
    def.handling.mass = 800.0f;

    // Driver seat
    VehicleSeat driver(VehicleSeatRole::DRIVER, {-0.4f, 0.5f, 0.2f});
    def.seats.push_back(driver);

    // Gunner seat (top-mounted gun)
    VehicleSeat gunner(VehicleSeatRole::GUNNER, {0.0f, 1.2f, -0.5f});
    gunner.mountedWeapon = WeaponType::MACHINE_GUN;
    gunner.weaponFireRate = 10.0f;
    gunner.weaponDamage = 15.0f;
    def.seats.push_back(gunner);

    // Rear passenger
    VehicleSeat passenger(VehicleSeatRole::PASSENGER, {0.4f, 0.5f, -1.0f});
    def.seats.push_back(passenger);

    m_definitions[static_cast<int>(VehicleType::BUGGY)] = def;
}

void VehicleSystem::InitTank()
{
    VehicleDefinition def;
    def.type = VehicleType::TANK;
    def.name = "Vanguard MBT";
    def.description = "Heavy main battle tank with powerful cannon";
    def.maxHealth = 2000.0f;
    def.maxArmor = 500.0f;
    def.explosionDamage = 400.0f;
    def.explosionRadius = 15.0f;
    def.dimensions = {3.5f, 2.5f, 6.0f};
    def.isAerial = false;

    def.handling.maxSpeed = 12.0f;
    def.handling.reverseSpeed = 5.0f;
    def.handling.acceleration = 8.0f;
    def.handling.brakeForce = 20.0f;
    def.handling.turnRate = 1.2f;
    def.handling.friction = 0.92f;
    def.handling.mass = 5000.0f;

    VehicleSeat driver(VehicleSeatRole::DRIVER, {0.0f, 1.0f, 1.0f});
    driver.mountedWeapon = WeaponType::RAILGUN;
    driver.weaponFireRate = 0.3f;
    driver.weaponDamage = 200.0f;
    def.seats.push_back(driver);

    VehicleSeat gunner(VehicleSeatRole::GUNNER, {0.0f, 2.0f, -0.5f});
    gunner.mountedWeapon = WeaponType::MACHINE_GUN;
    gunner.weaponFireRate = 8.0f;
    gunner.weaponDamage = 20.0f;
    def.seats.push_back(gunner);

    m_definitions[static_cast<int>(VehicleType::TANK)] = def;
}

void VehicleSystem::InitAPC()
{
    VehicleDefinition def;
    def.type = VehicleType::APC;
    def.name = "Sunderer APC";
    def.description = "Armored transport with mobile spawn capability";
    def.maxHealth = 1500.0f;
    def.maxArmor = 300.0f;
    def.explosionDamage = 250.0f;
    def.explosionRadius = 12.0f;
    def.dimensions = {3.0f, 2.5f, 7.0f};
    def.isAerial = false;

    def.handling.maxSpeed = 15.0f;
    def.handling.reverseSpeed = 6.0f;
    def.handling.acceleration = 10.0f;
    def.handling.brakeForce = 22.0f;
    def.handling.turnRate = 1.5f;
    def.handling.friction = 0.93f;
    def.handling.mass = 4000.0f;

    VehicleSeat driver(VehicleSeatRole::DRIVER, {-1.0f, 1.0f, 2.0f});
    def.seats.push_back(driver);

    // Two gunner seats
    VehicleSeat gunnerL(VehicleSeatRole::GUNNER, {-1.2f, 1.5f, 0.0f});
    gunnerL.mountedWeapon = WeaponType::MACHINE_GUN;
    gunnerL.weaponFireRate = 8.0f;
    gunnerL.weaponDamage = 18.0f;
    def.seats.push_back(gunnerL);

    VehicleSeat gunnerR(VehicleSeatRole::GUNNER, {1.2f, 1.5f, 0.0f});
    gunnerR.mountedWeapon = WeaponType::MACHINE_GUN;
    gunnerR.weaponFireRate = 8.0f;
    gunnerR.weaponDamage = 18.0f;
    def.seats.push_back(gunnerR);

    // Passenger seats
    for (int i = 0; i < 3; ++i) {
        VehicleSeat passenger(VehicleSeatRole::PASSENGER,
                              {(i % 2 == 0 ? -0.8f : 0.8f), 0.8f, -1.0f - i * 0.8f});
        def.seats.push_back(passenger);
    }

    m_definitions[static_cast<int>(VehicleType::APC)] = def;
}

void VehicleSystem::InitMotorcycle()
{
    VehicleDefinition def;
    def.type = VehicleType::MOTORCYCLE;
    def.name = "Flash Motorcycle";
    def.description = "Ultra-fast single-rider recon vehicle";
    def.maxHealth = 200.0f;
    def.maxArmor = 0.0f;
    def.explosionDamage = 80.0f;
    def.explosionRadius = 5.0f;
    def.dimensions = {0.8f, 1.2f, 2.0f};
    def.isAerial = false;

    def.handling.maxSpeed = 40.0f;
    def.handling.reverseSpeed = 8.0f;
    def.handling.acceleration = 25.0f;
    def.handling.brakeForce = 35.0f;
    def.handling.turnRate = 3.0f;
    def.handling.friction = 0.97f;
    def.handling.mass = 300.0f;

    VehicleSeat driver(VehicleSeatRole::DRIVER, {0.0f, 0.6f, 0.0f});
    def.seats.push_back(driver);

    m_definitions[static_cast<int>(VehicleType::MOTORCYCLE)] = def;
}

void VehicleSystem::InitTruck()
{
    VehicleDefinition def;
    def.type = VehicleType::TRUCK;
    def.name = "ANT Transport";
    def.description = "Heavy resource transport truck";
    def.maxHealth = 1000.0f;
    def.maxArmor = 200.0f;
    def.explosionDamage = 200.0f;
    def.explosionRadius = 10.0f;
    def.dimensions = {3.0f, 3.0f, 8.0f};
    def.isAerial = false;

    def.handling.maxSpeed = 18.0f;
    def.handling.reverseSpeed = 6.0f;
    def.handling.acceleration = 8.0f;
    def.handling.brakeForce = 18.0f;
    def.handling.turnRate = 1.3f;
    def.handling.friction = 0.91f;
    def.handling.mass = 6000.0f;

    VehicleSeat driver(VehicleSeatRole::DRIVER, {-0.8f, 1.5f, 2.0f});
    def.seats.push_back(driver);

    VehicleSeat passenger(VehicleSeatRole::PASSENGER, {0.8f, 1.5f, 2.0f});
    def.seats.push_back(passenger);

    m_definitions[static_cast<int>(VehicleType::TRUCK)] = def;
}

void VehicleSystem::InitHelicopter()
{
    VehicleDefinition def;
    def.type = VehicleType::HELICOPTER;
    def.name = "Valkyrie Gunship";
    def.description = "Versatile VTOL attack helicopter";
    def.maxHealth = 800.0f;
    def.maxArmor = 100.0f;
    def.explosionDamage = 300.0f;
    def.explosionRadius = 12.0f;
    def.dimensions = {4.0f, 3.0f, 8.0f};
    def.isAerial = true;

    def.handling.maxSpeed = 25.0f;
    def.handling.reverseSpeed = 10.0f;
    def.handling.acceleration = 15.0f;
    def.handling.brakeForce = 12.0f;
    def.handling.turnRate = 2.0f;
    def.handling.liftForce = 25.0f;
    def.handling.maxAltitude = 80.0f;
    def.handling.pitchRate = 2.0f;
    def.handling.rollRate = 2.5f;
    def.handling.hoverStability = 0.85f;
    def.handling.mass = 2000.0f;

    VehicleSeat pilot(VehicleSeatRole::PILOT, {0.0f, 0.5f, 1.5f});
    def.seats.push_back(pilot);

    VehicleSeat gunner(VehicleSeatRole::GUNNER, {0.0f, -0.5f, 0.0f});
    gunner.mountedWeapon = WeaponType::MINIGUN;
    gunner.weaponFireRate = 15.0f;
    gunner.weaponDamage = 12.0f;
    def.seats.push_back(gunner);

    // Door gunners and passengers
    for (int i = 0; i < 4; ++i) {
        VehicleSeat pass(VehicleSeatRole::PASSENGER,
                         {(i % 2 == 0 ? -1.2f : 1.2f), 0.2f, -0.5f - (i / 2) * 0.8f});
        def.seats.push_back(pass);
    }

    m_definitions[static_cast<int>(VehicleType::HELICOPTER)] = def;
}

void VehicleSystem::InitJet()
{
    VehicleDefinition def;
    def.type = VehicleType::JET;
    def.name = "Mosquito Fighter";
    def.description = "Single-seat high-speed combat aircraft";
    def.maxHealth = 600.0f;
    def.maxArmor = 50.0f;
    def.explosionDamage = 350.0f;
    def.explosionRadius = 15.0f;
    def.dimensions = {6.0f, 2.0f, 8.0f};
    def.isAerial = true;

    def.handling.maxSpeed = 50.0f;
    def.handling.reverseSpeed = 5.0f;
    def.handling.acceleration = 30.0f;
    def.handling.brakeForce = 15.0f;
    def.handling.turnRate = 2.5f;
    def.handling.liftForce = 35.0f;
    def.handling.maxAltitude = 150.0f;
    def.handling.pitchRate = 3.0f;
    def.handling.rollRate = 4.0f;
    def.handling.hoverStability = 0.3f; // Jets don't hover well
    def.handling.mass = 1500.0f;

    VehicleSeat pilot(VehicleSeatRole::PILOT, {0.0f, 0.5f, 0.0f});
    pilot.mountedWeapon = WeaponType::PLASMA_RIFLE;
    pilot.weaponFireRate = 8.0f;
    pilot.weaponDamage = 30.0f;
    def.seats.push_back(pilot);

    m_definitions[static_cast<int>(VehicleType::JET)] = def;
}

void VehicleSystem::InitDropship()
{
    VehicleDefinition def;
    def.type = VehicleType::DROPSHIP;
    def.name = "Galaxy Dropship";
    def.description = "Massive troop transport aircraft";
    def.maxHealth = 3000.0f;
    def.maxArmor = 400.0f;
    def.explosionDamage = 500.0f;
    def.explosionRadius = 20.0f;
    def.dimensions = {12.0f, 5.0f, 15.0f};
    def.isAerial = true;

    def.handling.maxSpeed = 18.0f;
    def.handling.reverseSpeed = 5.0f;
    def.handling.acceleration = 8.0f;
    def.handling.brakeForce = 10.0f;
    def.handling.turnRate = 1.0f;
    def.handling.liftForce = 20.0f;
    def.handling.maxAltitude = 120.0f;
    def.handling.pitchRate = 1.0f;
    def.handling.rollRate = 1.5f;
    def.handling.hoverStability = 0.9f;
    def.handling.mass = 10000.0f;

    VehicleSeat pilot(VehicleSeatRole::PILOT, {0.0f, 1.0f, 5.0f});
    def.seats.push_back(pilot);

    // Multiple gunner positions
    VehicleSeat tailGunner(VehicleSeatRole::GUNNER, {0.0f, 2.0f, -5.0f});
    tailGunner.mountedWeapon = WeaponType::MACHINE_GUN;
    tailGunner.weaponFireRate = 10.0f;
    tailGunner.weaponDamage = 20.0f;
    def.seats.push_back(tailGunner);

    // 10 passenger seats
    for (int i = 0; i < 10; ++i) {
        VehicleSeat pass(VehicleSeatRole::PASSENGER,
                         {(i % 2 == 0 ? -2.0f : 2.0f), 0.5f, 2.0f - i * 0.8f});
        def.seats.push_back(pass);
    }

    m_definitions[static_cast<int>(VehicleType::DROPSHIP)] = def;
}

void VehicleSystem::InitDrone()
{
    VehicleDefinition def;
    def.type = VehicleType::DRONE;
    def.name = "Recon Drone";
    def.description = "Small remote-controlled surveillance drone";
    def.maxHealth = 100.0f;
    def.maxArmor = 0.0f;
    def.explosionDamage = 50.0f;
    def.explosionRadius = 3.0f;
    def.dimensions = {0.6f, 0.3f, 0.6f};
    def.isAerial = true;

    def.handling.maxSpeed = 20.0f;
    def.handling.reverseSpeed = 10.0f;
    def.handling.acceleration = 25.0f;
    def.handling.brakeForce = 20.0f;
    def.handling.turnRate = 4.0f;
    def.handling.liftForce = 15.0f;
    def.handling.maxAltitude = 50.0f;
    def.handling.pitchRate = 4.0f;
    def.handling.rollRate = 5.0f;
    def.handling.hoverStability = 0.95f;
    def.handling.mass = 5.0f;

    // Drone is remote-controlled, no physical seat
    VehicleSeat pilot(VehicleSeatRole::PILOT, {0.0f, 0.0f, 0.0f});
    def.seats.push_back(pilot);

    m_definitions[static_cast<int>(VehicleType::DRONE)] = def;
}

} // namespace Spark


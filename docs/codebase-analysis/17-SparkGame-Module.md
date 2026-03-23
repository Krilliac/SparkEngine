# 17 — SparkGame Module

**Location:** `SparkGame/Source/`

Example FPS game module demonstrating all major engine subsystems. Loaded at runtime as a DLL, implements both `Spark::IModule` and legacy `IGameModule`.

---

## Module Structure

```
SparkGame/Source/
├── Core/
│   ├── Main.cpp              — DLL exports (CreateModule/DestroyModule)
│   └── SparkGame.h           — SparkGameModule class
├── Game/
│   ├── Game.h/cpp            — Central game coordinator (660+ lines)
│   ├── Player.h/cpp          — First-person player controller
│   ├── Enemy.h/cpp           — Enemy AI entities
│   ├── ClassSystem.h/cpp     — 6 FPS class archetypes
│   ├── GameMode.h/cpp        — Scoring and game rules
│   ├── HUDSystem.h/cpp       — Heads-up display
│   ├── InventorySystem.h/cpp — Item management
│   ├── QuestSystem.h/cpp     — Quest progression
│   ├── WaveSpawner.h/cpp     — Enemy wave management
│   ├── ProgressionSystem.h   — XP and leveling
│   ├── LootSystem.h/cpp      — Loot drops and power-ups
│   ├── VehicleSystem.h/cpp   — Vehicle management
│   ├── GravitySystem.h/cpp   — Gravity zones
│   ├── InteractionSystem.h   — Interactive objects
│   ├── Console.h/cpp         — In-game console overlay
│   └── ...
├── Projectiles/
│   ├── Projectile.h          — Base projectile class
│   ├── Bullet.h              — Hitscan bullets
│   ├── Grenade.h             — Grenade with arc + explosion
│   ├── Rocket.h              — Rocket with tracking
│   ├── ProjectilePool.h      — Object pool
│   └── WeaponStats.h         — Weapon definitions
└── Console/
    └── AdvancedConsoleCommands.h — Console command registration
```

---

## Game Class — Central Coordinator

**File:** `SparkGame/Source/Game/Game.h`

### Owned Systems

```cpp
class Game {
    // Core
    std::unique_ptr<SparkEngineCamera> m_camera;
    std::unique_ptr<Player> m_player;
    std::unique_ptr<ProjectilePool> m_projectilePool;

    // Gameplay
    std::unique_ptr<ClassSystem> m_classSystem;
    std::unique_ptr<GameMode> m_gameMode;
    std::unique_ptr<HUDSystem> m_hudSystem;
    std::unique_ptr<WaveSpawner> m_waveSpawner;
    std::unique_ptr<ProgressionSystem> m_progression;
    std::unique_ptr<LootSystem> m_lootSystem;

    // Systems
    std::unique_ptr<VehicleSystem> m_vehicleSystem;
    std::unique_ptr<GravitySystem> m_gravitySystem;
    std::unique_ptr<InteractionSystem> m_interactionSystem;

    // Inventory & Quests
    std::unique_ptr<PlayerInventory> m_playerInventory;
    std::unique_ptr<PlayerQuests> m_playerQuests;

    // State
    float m_timeScale = 1.0f;
    bool m_godModeEnabled = false;
    bool m_noclipEnabled = false;
    bool m_infiniteAmmoEnabled = false;
};
```

### Key Methods

```cpp
// Lifecycle
void Initialize(GraphicsEngine* graphics, InputManager* input);
void Shutdown();
void Update(float deltaTime);
void Render();
void Pause(); void Resume(); bool IsPaused();

// Scene
void LoadScene(); void SaveScene(); void ClearScene(); void CreateTestScene();

// Debug
void ApplyDebugSettings(bool godMode, bool noclip, bool infiniteAmmo);
void TeleportPlayer(float x, float y, float z);
void SpawnObject(const std::string& type, float x, float y, float z);

// Networking (if ENABLE_NETWORKING)
void StartServer(int port, int maxClients);
void ConnectToServer(const std::string& address, int port);
void DisconnectNetwork();

// Vehicles
void SpawnVehicle();
void PlayerEnterNearestVehicle();
void PlayerExitVehicle();

// Enemies
void SpawnEnemy();
int GetAliveEnemyCount();

// Class system
void SetPlayerClass(PlayerClass cls);
PlayerClass GetPlayerClass();
void CycleNextClass();
void CreateCombatArena();
```

---

## Player — First-Person Controller

**File:** `SparkGame/Source/Game/Player.h`

```cpp
class Player : public GameObject {
public:
    void Initialize(ID3D11Device*, ID3D11DeviceContext*, SparkEngineCamera*, InputManager*);
    void Update(float dt);
    void Render();
    void RenderWeapon();

    // Combat
    void TakeDamage(float damage);  // Accounts for armor
    float GetHealth() const;
    float GetArmor() const;
    float GetShield() const;

    // Weapons
    void Fire();
    void Reload();
    void SwitchWeapon(int slot);

    // Movement
    void Jump();
    void Crouch();
    void Sprint(bool enable);

    // State
    bool IsAlive() const;
    bool IsSprinting() const;
    bool IsCrouching() const;
};
```

Features: physics-based movement with gravity, head bob, footstep sounds, weapon sway, health/armor/shield, multiple weapon slots with reloading.

---

## ClassSystem — FPS Archetypes

**File:** `SparkGame/Source/Game/ClassSystem.h`

Six distinct player classes:

| Class | Health | Armor | Speed | Special |
|-------|--------|-------|-------|---------|
| **SCOUT** | Low | Low | Fast | Speed boost ability |
| **MEDIC** | Medium | Medium | Medium | Heal ability |
| **ENGINEER** | Medium | High | Medium | Turret deployment |
| **RECON** | Low | Low | Medium | Stealth ability |
| **VANGUARD** | High | High | Slow | Shield charge |
| **TITAN** | Very High | Very High | Very Slow | Heavy weapons |

### Class Definition

```cpp
struct ClassDefinition {
    PlayerClass classType;
    std::string name;
    float health, armor, shield;
    float moveSpeed, jumpHeight, sprintMultiplier, stamina;
    ClassLoadout loadout;          // Primary, Secondary, Sidearm, Tool
    ClassAbility ability;          // Active ability
    std::vector<PassiveTrait> passiveTraits;
};
```

### Abilities

`BOOST_SPEED`, `SHIELD_CHARGE`, `TURRET_DEPLOY`, `STEALTH`, `HEAL_AURA`, `EMP_BLAST`, `GRAPPLE_HOOK`, `FORTIFY`, `SCAN_ENEMIES`, `SUPPLY_DROP`

---

## Weapon System

**File:** `SparkGame/Source/Projectiles/WeaponStats.h`

```cpp
struct WeaponStats {
    std::string name;
    float damage;
    float fireRate;          // Rounds per second
    int magazineSize;
    int reserveAmmo;
    float reloadTime;
    float range;
    float accuracy;          // 0-1 (1 = perfect)
    float recoilVertical;
    float recoilHorizontal;
    ProjectileType projectileType;  // Bullet, Grenade, Rocket
};
```

---

## Projectile System

### ProjectilePool — Object Pool

```cpp
ProjectilePool pool;
pool.Initialize(200);  // Pre-allocate 200 projectiles

Projectile* bullet = pool.Acquire<Bullet>(origin, direction, stats);
bullet->Update(deltaTime);
if (bullet->IsExpired())
    pool.Release(bullet);
```

### Projectile Types

| Type | Behavior |
|------|----------|
| `Bullet` | Hitscan, instant raycast |
| `Grenade` | Arc trajectory, timed detonation, explosion radius |
| `Rocket` | Propelled, optional tracking, explosion on impact |

---

## Wave Spawner

**File:** `SparkGame/Source/Game/WaveSpawner.h`

```cpp
WaveSpawner spawner;
spawner.SetSpawnPoints(spawnPointPositions);
spawner.SetWaveConfig({
    {.enemyCount = 5, .delayBetween = 10.0f, .enemyType = "Basic"},
    {.enemyCount = 8, .delayBetween = 15.0f, .enemyType = "Heavy"},
    {.enemyCount = 12, .delayBetween = 20.0f, .enemyType = "Mixed"},
});

spawner.StartWaves();
spawner.Update(deltaTime);
int wave = spawner.GetCurrentWave();
int alive = spawner.GetAliveEnemyCount();
```

---

## DLL Exports

**File:** `SparkGame/Source/Core/Main.cpp`

```cpp
// New interface
extern "C" SPARK_GAME_API Spark::IModule* CreateModule() {
    return new SparkGameModule();
}
extern "C" SPARK_GAME_API void DestroyModule(Spark::IModule* mod) {
    delete mod;
}

// Legacy interface (backward compatibility)
extern "C" SPARK_GAME_API IGameModule* CreateGameModule() {
    return new SparkGameModule();
}
extern "C" SPARK_GAME_API void DestroyGameModule(IGameModule* mod) {
    delete mod;
}
```

### Module Info

```cpp
Spark::ModuleInfo SparkGameModule::GetModuleInfo() const {
    return {"Spark Arena - Engine Showcase", "2.0.0", SPARK_SDK_VERSION, 0, nullptr, 0};
}
```

---

## Console Commands

**File:** `SparkGame/Source/Console/AdvancedConsoleCommands.h`

```cpp
void RegisterAdvancedCommands(Game* game, GraphicsEngine* graphics);
```

Registers commands for: physics tweaking, graphics settings, debug modes (god, noclip, infinite ammo), stats display, teleportation, object spawning, enemy management, vehicle control, and more.

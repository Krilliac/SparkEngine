/**
 * @file HUDSystem.h
 * @brief FPS Heads-Up Display system for rendering in-game UI elements
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a complete FPS HUD with health/armor bars, ammo counter,
 * crosshair, damage indicators, kill feed, minimap, and objective markers.
 */

#pragma once
#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <deque>
#include <chrono>
#include <functional>
#include "../Enums/GameSystemEnums.h"

// Forward declarations
class Player;

namespace Spark {

/**
 * @brief Crosshair style options
 */
enum class CrosshairStyle {
    Classic,        ///< Simple + shape
    Dot,            ///< Center dot only
    Circle,         ///< Circle with center dot
    Dynamic,        ///< Expands when moving/shooting
    Custom          ///< User-defined crosshair
};

/**
 * @brief Damage indicator direction
 */
struct DamageIndicator {
    float angle;            ///< Direction of damage source (radians from forward)
    float intensity;        ///< Damage amount normalized 0-1
    float timeRemaining;    ///< Seconds until fade out
    float maxTime;          ///< Original duration

    DamageIndicator(float a, float i, float t)
        : angle(a), intensity(i), timeRemaining(t), maxTime(t) {}
};

/**
 * @brief Kill feed entry
 */
struct KillFeedEntry {
    std::string killerName;
    std::string victimName;
    std::string weaponName;
    bool isHeadshot;
    float timeRemaining;

    KillFeedEntry(const std::string& killer, const std::string& victim,
                  const std::string& weapon, bool headshot, float duration = 5.0f)
        : killerName(killer), victimName(victim), weaponName(weapon)
        , isHeadshot(headshot), timeRemaining(duration) {}
};

/**
 * @brief Objective marker for HUD compass/screen
 */
struct ObjectiveMarker {
    std::string label;
    DirectX::XMFLOAT3 worldPosition;
    float distance;
    bool isActive;
    int priority;       ///< Higher priority shown first

    ObjectiveMarker() : worldPosition{}, distance(0), isActive(true), priority(0) {}
};

/**
 * @brief Minimap blip types
 */
enum class MinimapBlipType {
    Player,
    Enemy,
    Ally,
    Objective,
    Pickup,
    Hazard
};

/**
 * @brief Minimap blip entry
 */
struct MinimapBlip {
    DirectX::XMFLOAT3 worldPosition;
    MinimapBlipType type;
    bool isPulsing;

    MinimapBlip() : worldPosition{}, type(MinimapBlipType::Player), isPulsing(false) {}
};

/**
 * @brief HUD configuration settings
 */
struct HUDConfig {
    // Visibility toggles
    bool showHealthBar = true;
    bool showArmorBar = true;
    bool showShieldBar = true;          ///< Rechargeable shield bar (PS2 style)
    bool showAmmoCounter = true;
    bool showCrosshair = true;
    bool showDamageIndicators = true;
    bool showKillFeed = true;
    bool showMinimap = true;
    bool showObjectiveMarkers = true;
    bool showScoreboard = false;
    bool showInteractionPrompt = true;
    bool showWeaponInfo = true;
    bool showStaminaBar = true;
    bool showClassInfo = true;          ///< Class name and icon
    bool showAbilityBar = true;         ///< Primary/secondary ability cooldowns
    bool showEnergyBar = true;          ///< Ability energy pool
    bool showLoadoutSlots = true;       ///< Weapon loadout slots (1-4)
    bool showOvershield = true;         ///< Heavy Assault overshield indicator
    bool showJetpackFuel = true;        ///< Light Assault jetpack fuel gauge
    bool showCloakIndicator = true;     ///< Infiltrator cloak status

    // Crosshair settings
    CrosshairStyle crosshairStyle = CrosshairStyle::Dynamic;
    float crosshairSize = 12.0f;
    float crosshairGap = 3.0f;
    float crosshairThickness = 2.0f;
    float crosshairSpread = 0.0f;  ///< Dynamic spread from movement/firing

    // Minimap settings
    float minimapSize = 150.0f;
    float minimapZoom = 1.0f;
    bool minimapRotateWithPlayer = true;

    // HUD opacity
    float globalOpacity = 1.0f;
    float healthBarOpacity = 1.0f;
    float minimapOpacity = 0.85f;

    // Colors (RGBA 0-255)
    struct { int r=76, g=175, b=80, a=220; } healthColor;
    struct { int r=45, g=140, b=240, a=220; } armorColor;
    struct { int r=0, g=200, b=255, a=200; } shieldColor;       ///< Shield bar color (cyan)
    struct { int r=255, g=255, b=255, a=200; } crosshairColor;
    struct { int r=229, g=57, b=53, a=180; } damageColor;
    struct { int r=255, g=200, b=50, a=220; } energyColor;      ///< Ability energy (gold)
    struct { int r=200, g=100, b=255, a=200; } abilityColor;    ///< Ability cooldown (purple)
    struct { int r=100, g=200, b=255, a=180; } overshieldColor;  ///< Overshield (light blue)
    struct { int r=255, g=165, b=0, a=220; } jetpackColor;      ///< Jetpack fuel (orange)
    struct { int r=150, g=150, b=200, a=150; } cloakColor;      ///< Cloak indicator (faded blue)
};

/**
 * @brief Interaction prompt data
 */
struct InteractionPrompt {
    std::string actionText;     ///< e.g., "Pick up"
    std::string objectName;     ///< e.g., "Shotgun"
    std::string keyHint;        ///< e.g., "E"
    bool isVisible = false;
};

/**
 * @brief Scoreboard entry for multiplayer/game mode display
 */
struct ScoreboardEntry {
    std::string playerName;
    int kills = 0;
    int deaths = 0;
    int assists = 0;
    int score = 0;
    int ping = 0;
    bool isLocalPlayer = false;
};

/**
 * @brief Complete FPS HUD system
 *
 * Manages all heads-up display elements for a first-person shooter game.
 * Reads player state and renders health, armor, ammo, crosshair, damage
 * indicators, kill feed, minimap, and objectives.
 */
class HUDSystem {
public:
    HUDSystem();
    ~HUDSystem() = default;

    /**
     * @brief Initialize the HUD system
     * @return true on success
     */
    bool Initialize();

    /**
     * @brief Update HUD animations and timers
     * @param deltaTime Frame delta time in seconds
     */
    void Update(float deltaTime);

    /**
     * @brief Set the player to read state from
     * @param player Pointer to active player
     */
    void SetPlayer(Player* player) { m_player = player; }

    /**
     * @brief Get mutable HUD configuration
     * @return Reference to HUD config
     */
    HUDConfig& GetConfig() { return m_config; }

    /**
     * @brief Get read-only HUD configuration
     * @return Const reference to HUD config
     */
    const HUDConfig& GetConfig() const { return m_config; }

    // === Damage Indicators ===

    /**
     * @brief Add a damage indicator from a direction
     * @param angle Direction angle in radians (0 = forward, PI/2 = right)
     * @param intensity Damage normalized 0-1
     * @param duration Display time in seconds
     */
    void AddDamageIndicator(float angle, float intensity, float duration = 1.5f);

    /**
     * @brief Add damage indicator from world position
     * @param damageSource World position of damage source
     * @param playerPos Player world position
     * @param playerForward Player forward direction
     * @param intensity Damage normalized 0-1
     */
    void AddDamageFromWorldPos(const DirectX::XMFLOAT3& damageSource,
                               const DirectX::XMFLOAT3& playerPos,
                               const DirectX::XMFLOAT3& playerForward,
                               float intensity);

    const std::vector<DamageIndicator>& GetDamageIndicators() const { return m_damageIndicators; }

    // === Kill Feed ===

    /**
     * @brief Add a kill feed entry
     */
    void AddKillFeedEntry(const std::string& killer, const std::string& victim,
                          const std::string& weapon, bool headshot = false);

    const std::deque<KillFeedEntry>& GetKillFeed() const { return m_killFeed; }

    // === Objectives ===

    void AddObjective(const std::string& label, const DirectX::XMFLOAT3& position, int priority = 0);
    void RemoveObjective(const std::string& label);
    void UpdateObjectivePosition(const std::string& label, const DirectX::XMFLOAT3& position);
    const std::vector<ObjectiveMarker>& GetObjectives() const { return m_objectives; }

    // === Minimap ===

    void ClearMinimapBlips();
    void AddMinimapBlip(const DirectX::XMFLOAT3& pos, MinimapBlipType type, bool pulsing = false);
    const std::vector<MinimapBlip>& GetMinimapBlips() const { return m_minimapBlips; }

    // === Interaction ===

    void SetInteractionPrompt(const std::string& action, const std::string& object, const std::string& key = "E");
    void ClearInteractionPrompt();
    const InteractionPrompt& GetInteractionPrompt() const { return m_interactionPrompt; }

    // === Scoreboard ===

    void SetScoreboard(const std::vector<ScoreboardEntry>& entries);
    const std::vector<ScoreboardEntry>& GetScoreboard() const { return m_scoreboard; }

    // === Dynamic Crosshair ===

    void SetCrosshairSpread(float spread);
    float GetCrosshairSpread() const { return m_config.crosshairSpread; }

    // === Hit Marker ===

    void ShowHitMarker(bool headshot = false);
    bool IsHitMarkerVisible() const { return m_hitMarkerTimer > 0.0f; }
    bool IsHitMarkerHeadshot() const { return m_hitMarkerHeadshot; }
    float GetHitMarkerAlpha() const;

    // === Low Health Effects ===

    float GetLowHealthVignetteIntensity() const { return m_lowHealthVignette; }
    bool IsLowHealthPulsing() const { return m_lowHealthPulseTimer > 0.0f; }

    // === Weapon Switching ===

    void ShowWeaponSwitch(const std::string& weaponName, int slotIndex);
    bool IsWeaponSwitchVisible() const { return m_weaponSwitchTimer > 0.0f; }
    const std::string& GetWeaponSwitchName() const { return m_weaponSwitchName; }
    int GetWeaponSwitchSlot() const { return m_weaponSwitchSlot; }
    float GetWeaponSwitchAlpha() const;

    // === Class System HUD ===

    /**
     * @brief Show class change notification
     * @param className Name of the new class
     * @param classType The class type enum
     */
    void ShowClassChange(const std::string& className, SparkEditor::PlayerClass classType);

    /**
     * @brief Check if class change notification is visible
     */
    bool IsClassChangeVisible() const { return m_classChangeTimer > 0.0f; }

    /**
     * @brief Get class change display name
     */
    const std::string& GetClassChangeName() const { return m_classChangeName; }

    /**
     * @brief Get class change display alpha
     */
    float GetClassChangeAlpha() const;

    /**
     * @brief Show ability activation feedback
     * @param abilityName Name of the ability
     * @param isPrimary Whether it's the primary ability
     */
    void ShowAbilityActivation(const std::string& abilityName, bool isPrimary);

    /**
     * @brief Check if ability activation notification is visible
     */
    bool IsAbilityNotifVisible() const { return m_abilityNotifTimer > 0.0f; }

    /**
     * @brief Get ability notification text
     */
    const std::string& GetAbilityNotifText() const { return m_abilityNotifText; }

    /**
     * @brief Get current class type for HUD rendering
     */
    SparkEditor::PlayerClass GetCurrentClassType() const { return m_currentClass; }

    /**
     * @brief Set current class type for HUD rendering
     */
    void SetCurrentClass(SparkEditor::PlayerClass classType) { m_currentClass = classType; }

private:
    Player* m_player = nullptr;
    HUDConfig m_config;

    // Damage indicators
    std::vector<DamageIndicator> m_damageIndicators;
    static constexpr int MAX_DAMAGE_INDICATORS = 8;

    // Kill feed
    std::deque<KillFeedEntry> m_killFeed;
    static constexpr int MAX_KILL_FEED = 5;

    // Objectives
    std::vector<ObjectiveMarker> m_objectives;

    // Minimap
    std::vector<MinimapBlip> m_minimapBlips;

    // Interaction
    InteractionPrompt m_interactionPrompt;

    // Scoreboard
    std::vector<ScoreboardEntry> m_scoreboard;

    // Hit marker
    float m_hitMarkerTimer = 0.0f;
    bool m_hitMarkerHeadshot = false;
    static constexpr float HIT_MARKER_DURATION = 0.3f;

    // Low health effects
    float m_lowHealthVignette = 0.0f;
    float m_lowHealthPulseTimer = 0.0f;

    // Weapon switch display
    float m_weaponSwitchTimer = 0.0f;
    std::string m_weaponSwitchName;
    int m_weaponSwitchSlot = 0;
    static constexpr float WEAPON_SWITCH_DURATION = 2.0f;

    void UpdateDamageIndicators(float dt);
    void UpdateKillFeed(float dt);
    void UpdateLowHealthEffects(float dt);

    // Class system HUD state
    SparkEditor::PlayerClass m_currentClass = SparkEditor::PlayerClass::LIGHT_ASSAULT;
    float m_classChangeTimer = 0.0f;
    std::string m_classChangeName;
    static constexpr float CLASS_CHANGE_DURATION = 3.0f;

    // Ability notification
    float m_abilityNotifTimer = 0.0f;
    std::string m_abilityNotifText;
    static constexpr float ABILITY_NOTIF_DURATION = 1.5f;
};

} // namespace Spark

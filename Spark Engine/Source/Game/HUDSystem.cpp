/**
 * @file HUDSystem.cpp
 * @brief Implementation of the FPS HUD system
 * @author Spark Engine Team
 * @date 2025
 */

#include "HUDSystem.h"
#include "Player.h"
#include <cmath>
#include <algorithm>

namespace Spark {

HUDSystem::HUDSystem() = default;

bool HUDSystem::Initialize() {
    m_damageIndicators.reserve(MAX_DAMAGE_INDICATORS);
    m_objectives.reserve(16);
    m_minimapBlips.reserve(64);
    return true;
}

void HUDSystem::Update(float deltaTime) {
    UpdateDamageIndicators(deltaTime);
    UpdateKillFeed(deltaTime);
    UpdateLowHealthEffects(deltaTime);

    // Update hit marker
    if (m_hitMarkerTimer > 0.0f) {
        m_hitMarkerTimer -= deltaTime;
    }

    // Update weapon switch display
    if (m_weaponSwitchTimer > 0.0f) {
        m_weaponSwitchTimer -= deltaTime;
    }
}

// === Damage Indicators ===

void HUDSystem::AddDamageIndicator(float angle, float intensity, float duration) {
    if (m_damageIndicators.size() >= MAX_DAMAGE_INDICATORS) {
        // Remove oldest
        m_damageIndicators.erase(m_damageIndicators.begin());
    }
    m_damageIndicators.emplace_back(angle, std::clamp(intensity, 0.0f, 1.0f), duration);
}

void HUDSystem::AddDamageFromWorldPos(const DirectX::XMFLOAT3& damageSource,
                                       const DirectX::XMFLOAT3& playerPos,
                                       const DirectX::XMFLOAT3& playerForward,
                                       float intensity) {
    // Calculate direction from player to damage source
    float dx = damageSource.x - playerPos.x;
    float dz = damageSource.z - playerPos.z;

    // Calculate angle relative to player forward
    float angle = atan2f(dx, dz) - atan2f(playerForward.x, playerForward.z);

    AddDamageIndicator(angle, intensity);
}

void HUDSystem::UpdateDamageIndicators(float dt) {
    auto it = m_damageIndicators.begin();
    while (it != m_damageIndicators.end()) {
        it->timeRemaining -= dt;
        if (it->timeRemaining <= 0.0f) {
            it = m_damageIndicators.erase(it);
        } else {
            ++it;
        }
    }
}

// === Kill Feed ===

void HUDSystem::AddKillFeedEntry(const std::string& killer, const std::string& victim,
                                  const std::string& weapon, bool headshot) {
    m_killFeed.emplace_front(killer, victim, weapon, headshot);
    while (m_killFeed.size() > MAX_KILL_FEED) {
        m_killFeed.pop_back();
    }
}

void HUDSystem::UpdateKillFeed(float dt) {
    auto it = m_killFeed.begin();
    while (it != m_killFeed.end()) {
        it->timeRemaining -= dt;
        if (it->timeRemaining <= 0.0f) {
            it = m_killFeed.erase(it);
        } else {
            ++it;
        }
    }
}

// === Objectives ===

void HUDSystem::AddObjective(const std::string& label, const DirectX::XMFLOAT3& position, int priority) {
    ObjectiveMarker marker;
    marker.label = label;
    marker.worldPosition = position;
    marker.priority = priority;
    marker.isActive = true;
    m_objectives.push_back(marker);
}

void HUDSystem::RemoveObjective(const std::string& label) {
    m_objectives.erase(
        std::remove_if(m_objectives.begin(), m_objectives.end(),
            [&label](const ObjectiveMarker& m) { return m.label == label; }),
        m_objectives.end());
}

void HUDSystem::UpdateObjectivePosition(const std::string& label, const DirectX::XMFLOAT3& position) {
    for (auto& obj : m_objectives) {
        if (obj.label == label) {
            obj.worldPosition = position;
            break;
        }
    }
}

// === Minimap ===

void HUDSystem::ClearMinimapBlips() {
    m_minimapBlips.clear();
}

void HUDSystem::AddMinimapBlip(const DirectX::XMFLOAT3& pos, MinimapBlipType type, bool pulsing) {
    MinimapBlip blip;
    blip.worldPosition = pos;
    blip.type = type;
    blip.isPulsing = pulsing;
    m_minimapBlips.push_back(blip);
}

// === Interaction Prompt ===

void HUDSystem::SetInteractionPrompt(const std::string& action, const std::string& object, const std::string& key) {
    m_interactionPrompt.actionText = action;
    m_interactionPrompt.objectName = object;
    m_interactionPrompt.keyHint = key;
    m_interactionPrompt.isVisible = true;
}

void HUDSystem::ClearInteractionPrompt() {
    m_interactionPrompt.isVisible = false;
}

// === Scoreboard ===

void HUDSystem::SetScoreboard(const std::vector<ScoreboardEntry>& entries) {
    m_scoreboard = entries;
}

// === Crosshair ===

void HUDSystem::SetCrosshairSpread(float spread) {
    m_config.crosshairSpread = std::max(0.0f, spread);
}

// === Hit Marker ===

void HUDSystem::ShowHitMarker(bool headshot) {
    m_hitMarkerTimer = HIT_MARKER_DURATION;
    m_hitMarkerHeadshot = headshot;
}

float HUDSystem::GetHitMarkerAlpha() const {
    if (m_hitMarkerTimer <= 0.0f) return 0.0f;
    return m_hitMarkerTimer / HIT_MARKER_DURATION;
}

// === Low Health Effects ===

void HUDSystem::UpdateLowHealthEffects(float dt) {
    if (!m_player) {
        m_lowHealthVignette = 0.0f;
        return;
    }

    float healthPct = m_player->GetHealth() / m_player->GetMaxHealth();

    if (healthPct < 0.3f) {
        // Intensity increases as health decreases
        m_lowHealthVignette = 1.0f - (healthPct / 0.3f);

        // Pulse effect
        m_lowHealthPulseTimer += dt * 3.0f;
        if (m_lowHealthPulseTimer > 6.2832f) { // 2*PI
            m_lowHealthPulseTimer -= 6.2832f;
        }
        m_lowHealthVignette *= (0.7f + 0.3f * sinf(m_lowHealthPulseTimer));
    } else {
        m_lowHealthVignette = 0.0f;
        m_lowHealthPulseTimer = 0.0f;
    }
}

// === Weapon Switch ===

void HUDSystem::ShowWeaponSwitch(const std::string& weaponName, int slotIndex) {
    m_weaponSwitchName = weaponName;
    m_weaponSwitchSlot = slotIndex;
    m_weaponSwitchTimer = WEAPON_SWITCH_DURATION;
}

float HUDSystem::GetWeaponSwitchAlpha() const {
    if (m_weaponSwitchTimer <= 0.0f) return 0.0f;
    if (m_weaponSwitchTimer > WEAPON_SWITCH_DURATION - 0.3f) {
        // Fade in
        return (WEAPON_SWITCH_DURATION - m_weaponSwitchTimer) / 0.3f;
    }
    if (m_weaponSwitchTimer < 0.5f) {
        // Fade out
        return m_weaponSwitchTimer / 0.5f;
    }
    return 1.0f;
}

} // namespace Spark

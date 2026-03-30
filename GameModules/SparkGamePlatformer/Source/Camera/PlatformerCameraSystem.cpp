/**
 * @file PlatformerCameraSystem.cpp
 * @brief Third-person follow camera with smooth tracking, look-ahead, and collision
 */

#include "PlatformerCameraSystem.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>

namespace Platformer
{

    bool PlatformerCameraSystem::Initialize(Spark::IEngineContext* context)
    {
        if (!context)
            return false;

        m_context = context;

        BuildDemoCameraZones();

        m_initialized = true;

        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("[Platformer Camera] Camera system initialized (" + std::to_string(m_cameraZones.size()) +
                        " camera zones)");
        return true;
    }

    void PlatformerCameraSystem::BuildDemoCameraZones()
    {
        // Side-scrolling zone for a narrow corridor section
        CameraZone sideView{};
        sideView.id = 1;
        sideView.posX = 40.0f;
        sideView.posY = 5.0f;
        sideView.posZ = 0.0f;
        sideView.width = 20.0f;
        sideView.height = 15.0f;
        sideView.depth = 10.0f;
        sideView.overrideOffsetX = 0.0f;
        sideView.overrideOffsetY = 5.0f;
        sideView.overrideOffsetZ = -15.0f;
        sideView.isFixedAngle = true;
        m_cameraZones.push_back(sideView);

        // Top-down zone for a puzzle area
        CameraZone topDown{};
        topDown.id = 2;
        topDown.posX = 80.0f;
        topDown.posY = 10.0f;
        topDown.posZ = 0.0f;
        topDown.width = 15.0f;
        topDown.height = 20.0f;
        topDown.depth = 15.0f;
        topDown.overrideOffsetX = 0.0f;
        topDown.overrideOffsetY = 18.0f;
        topDown.overrideOffsetZ = -2.0f;
        topDown.isFixedAngle = true;
        m_cameraZones.push_back(topDown);
    }

    void PlatformerCameraSystem::Update(float deltaTime, PlayerPosition targetPosition)
    {
        if (!m_initialized)
            return;

        CheckCameraZones(targetPosition.x, targetPosition.y, targetPosition.z);

        SmoothFollow(deltaTime, targetPosition);
        ApplyLookAhead(deltaTime, targetPosition);
        ApplyVerticalTracking(deltaTime, targetPosition.y);
        CheckCollision();

        m_previousTargetX = targetPosition.x;
    }

    void PlatformerCameraSystem::SmoothFollow(float deltaTime, const PlayerPosition& target)
    {
        // Determine active offsets (zone override or default)
        float activeOffsetX = m_offsetX;
        float activeOffsetY = m_offsetY;
        float activeOffsetZ = m_offsetZ;

        if (m_activeZone)
        {
            activeOffsetX = m_activeZone->overrideOffsetX;
            activeOffsetY = m_activeZone->overrideOffsetY;
            activeOffsetZ = m_activeZone->overrideOffsetZ;
        }

        // Target camera position = player + offset + look-ahead
        float desiredX = target.x + activeOffsetX + m_currentLookAheadX;
        float desiredY = target.y + activeOffsetY + m_currentVerticalOffset;
        float desiredZ = target.z + activeOffsetZ;

        // Exponential smoothing (frame-rate independent)
        float smoothX = 1.0f - std::exp(-m_followSmoothX * deltaTime);
        float smoothY = 1.0f - std::exp(-m_followSmoothY * deltaTime);
        float smoothZ = 1.0f - std::exp(-m_followSmoothZ * deltaTime);

        m_cameraPosition.x += (desiredX - m_cameraPosition.x) * smoothX;
        m_cameraPosition.y += (desiredY - m_cameraPosition.y) * smoothY;
        m_cameraPosition.z += (desiredZ - m_cameraPosition.z) * smoothZ;

        // Look target tracks the player directly
        m_lookTarget.x = target.x + m_currentLookAheadX * 0.5f;
        m_lookTarget.y = target.y + 1.5f; // Look slightly above feet
        m_lookTarget.z = target.z;
    }

    void PlatformerCameraSystem::ApplyLookAhead(float deltaTime, const PlayerPosition& target)
    {
        if (m_activeZone && m_activeZone->isFixedAngle)
        {
            // Fixed-angle zones may override look-ahead
            float desiredLookAhead = m_activeZone->overrideLookAheadX;
            float smooth = 1.0f - std::exp(-m_lookAheadSmooth * deltaTime);
            m_currentLookAheadX += (desiredLookAhead - m_currentLookAheadX) * smooth;
            return;
        }

        // Determine movement direction from position delta
        float velocityX = target.x - m_previousTargetX;
        float desiredLookAhead = 0.0f;

        if (std::abs(velocityX) > 0.01f)
        {
            float direction = (velocityX > 0.0f) ? 1.0f : -1.0f;
            desiredLookAhead = direction * m_lookAheadDistance;
        }

        // Smooth transition to target look-ahead
        float smooth = 1.0f - std::exp(-m_lookAheadSmooth * deltaTime);
        m_currentLookAheadX += (desiredLookAhead - m_currentLookAheadX) * smooth;
    }

    void PlatformerCameraSystem::ApplyVerticalTracking(float deltaTime, float targetY)
    {
        // Dead zone: don't adjust camera for small vertical movements
        float relativeY = targetY - (m_cameraPosition.y - m_offsetY);

        if (relativeY < m_verticalDeadZoneMin)
        {
            // Player below dead zone — camera follows down
            float target = relativeY - m_verticalDeadZoneMin;
            float smooth = 1.0f - std::exp(-m_followSmoothY * deltaTime);
            m_currentVerticalOffset += target * smooth;
        }
        else if (relativeY > m_verticalDeadZoneMax)
        {
            // Player above dead zone — camera follows up
            float target = relativeY - m_verticalDeadZoneMax;
            float smooth = 1.0f - std::exp(-m_followSmoothY * deltaTime);
            m_currentVerticalOffset += target * smooth;
        }
    }

    void PlatformerCameraSystem::CheckCollision()
    {
        // In a full implementation, this would cast a ray from the look target
        // to the desired camera position and pull the camera closer if the ray
        // hits level geometry (preventing the camera from clipping through walls).
        //
        // float hitDistance = Physics::Raycast(m_lookTarget, m_cameraPosition);
        // if (hitDistance < m_maxDistance)
        //     m_currentDistance = std::max(hitDistance - 0.5f, m_minDistance);
        // else
        //     m_currentDistance = m_maxDistance;
    }

    void PlatformerCameraSystem::CheckCameraZones(float playerX, float playerY, float playerZ)
    {
        m_activeZone = nullptr;

        for (const auto& zone : m_cameraZones)
        {
            float halfW = zone.width * 0.5f;
            float halfH = zone.height * 0.5f;
            float halfD = zone.depth * 0.5f;

            if (playerX >= zone.posX - halfW && playerX <= zone.posX + halfW && playerY >= zone.posY - halfH &&
                playerY <= zone.posY + halfH && playerZ >= zone.posZ - halfD && playerZ <= zone.posZ + halfD)
            {
                m_activeZone = &zone;
                return; // First matching zone wins
            }
        }
    }

    void PlatformerCameraSystem::Shutdown()
    {
        m_cameraZones.clear();
        m_activeZone = nullptr;
        m_initialized = false;
    }

    void PlatformerCameraSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!ImGui::CollapsingHeader("Platformer Camera"))
            return;

        ImGui::Text("Camera Position: (%.2f, %.2f, %.2f)", m_cameraPosition.x, m_cameraPosition.y, m_cameraPosition.z);
        ImGui::Text("Look Target: (%.2f, %.2f, %.2f)", m_lookTarget.x, m_lookTarget.y, m_lookTarget.z);
        ImGui::Text("Look Ahead X: %.2f", m_currentLookAheadX);
        ImGui::Text("Vertical Offset: %.2f", m_currentVerticalOffset);
        ImGui::Text("Distance: %.2f / %.2f", m_currentDistance, m_maxDistance);
        ImGui::Separator();

        if (m_activeZone)
        {
            ImGui::TextColored(ImVec4(1, 1, 0, 1), "Active Camera Zone: %u", m_activeZone->id);
            ImGui::Text("  Fixed Angle: %s", m_activeZone->isFixedAngle ? "Yes" : "No");
            ImGui::Text("  Offset: (%.1f, %.1f, %.1f)", m_activeZone->overrideOffsetX, m_activeZone->overrideOffsetY,
                        m_activeZone->overrideOffsetZ);
        }
        else
        {
            ImGui::Text("Camera Zone: Default");
        }

        ImGui::Separator();
        ImGui::SliderFloat("Follow Smooth X", &m_followSmoothX, 0.5f, 15.0f);
        ImGui::SliderFloat("Follow Smooth Y", &m_followSmoothY, 0.5f, 15.0f);
        ImGui::SliderFloat("Look Ahead Dist", &m_lookAheadDistance, 0.0f, 10.0f);
        ImGui::SliderFloat("Offset Y", &m_offsetY, 2.0f, 20.0f);
        ImGui::SliderFloat("Offset Z", &m_offsetZ, -20.0f, -3.0f);
#endif
    }

} // namespace Platformer

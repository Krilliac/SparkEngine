/**
 * @file GameplayDebugger.h
 * @brief Structured in-world gameplay debugging with category-based visualization
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a unified overlay for visualizing AI perception cones, navigation paths,
 * physics contacts, animation states, and networking data per-entity. Categories
 * can be toggled independently via console commands or editor UI.
 *
 * ## Usage
 * @code
 *   auto& debugger = Spark::Utils::GameplayDebugger::GetInstance();
 *   debugger.Initialize();
 *
 *   // Enable AI debug visualization
 *   debugger.SetCategoryEnabled(DebugCategory::AI, true);
 *
 *   // Add per-entity debug info
 *   debugger.AddEntityText(entityId, DebugCategory::AI, "State: Patrol");
 *   debugger.AddEntitySphere(entityId, DebugCategory::AI, position, perceptionRadius, color);
 *
 *   // In render loop
 *   debugger.Render();
 * @endcode
 */

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Utils
{

    /**
     * @brief Debug visualization categories
     */
    enum class DebugCategory : uint8_t
    {
        AI,         ///< Behavior tree state, perception cones, blackboard values
        Physics,    ///< Collision shapes, contacts, raycasts
        Navigation, ///< NavMesh, pathfinding routes, obstacle avoidance
        Animation,  ///< Bone transforms, blend weights, state machine states
        Networking, ///< Replication status, ping, packet loss
        Gameplay,   ///< Abilities, status effects, damage numbers
        Audio,      ///< Sound sources, listener position, attenuation radii
        Scripting,  ///< Script execution state, variable watches
        Count       ///< Number of categories (must be last)
    };

    /**
     * @brief Returns the display name for a debug category
     * @param cat The category
     * @return String name
     */
    inline const char* GetCategoryName(DebugCategory cat)
    {
        switch (cat)
        {
        case DebugCategory::AI:
            return "AI";
        case DebugCategory::Physics:
            return "Physics";
        case DebugCategory::Navigation:
            return "Navigation";
        case DebugCategory::Animation:
            return "Animation";
        case DebugCategory::Networking:
            return "Networking";
        case DebugCategory::Gameplay:
            return "Gameplay";
        case DebugCategory::Audio:
            return "Audio";
        case DebugCategory::Scripting:
            return "Scripting";
        default:
            return "Unknown";
        }
    }

    /**
     * @brief A debug text entry attached to an entity
     */
    struct DebugTextEntry
    {
        uint32_t entityId = 0;
        DebugCategory category = DebugCategory::Gameplay;
        std::string text;
        float lifetime = 0.0f; ///< 0 = one frame, >0 = seconds remaining
        uint32_t color = 0xFFFFFFFF;
    };

    /**
     * @brief A debug shape for 3D visualization
     */
    struct DebugShapeEntry
    {
        enum class Shape : uint8_t
        {
            Sphere,
            Box,
            Line,
            Cone,
            Arrow
        };

        uint32_t entityId = 0;
        DebugCategory category = DebugCategory::Gameplay;
        Shape shape = Shape::Sphere;
        float x = 0.0f, y = 0.0f, z = 0.0f;    ///< Position/start point
        float x2 = 0.0f, y2 = 0.0f, z2 = 0.0f; ///< End point (for lines/arrows)
        float radius = 1.0f;                   ///< Radius (sphere/cone)
        float halfAngle = 0.5f;                ///< Half-angle in radians (cone)
        uint32_t color = 0xFF00FF00;
        float lifetime = 0.0f;
    };

    /**
     * @brief Centralized gameplay debugging system
     *
     * Collects debug text and shapes per-entity, organized by category.
     * Renders them as overlays when the category is enabled. Debug data
     * is cleared each frame unless a lifetime is specified.
     */
    class GameplayDebugger
    {
      public:
        /**
         * @brief Get the singleton instance
         * @return Reference to the GameplayDebugger
         */
        static GameplayDebugger& GetInstance()
        {
            static GameplayDebugger instance;
            return instance;
        }

        /**
         * @brief Initialize the debugger
         */
        void Initialize()
        {
            m_categoryEnabled.fill(false);
            m_textEntries.clear();
            m_shapeEntries.clear();
            m_initialized = true;
        }

        /**
         * @brief Shut down the debugger
         */
        void Shutdown()
        {
            m_textEntries.clear();
            m_shapeEntries.clear();
            m_initialized = false;
        }

        /**
         * @brief Enable or disable a debug category
         * @param cat Category to toggle
         * @param enabled Whether to show this category
         */
        void SetCategoryEnabled(DebugCategory cat, bool enabled)
        {
            if (static_cast<size_t>(cat) < m_categoryEnabled.size())
                m_categoryEnabled[static_cast<size_t>(cat)] = enabled;
        }

        /**
         * @brief Check if a category is enabled
         * @param cat Category to check
         * @return true if enabled
         */
        bool IsCategoryEnabled(DebugCategory cat) const
        {
            auto idx = static_cast<size_t>(cat);
            return idx < m_categoryEnabled.size() && m_categoryEnabled[idx];
        }

        /**
         * @brief Toggle a debug category
         * @param cat Category to toggle
         */
        void ToggleCategory(DebugCategory cat)
        {
            auto idx = static_cast<size_t>(cat);
            if (idx < m_categoryEnabled.size())
                m_categoryEnabled[idx] = !m_categoryEnabled[idx];
        }

        /**
         * @brief Add debug text floating above an entity
         * @param entityId Entity to attach text to
         * @param cat Debug category
         * @param text Text to display
         * @param color RGBA color (default white)
         * @param lifetime Duration in seconds (0 = this frame only)
         */
        void AddEntityText(uint32_t entityId, DebugCategory cat, const std::string& text, uint32_t color = 0xFFFFFFFF,
                           float lifetime = 0.0f)
        {
            m_textEntries.push_back({entityId, cat, text, lifetime, color});
        }

        /**
         * @brief Add a debug sphere to an entity's position
         * @param entityId Entity to attach to
         * @param cat Debug category
         * @param x X position
         * @param y Y position
         * @param z Z position
         * @param radius Sphere radius
         * @param color RGBA color
         * @param lifetime Duration in seconds (0 = this frame only)
         */
        void AddEntitySphere(uint32_t entityId, DebugCategory cat, float x, float y, float z, float radius,
                             uint32_t color = 0xFF00FF00, float lifetime = 0.0f)
        {
            DebugShapeEntry entry;
            entry.entityId = entityId;
            entry.category = cat;
            entry.shape = DebugShapeEntry::Shape::Sphere;
            entry.x = x;
            entry.y = y;
            entry.z = z;
            entry.radius = radius;
            entry.color = color;
            entry.lifetime = lifetime;
            m_shapeEntries.push_back(entry);
        }

        /**
         * @brief Add a debug line
         * @param entityId Entity context
         * @param cat Debug category
         * @param x1 Start X
         * @param y1 Start Y
         * @param z1 Start Z
         * @param x2 End X
         * @param y2 End Y
         * @param z2 End Z
         * @param color RGBA color
         * @param lifetime Duration in seconds (0 = this frame only)
         */
        void AddLine(uint32_t entityId, DebugCategory cat, float x1, float y1, float z1, float x2, float y2, float z2,
                     uint32_t color = 0xFFFF0000, float lifetime = 0.0f)
        {
            DebugShapeEntry entry;
            entry.entityId = entityId;
            entry.category = cat;
            entry.shape = DebugShapeEntry::Shape::Line;
            entry.x = x1;
            entry.y = y1;
            entry.z = z1;
            entry.x2 = x2;
            entry.y2 = y2;
            entry.z2 = z2;
            entry.color = color;
            entry.lifetime = lifetime;
            m_shapeEntries.push_back(entry);
        }

        /**
         * @brief Add a debug cone (for AI perception visualization)
         * @param entityId Entity context
         * @param cat Debug category
         * @param x Position X
         * @param y Position Y
         * @param z Position Z
         * @param dirX Direction X
         * @param dirY Direction Y
         * @param dirZ Direction Z
         * @param halfAngle Half-angle in radians
         * @param length Cone length
         * @param color RGBA color
         * @param lifetime Duration in seconds (0 = this frame only)
         */
        void AddCone(uint32_t entityId, DebugCategory cat, float x, float y, float z, float dirX, float dirY,
                     float dirZ, float halfAngle, float length, uint32_t color = 0xFF00FFFF, float lifetime = 0.0f)
        {
            DebugShapeEntry entry;
            entry.entityId = entityId;
            entry.category = cat;
            entry.shape = DebugShapeEntry::Shape::Cone;
            entry.x = x;
            entry.y = y;
            entry.z = z;
            entry.x2 = dirX;
            entry.y2 = dirY;
            entry.z2 = dirZ;
            entry.halfAngle = halfAngle;
            entry.radius = length;
            entry.color = color;
            entry.lifetime = lifetime;
            m_shapeEntries.push_back(entry);
        }

        /**
         * @brief Update the debugger — expire timed entries
         * @param deltaTime Frame delta time in seconds
         */
        void Update(float deltaTime)
        {
            // Expire text entries
            for (auto it = m_textEntries.begin(); it != m_textEntries.end();)
            {
                it->lifetime -= deltaTime;
                if (it->lifetime <= 0.0f)
                    it = m_textEntries.erase(it);
                else
                    ++it;
            }

            // Expire shape entries
            for (auto it = m_shapeEntries.begin(); it != m_shapeEntries.end();)
            {
                it->lifetime -= deltaTime;
                if (it->lifetime <= 0.0f)
                    it = m_shapeEntries.erase(it);
                else
                    ++it;
            }
        }

        /**
         * @brief Get all active text entries (filtered by enabled categories)
         * @return Vector of visible text entries
         */
        std::vector<DebugTextEntry> GetVisibleTextEntries() const
        {
            std::vector<DebugTextEntry> result;
            for (const auto& entry : m_textEntries)
            {
                if (IsCategoryEnabled(entry.category))
                    result.push_back(entry);
            }
            return result;
        }

        /**
         * @brief Get all active shape entries (filtered by enabled categories)
         * @return Vector of visible shape entries
         */
        std::vector<DebugShapeEntry> GetVisibleShapeEntries() const
        {
            std::vector<DebugShapeEntry> result;
            for (const auto& entry : m_shapeEntries)
            {
                if (IsCategoryEnabled(entry.category))
                    result.push_back(entry);
            }
            return result;
        }

        /**
         * @brief Get total number of debug entries (text + shapes)
         * @return Total entry count
         */
        size_t GetTotalEntryCount() const { return m_textEntries.size() + m_shapeEntries.size(); }

        /**
         * @brief Get count of enabled categories
         * @return Number of enabled categories
         */
        size_t GetEnabledCategoryCount() const
        {
            size_t count = 0;
            for (size_t i = 0; i < static_cast<size_t>(DebugCategory::Count); ++i)
            {
                if (m_categoryEnabled[i])
                    ++count;
            }
            return count;
        }

        /**
         * @brief Get status string for console/debug display
         * @return Formatted status string
         */
        std::string Console_GetStatus() const
        {
            std::string status = "GameplayDebugger: ";
            status += std::to_string(m_textEntries.size()) + " text, ";
            status += std::to_string(m_shapeEntries.size()) + " shapes, ";
            status += std::to_string(GetEnabledCategoryCount()) + "/" +
                      std::to_string(static_cast<size_t>(DebugCategory::Count)) + " categories enabled";
            return status;
        }

      private:
        GameplayDebugger() = default;

        bool m_initialized = false;
        std::array<bool, static_cast<size_t>(DebugCategory::Count)> m_categoryEnabled{};
        std::vector<DebugTextEntry> m_textEntries;
        std::vector<DebugShapeEntry> m_shapeEntries;
    };

} // namespace Spark::Utils

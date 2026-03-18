// TestModuleHotReload.cpp - Tests for Spark::ModuleHotReloadManager
// Standalone reimplementation to avoid platform-specific header dependencies

#include "TestFramework.h"
#include <chrono>
#include <cstdint>
#include <string>

// ============================================================================
// Standalone reimplementation of Spark::ModuleHotReloadManager
// ============================================================================

namespace
{

    enum class ModuleChangeType : uint8_t
    {
        Modified,
        Added,
        Removed
    };

    struct ModuleChangeEvent
    {
        std::string modulePath;
        ModuleChangeType type = ModuleChangeType::Modified;
        uint64_t timestampMs = 0;
    };

    class ModuleHotReloadManager
    {
      public:
        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsEnabled() const { return m_enabled; }

        void SetDebounceMs(uint64_t ms) { m_debounceMs = ms; }
        uint64_t GetDebounceMs() const { return m_debounceMs; }

        /// Returns true if the event should trigger a reload (respects debounce).
        bool ShouldReload(const ModuleChangeEvent& event)
        {
            if (!m_enabled)
                return false;

            if (event.timestampMs < m_lastReloadTimestampMs + m_debounceMs)
                return false;

            return true;
        }

        /// Record that a reload occurred at the given timestamp.
        void RecordReload(uint64_t timestampMs)
        {
            m_lastReloadTimestampMs = timestampMs;
            m_reloadCount++;
        }

        uint32_t GetReloadCount() const { return m_reloadCount; }

        std::string GetStatusString() const
        {
            if (!m_enabled)
                return "Hot reload: disabled";

            if (m_reloadCount == 0)
                return "Hot reload: enabled (no reloads)";

            return "Hot reload: enabled (" + std::to_string(m_reloadCount) + " reload" +
                   (m_reloadCount == 1 ? "" : "s") + ")";
        }

      private:
        bool m_enabled = false;
        uint64_t m_debounceMs = 500;
        uint64_t m_lastReloadTimestampMs = 0;
        uint32_t m_reloadCount = 0;
    };

} // namespace

// ============================================================================
// ModuleChangeEvent struct
// ============================================================================

TEST(ModuleHotReload_ChangeEvent_Defaults)
{
    ModuleChangeEvent event;
    EXPECT_TRUE(event.modulePath.empty());
    EXPECT_TRUE(event.type == ModuleChangeType::Modified);
    EXPECT_EQ(event.timestampMs, 0u);
}

TEST(ModuleHotReload_ChangeEvent_Values)
{
    ModuleChangeEvent event;
    event.modulePath = "Scripts/Player.as";
    event.type = ModuleChangeType::Added;
    event.timestampMs = 12345;

    EXPECT_EQ(event.modulePath, std::string("Scripts/Player.as"));
    EXPECT_TRUE(event.type == ModuleChangeType::Added);
    EXPECT_EQ(event.timestampMs, 12345u);
}

// ============================================================================
// Enable / disable state
// ============================================================================

TEST(ModuleHotReload_DefaultDisabled)
{
    ModuleHotReloadManager manager;
    EXPECT_FALSE(manager.IsEnabled());
}

TEST(ModuleHotReload_EnableDisable)
{
    ModuleHotReloadManager manager;

    manager.SetEnabled(true);
    EXPECT_TRUE(manager.IsEnabled());

    manager.SetEnabled(false);
    EXPECT_FALSE(manager.IsEnabled());
}

TEST(ModuleHotReload_DisabledBlocksReload)
{
    ModuleHotReloadManager manager;
    manager.SetEnabled(false);

    ModuleChangeEvent event;
    event.timestampMs = 10000;
    EXPECT_FALSE(manager.ShouldReload(event));
}

// ============================================================================
// Debounce timing logic
// ============================================================================

TEST(ModuleHotReload_DebounceDefault)
{
    ModuleHotReloadManager manager;
    EXPECT_EQ(manager.GetDebounceMs(), 500u);
}

TEST(ModuleHotReload_DebounceCustom)
{
    ModuleHotReloadManager manager;
    manager.SetDebounceMs(1000);
    EXPECT_EQ(manager.GetDebounceMs(), 1000u);
}

TEST(ModuleHotReload_DebounceRejectsEarlyEvent)
{
    ModuleHotReloadManager manager;
    manager.SetEnabled(true);
    manager.SetDebounceMs(500);

    // First reload at t=1000
    ModuleChangeEvent first;
    first.timestampMs = 1000;
    EXPECT_TRUE(manager.ShouldReload(first));
    manager.RecordReload(first.timestampMs);

    // Second event at t=1200, within debounce window
    ModuleChangeEvent tooSoon;
    tooSoon.timestampMs = 1200;
    EXPECT_FALSE(manager.ShouldReload(tooSoon));

    // Third event at t=1600, after debounce window
    ModuleChangeEvent afterDebounce;
    afterDebounce.timestampMs = 1600;
    EXPECT_TRUE(manager.ShouldReload(afterDebounce));
}

TEST(ModuleHotReload_DebounceZeroAllowsAll)
{
    ModuleHotReloadManager manager;
    manager.SetEnabled(true);
    manager.SetDebounceMs(0);

    ModuleChangeEvent event1;
    event1.timestampMs = 100;
    EXPECT_TRUE(manager.ShouldReload(event1));
    manager.RecordReload(event1.timestampMs);

    ModuleChangeEvent event2;
    event2.timestampMs = 100;
    EXPECT_TRUE(manager.ShouldReload(event2));
}

// ============================================================================
// Reload count tracking
// ============================================================================

TEST(ModuleHotReload_ReloadCountInitiallyZero)
{
    ModuleHotReloadManager manager;
    EXPECT_EQ(manager.GetReloadCount(), 0u);
}

TEST(ModuleHotReload_ReloadCountIncrements)
{
    ModuleHotReloadManager manager;
    manager.SetEnabled(true);

    manager.RecordReload(1000);
    EXPECT_EQ(manager.GetReloadCount(), 1u);

    manager.RecordReload(2000);
    EXPECT_EQ(manager.GetReloadCount(), 2u);

    manager.RecordReload(3000);
    EXPECT_EQ(manager.GetReloadCount(), 3u);
}

// ============================================================================
// Status string generation
// ============================================================================

TEST(ModuleHotReload_StatusString_Disabled)
{
    ModuleHotReloadManager manager;
    EXPECT_EQ(manager.GetStatusString(), std::string("Hot reload: disabled"));
}

TEST(ModuleHotReload_StatusString_EnabledNoReloads)
{
    ModuleHotReloadManager manager;
    manager.SetEnabled(true);
    EXPECT_EQ(manager.GetStatusString(), std::string("Hot reload: enabled (no reloads)"));
}

TEST(ModuleHotReload_StatusString_SingleReload)
{
    ModuleHotReloadManager manager;
    manager.SetEnabled(true);
    manager.RecordReload(1000);
    EXPECT_EQ(manager.GetStatusString(), std::string("Hot reload: enabled (1 reload)"));
}

TEST(ModuleHotReload_StatusString_MultipleReloads)
{
    ModuleHotReloadManager manager;
    manager.SetEnabled(true);
    manager.RecordReload(1000);
    manager.RecordReload(2000);
    manager.RecordReload(3000);
    EXPECT_EQ(manager.GetStatusString(), std::string("Hot reload: enabled (3 reloads)"));
}

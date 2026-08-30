/**
 * @file MemoryIntegrity.cpp
 * @brief Runtime memory integrity and branch protection implementation
 * @author Spark Engine Team
 * @date 2026
 *
 * @note MEMORY INTEGRITY GUARDS: NOT INCLUDED in this file.
 * Reason: Self-referential — the integrity system cannot guard itself without
 * creating a circular dependency. The system's own code regions are instead
 * protected via auto-discovered code page scanning at startup.
 */

#include "MemoryIntegrity.h"
#include "../../Utils/EventBus.h"
#include "../../Utils/LogMacros.h"
#include "../../Utils/SparkConsole.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <sstream>

#ifdef SPARK_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(SPARK_PLATFORM_LINUX)
#include <fstream>
#endif

namespace Spark::Security
{

    // =========================================================================
    // Event type for EventBus integration
    // =========================================================================

    /// @brief Published on EventBus when a memory integrity violation is detected
    struct MemoryViolationEvent
    {
        ViolationType type;
        ViolationSeverity severity;
        std::string regionName;
        uint64_t expectedHash;
        uint64_t actualHash;
    };

    // =========================================================================
    // Singleton
    // =========================================================================

    MemoryIntegritySystem& MemoryIntegritySystem::GetInstance()
    {
        static MemoryIntegritySystem instance;
        return instance;
    }

    // =========================================================================
    // Lifecycle
    // =========================================================================

    void MemoryIntegritySystem::Initialize()
    {
        std::lock_guard lifecycleLock(m_lifecycleMutex);
        size_t discoveredRegionCount = 0;

        {
            std::lock_guard stateLock(m_mutex);

            if (m_initialized.load(std::memory_order_acquire))
                return;

            m_regions.clear();
            m_branchGuards.clear();
            if (++m_branchGuardGeneration == 0)
                ++m_branchGuardGeneration;
            m_violations.clear();
            m_scanCursor = 0;
            m_scanCount = 0;
            m_timeSinceLastScan = 0.0f;
            m_lastScanDurationSec = 0.0f;

            if (m_config.autoDiscoverRegions)
            {
                AutoDiscoverCodeRegions();
            }

            discoveredRegionCount = m_regions.size();
        }

        // SimpleConsole::ExecuteCommand holds the console lifecycle lease while
        // its permission guard acquires m_mutex. Never acquire console locks
        // while m_mutex is held, or those two paths form a lock-order cycle.
        RegisterConsoleCommands();

        m_initialized.store(true, std::memory_order_release);
        SPARK_LOG_INFO(Spark::LogCategory::Core, "MemoryIntegritySystem initialized (%zu regions auto-discovered)",
                       discoveredRegionCount);
    }

    void MemoryIntegritySystem::Update(float dt)
    {
        if (!m_config.enabled || !m_initialized.load(std::memory_order_acquire))
            return;

        m_timeSinceLastScan += dt;

        if (m_timeSinceLastScan >= m_config.scanIntervalSec)
        {
            m_timeSinceLastScan = 0.0f;
            ScanRegionBatch(m_config.batchSize);
        }
    }

    void MemoryIntegritySystem::Shutdown()
    {
        std::lock_guard lifecycleLock(m_lifecycleMutex);
        size_t violationCount = 0;
        uint32_t scanCount = 0;

        {
            std::lock_guard stateLock(m_mutex);

            if (!m_initialized.load(std::memory_order_acquire))
                return;

            violationCount = m_violations.size();
            scanCount = m_scanCount;
            m_regions.clear();
            m_branchGuards.clear();
            if (++m_branchGuardGeneration == 0)
                ++m_branchGuardGeneration;
            m_initialized.store(false, std::memory_order_release);
        }

        if (violationCount != 0)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "MemoryIntegritySystem shutting down with %zu violations recorded",
                           violationCount);
        }

        SPARK_LOG_INFO(Spark::LogCategory::Core, "MemoryIntegritySystem shut down (scanned %u times)", scanCount);
    }

    // =========================================================================
    // Configuration
    // =========================================================================

    void MemoryIntegritySystem::Configure(const IntegrityConfig& config)
    {
        std::lock_guard lock(m_mutex);
        m_config = config;
    }

    // =========================================================================
    // Code region protection
    // =========================================================================

    void MemoryIntegritySystem::RegisterCodeRegion(std::string_view name, const void* address, size_t size)
    {
        if (address == nullptr || size == 0)
            return;

        std::lock_guard lock(m_mutex);

        ProtectedRegion region;
        region.name = std::string(name);
        region.address = address;
        region.size = size;
        region.expectedHash = HashRegion(address, size);

        m_regions.push_back(std::move(region));
    }

    void MemoryIntegritySystem::RegisterFunction(std::string_view name, const void* funcPtr, size_t estimatedSize)
    {
        RegisterCodeRegion(name, funcPtr, estimatedSize);
    }

    bool MemoryIntegritySystem::ScanAllRegions()
    {
        std::lock_guard lock(m_mutex);

        auto scanStart = std::chrono::steady_clock::now();
        bool allClean = true;

        for (const auto& region : m_regions)
        {
            uint64_t currentHash = HashRegion(region.address, region.size);
            if (currentHash != region.expectedHash)
            {
                Violation v;
                v.type = ViolationType::CodeModified;
                v.severity = ViolationSeverity::Critical;
                v.regionName = region.name;
                v.expectedHash = region.expectedHash;
                v.actualHash = currentHash;
                v.timestamp = std::chrono::steady_clock::now();
                ReportViolation(v);
                allClean = false;
            }
        }

        auto scanEnd = std::chrono::steady_clock::now();
        m_lastScanDurationSec = std::chrono::duration<float>(scanEnd - scanStart).count();
        ++m_scanCount;

        return allClean;
    }

    bool MemoryIntegritySystem::ScanRegionBatch(size_t batchSize)
    {
        std::lock_guard lock(m_mutex);

        if (m_regions.empty())
            return true;

        auto scanStart = std::chrono::steady_clock::now();
        bool batchClean = true;

        for (size_t i = 0; i < batchSize && !m_regions.empty(); ++i)
        {
            if (m_scanCursor >= m_regions.size())
                m_scanCursor = 0;

            const auto& region = m_regions[m_scanCursor];
            uint64_t currentHash = HashRegion(region.address, region.size);

            if (currentHash != region.expectedHash)
            {
                Violation v;
                v.type = ViolationType::CodeModified;
                v.severity = ViolationSeverity::Critical;
                v.regionName = region.name;
                v.expectedHash = region.expectedHash;
                v.actualHash = currentHash;
                v.timestamp = std::chrono::steady_clock::now();
                ReportViolation(v);
                batchClean = false;
            }

            ++m_scanCursor;
        }

        auto scanEnd = std::chrono::steady_clock::now();
        m_lastScanDurationSec = std::chrono::duration<float>(scanEnd - scanStart).count();
        ++m_scanCount;

        return batchClean;
    }

    // =========================================================================
    // Branch guards
    // =========================================================================

    void MemoryIntegritySystem::RegisterBranchGuard(uint64_t id, std::string_view name)
    {
        std::lock_guard lock(m_mutex);

        if (m_branchGuards.contains(id))
            return;

        BranchGuardEntry entry;
        entry.name = std::string(name);
        m_branchGuards[id] = std::move(entry);
    }

    uint64_t MemoryIntegritySystem::RecordBranchExecution(uint64_t id, uint32_t pathTaken)
    {
        std::lock_guard lock(m_mutex);

        auto it = m_branchGuards.find(id);
        if (it == m_branchGuards.end())
        {
            // Auto-register on first use (macro handles explicit registration too)
            BranchGuardEntry entry;
            entry.lastPathTaken = pathTaken;
            entry.executionCount = 1;
            m_branchGuards[id] = std::move(entry);
            return m_branchGuardGeneration;
        }

        it->second.lastPathTaken = pathTaken;
        ++it->second.executionCount;
        return m_branchGuardGeneration;
    }

    void MemoryIntegritySystem::VerifyBranchExecuted(uint64_t id)
    {
        VerifyBranchExecuted(id, 0);
    }

    void MemoryIntegritySystem::VerifyBranchExecuted(uint64_t id, uint64_t recordedGeneration)
    {
        std::lock_guard lock(m_mutex);

        // A lifecycle boundary invalidates the complete observation. Do not
        // reinterpret evidence from an older, cleared registry as a bypass in
        // the new generation.
        if (recordedGeneration != 0 && recordedGeneration != m_branchGuardGeneration)
            return;

        auto it = m_branchGuards.find(id);
        if (it == m_branchGuards.end())
        {
            // Guard was never registered or executed — suspicious
            Violation v;
            v.type = ViolationType::BranchBypassed;
            v.severity = ViolationSeverity::Critical;
            v.regionName = std::format("branch_guard_{:#x}", id);
            v.timestamp = std::chrono::steady_clock::now();
            ReportViolation(v);
            return;
        }

        auto& guard = it->second;
        ++guard.verifyCount;

        if (guard.executionCount == 0)
        {
            // Branch was registered but never executed — NOP'd or bypassed
            ++guard.bypassCount;

            ViolationSeverity severity =
                (guard.bypassCount >= 3) ? ViolationSeverity::Critical : ViolationSeverity::Warning;

            Violation v;
            v.type = ViolationType::BranchBypassed;
            v.severity = severity;
            v.regionName = guard.name.empty() ? std::format("branch_guard_{:#x}", id) : guard.name;
            v.expectedHash = 1; // expected: at least 1 execution
            v.actualHash = 0;   // actual: 0 executions
            v.timestamp = std::chrono::steady_clock::now();
            ReportViolation(v);
        }

        // Reset execution count for next verify cycle
        guard.executionCount = 0;
    }

    bool MemoryIntegritySystem::WasBranchBypassed(uint64_t id) const
    {
        std::lock_guard lock(m_mutex);

        auto it = m_branchGuards.find(id);
        if (it == m_branchGuards.end())
            return true; // Unknown guard = suspicious

        return it->second.bypassCount > 0;
    }

    // =========================================================================
    // Violation callback
    // =========================================================================

    void MemoryIntegritySystem::SetViolationCallback(ViolationCallback callback)
    {
        std::lock_guard lock(m_mutex);
        m_violationCallback = std::move(callback);
    }

    // =========================================================================
    // Reporting
    // =========================================================================

    IntegrityMetrics MemoryIntegritySystem::GetMetrics() const
    {
        std::lock_guard lock(m_mutex);

        IntegrityMetrics metrics;
        metrics.regionsMonitored = static_cast<uint32_t>(m_regions.size());
        metrics.branchGuardsRegistered = static_cast<uint32_t>(m_branchGuards.size());
        metrics.violationsDetected = static_cast<uint32_t>(m_violations.size());
        metrics.scanCount = m_scanCount;
        metrics.lastScanTimeSec = m_lastScanDurationSec;
        metrics.timeSinceLastScanSec = m_timeSinceLastScan;
        return metrics;
    }

    std::string MemoryIntegritySystem::GetStatusString() const
    {
        auto metrics = GetMetrics();

        std::ostringstream ss;
        ss << "=== Memory Integrity Status ===\n";
        ss << "Enabled:           " << (m_config.enabled ? "yes" : "no") << "\n";
        ss << "Regions monitored: " << metrics.regionsMonitored << "\n";
        ss << "Branch guards:     " << metrics.branchGuardsRegistered << "\n";
        ss << "Violations:        " << metrics.violationsDetected << "\n";
        ss << "Scans completed:   " << metrics.scanCount << "\n";
        ss << std::format("Last scan time:    {:.3f} ms\n", metrics.lastScanTimeSec * 1000.0f);
        ss << std::format("Next scan in:      {:.1f}s\n",
                          std::max(0.0f, m_config.scanIntervalSec - metrics.timeSinceLastScanSec));
        return ss.str();
    }

    void MemoryIntegritySystem::RegisterConsoleCommands()
    {
        auto& console = Spark::SimpleConsole::GetInstance();

        console.RegisterCommand(
            "memory.integrity.status", [](const std::vector<std::string>&)
            { return MemoryIntegritySystem::GetInstance().GetStatusString(); }, "Show memory integrity system status",
            "Security", "memory.integrity.status", Spark::CommandPermission::Developer);

        console.RegisterCommand(
            "memory.integrity.scan",
            [](const std::vector<std::string>&)
            {
                bool clean = MemoryIntegritySystem::GetInstance().ScanAllRegions();
                return std::string(clean ? "Full scan complete: all regions clean"
                                         : "Full scan complete: VIOLATIONS DETECTED");
            },
            "Force immediate full integrity scan", "Security", "memory.integrity.scan",
            Spark::CommandPermission::Developer);

        console.RegisterCommand(
            "memory.integrity.violations",
            [](const std::vector<std::string>&)
            {
                const auto& violations = MemoryIntegritySystem::GetInstance().GetViolations();
                if (violations.empty())
                    return std::string("No violations recorded.");

                std::ostringstream ss;
                ss << violations.size() << " violation(s):\n";
                for (size_t i = 0; i < std::min(violations.size(), size_t{20}); ++i)
                {
                    const auto& v = violations[i];
                    const char* typeStr = "Unknown";
                    switch (v.type)
                    {
                    case ViolationType::CodeModified:
                        typeStr = "CodeModified";
                        break;
                    case ViolationType::BranchBypassed:
                        typeStr = "BranchBypassed";
                        break;
                    case ViolationType::BranchWrongPath:
                        typeStr = "BranchWrongPath";
                        break;
                    case ViolationType::FunctionModified:
                        typeStr = "FunctionModified";
                        break;
                    }
                    const char* sevStr = (v.severity == ViolationSeverity::Critical) ? "CRITICAL" : "WARNING";
                    ss << std::format("  [{}] {} — {} (expected {:#x}, got {:#x})\n", sevStr, typeStr, v.regionName,
                                      v.expectedHash, v.actualHash);
                }
                if (violations.size() > 20)
                    ss << "  ... and " << (violations.size() - 20) << " more\n";
                return ss.str();
            },
            "List recent memory integrity violations", "Security", "memory.integrity.violations",
            Spark::CommandPermission::Developer);
    }

    // =========================================================================
    // Private helpers
    // =========================================================================

    uint64_t MemoryIntegritySystem::HashRegion(const void* address, size_t size)
    {
        return Spark::FNV1a64(address, size);
    }

    void MemoryIntegritySystem::ReportViolation(const Violation& violation)
    {
        // Cap stored violations
        if (m_violations.size() < MAX_VIOLATIONS)
        {
            m_violations.push_back(violation);
        }

        // Log
        const char* sevStr = (violation.severity == ViolationSeverity::Critical) ? "CRITICAL" : "WARNING";
        SPARK_LOG_WARN(Spark::LogCategory::Core, "[MemoryIntegrity] %s violation in '%s' (expected %llx, got %llx)",
                       sevStr, violation.regionName.c_str(), static_cast<unsigned long long>(violation.expectedHash),
                       static_cast<unsigned long long>(violation.actualHash));

        // Fire callback (no lock held — callback runs outside our lock)
        if (m_violationCallback)
        {
            m_violationCallback(violation);
        }

        // Publish to EventBus
        Spark::EventBus::Global().Publish(MemoryViolationEvent{
            .type = violation.type,
            .severity = violation.severity,
            .regionName = violation.regionName,
            .expectedHash = violation.expectedHash,
            .actualHash = violation.actualHash,
        });
    }

    // =========================================================================
    // Platform-specific code region auto-discovery
    // =========================================================================

    void MemoryIntegritySystem::AutoDiscoverCodeRegions()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        // Walk the current module's executable pages via VirtualQuery
        HMODULE hModule = GetModuleHandle(nullptr);
        if (hModule == nullptr)
            return;

        // GetModuleInformation requires psapi — use VirtualQuery walk instead
        auto baseAddr = reinterpret_cast<const uint8_t*>(hModule);
        MEMORY_BASIC_INFORMATION mbi{};
        const uint8_t* addr = baseAddr;

        uint32_t regionIndex = 0;
        while (VirtualQuery(addr, &mbi, sizeof(mbi)) != 0)
        {
            // Only interested in committed, executable pages belonging to our module
            if (mbi.State == MEM_COMMIT && (mbi.Protect == PAGE_EXECUTE_READ || mbi.Protect == PAGE_EXECUTE_READWRITE ||
                                            mbi.Protect == PAGE_EXECUTE))
            {
                std::string name = std::format("code_page_{}", regionIndex++);
                ProtectedRegion region;
                region.name = std::move(name);
                region.address = mbi.BaseAddress;
                region.size = mbi.RegionSize;
                region.expectedHash = HashRegion(mbi.BaseAddress, mbi.RegionSize);
                m_regions.push_back(std::move(region));
            }

            addr = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;

            // Guard against wrap-around
            if (addr <= static_cast<const uint8_t*>(mbi.BaseAddress))
                break;
        }

#elif defined(SPARK_PLATFORM_LINUX)
        // Parse /proc/self/maps for executable regions
        std::ifstream maps("/proc/self/maps");
        if (!maps.is_open())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core,
                           "MemoryIntegrity: cannot open /proc/self/maps — code page integrity scanning disabled");
            return;
        }

        std::string line;
        uint32_t regionIndex = 0;

        while (std::getline(maps, line))
        {
            // Format: start-end perms offset dev inode pathname
            // We want 'r-xp' (read + execute, private) regions
            if (line.size() < 20)
                continue;

            // Find permissions field (after first space)
            auto spacePos = line.find(' ');
            if (spacePos == std::string::npos || spacePos + 4 >= line.size())
                continue;

            std::string_view perms(line.data() + spacePos + 1, 4);
            if (perms[0] != 'r' || perms[2] != 'x')
                continue; // Not readable + executable

            // Skip pseudo-regions like [vsyscall], [vdso], [vvar], [stack], [heap].
            // [vsyscall] in particular is mapped r-xp but dereferencing it from user
            // space triggers SIGSEGV on modern kernels (vsyscall=xonly/none). Also skip
            // anonymous mappings (no pathname) since those are typically JIT pages or
            // thread trampolines, not engine code we want to tamper-check.
            std::string_view rest(line.data() + spacePos + 1);
            auto pathPos = rest.find('/');
            if (pathPos == std::string_view::npos)
                continue; // no file-backed path → skip
            std::string_view pathname = rest.substr(pathPos);
            while (!pathname.empty() && (pathname.back() == '\n' || pathname.back() == '\r'))
                pathname.remove_suffix(1);
            if (pathname.empty() || pathname.front() == '[')
                continue;

            // Parse address range
            auto dashPos = line.find('-');
            if (dashPos == std::string::npos || dashPos >= spacePos)
                continue;

            unsigned long long startAddr = 0;
            unsigned long long endAddr = 0;
            try
            {
                startAddr = std::stoull(line.substr(0, dashPos), nullptr, 16);
                endAddr = std::stoull(line.substr(dashPos + 1, spacePos - dashPos - 1), nullptr, 16);
            }
            catch (...)
            {
                continue;
            }

            if (endAddr <= startAddr)
                continue;

            // Belt-and-braces: explicitly skip the vsyscall page by address
            // (0xffffffffff600000) in case /proc/self/maps formatting ever hides the tag.
            if (startAddr >= 0xffffffffff600000ULL)
                continue;

            size_t regionSize = static_cast<size_t>(endAddr - startAddr);
            auto* regionAddr = reinterpret_cast<const void*>(startAddr);

            // Cap region size to avoid hashing huge shared libraries
            static constexpr size_t MAX_AUTO_REGION_SIZE = 4 * 1024 * 1024; // 4 MB
            if (regionSize > MAX_AUTO_REGION_SIZE)
                regionSize = MAX_AUTO_REGION_SIZE;

            std::string name = std::format("code_page_{}", regionIndex++);
            ProtectedRegion region;
            region.name = std::move(name);
            region.address = regionAddr;
            region.size = regionSize;
            region.expectedHash = HashRegion(regionAddr, regionSize);
            m_regions.push_back(std::move(region));
        }

#else
        // No auto-discovery on this platform — rely on manual registration
        SPARK_LOG_INFO(Spark::LogCategory::Core,
                       "MemoryIntegrity: No auto-discovery on this platform, use manual registration");
#endif
    }

} // namespace Spark::Security

/**
 * @file DaemonDiagnostics.cpp
 * @brief Implementation of the daemon stats formatter (see header).
 */

#include "DaemonDiagnostics.h"

#include <array>
#include <cstdio>
#include <string>

namespace
{
    /// Render a byte count as "N.NN MiB" (or appropriate smaller unit) with
    /// one decimal place. Units use binary prefixes (1 KiB = 1024 B) to
    /// match how operators think about cache budgets (`--shader-cache-max-mb`
    /// is interpreted as MiB by the daemon).
    std::string FormatBytes(uint64_t bytes)
    {
        static constexpr std::array<const char*, 5> kUnits{"B", "KiB", "MiB", "GiB", "TiB"};
        double value = static_cast<double>(bytes);
        size_t unit = 0;
        while (value >= 1024.0 && unit + 1 < kUnits.size())
        {
            value /= 1024.0;
            ++unit;
        }
        char buf[48];
        if (unit == 0)
            std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
        else
            std::snprintf(buf, sizeof(buf), "%.1f %s", value, kUnits[unit]);
        return buf;
    }

    /// Render an uptime in seconds as the most natural unit.
    std::string FormatUptime(uint64_t seconds)
    {
        char buf[48];
        if (seconds < 60)
            std::snprintf(buf, sizeof(buf), "%llus", static_cast<unsigned long long>(seconds));
        else if (seconds < 3600)
            std::snprintf(buf, sizeof(buf), "%.1fm", static_cast<double>(seconds) / 60.0);
        else if (seconds < 86400)
            std::snprintf(buf, sizeof(buf), "%.1fh", static_cast<double>(seconds) / 3600.0);
        else
            std::snprintf(buf, sizeof(buf), "%.1fd", static_cast<double>(seconds) / 86400.0);
        return buf;
    }

    /// Render (hits, misses) as "H/M (R%)" with zero rate reported as "—".
    std::string FormatHitRate(uint64_t hits, uint64_t misses)
    {
        char buf[64];
        const uint64_t total = hits + misses;
        if (total == 0)
        {
            std::snprintf(buf, sizeof(buf), "%llu/%llu (—)", static_cast<unsigned long long>(hits),
                          static_cast<unsigned long long>(misses));
        }
        else
        {
            const double rate = static_cast<double>(hits) / static_cast<double>(total) * 100.0;
            std::snprintf(buf, sizeof(buf), "%llu/%llu (%.1f%%)", static_cast<unsigned long long>(hits),
                          static_cast<unsigned long long>(misses), rate);
        }
        return buf;
    }
} // namespace

namespace Spark::Daemon
{

    std::string ServiceIdName(uint16_t id)
    {
        switch (static_cast<ServiceId>(id))
        {
        case ServiceId::Control:
            return "control";
        case ServiceId::Asset:
            return "asset";
        case ServiceId::Shader:
            return "shader";
        case ServiceId::Collab:
            return "collab";
        case ServiceId::Build:
            return "build";
        case ServiceId::Orchestration:
            return "orchestration";
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "unknown(0x%04x)", static_cast<unsigned>(id));
        return buf;
    }

    std::string FormatDaemonStats(const DaemonStatsSnapshot& snap)
    {
        if (snap.socketPath.empty())
            return "daemon: not connected";

        std::string out;
        out.reserve(512);

        out += "daemon: connected\n";
        out += "  socket:   " + snap.socketPath + '\n';
        out += "  version:  " +
               (snap.control.protocolVersion.empty() ? std::string{"(unknown)"} : snap.control.protocolVersion) + '\n';
        out += "  uptime:   " + FormatUptime(snap.control.uptimeSeconds) + '\n';

        out += "  services: ";
        if (snap.control.registeredIds.empty())
        {
            out += "(none)";
        }
        else
        {
            bool first = true;
            for (uint16_t id : snap.control.registeredIds)
            {
                if (!first)
                    out += ", ";
                out += ServiceIdName(id);
                first = false;
            }
        }
        out += '\n';

        auto appendCache = [&out](const char* label, uint64_t entries, uint64_t bytes, uint64_t hits, uint64_t misses,
                                  uint64_t evictions)
        {
            out += '\n';
            out += std::string(label) + ":\n";
            char line[96];
            std::snprintf(line, sizeof(line), "  entries:   %llu\n", static_cast<unsigned long long>(entries));
            out += line;
            out += "  bytes:     " + FormatBytes(bytes) + '\n';
            out += "  hit/miss:  " + FormatHitRate(hits, misses) + '\n';
            std::snprintf(line, sizeof(line), "  evictions: %llu\n", static_cast<unsigned long long>(evictions));
            out += line;
        };

        if (snap.shader)
            appendCache("shader cache", snap.shader->entryCount, snap.shader->totalBytes, snap.shader->hitCount,
                        snap.shader->missCount, snap.shader->evictionCount);

        if (snap.asset)
            appendCache("asset cache", snap.asset->entryCount, snap.asset->totalBytes, snap.asset->hitCount,
                        snap.asset->missCount, snap.asset->evictionCount);

        return out;
    }

    DaemonCacheScope ParseDaemonCacheScope(const std::string& arg)
    {
        if (arg == "shader")
            return DaemonCacheScope::Shader;
        if (arg == "asset")
            return DaemonCacheScope::Asset;
        if (arg == "all")
            return DaemonCacheScope::All;
        return DaemonCacheScope::None;
    }

} // namespace Spark::Daemon

/**
 * @file DaemonDiagnostics.h
 * @brief Human-readable formatter for the SparkDaemon state.
 *
 * Pure helper used by the `daemon.stats` console command. The command
 * handler lives in `DaemonLifecycle.cpp` — it snapshots the live state
 * and calls `FormatDaemonStats()` here. Keeping the formatter in its
 * own translation unit lets unit tests exercise the full output shape
 * without spinning up a daemon connection.
 */

#pragma once

#include "AssetServiceProtocol.h"
#include "DaemonProtocol.h"
#include "ShaderServiceProtocol.h"

#include <optional>
#include <string>

namespace Spark::Daemon
{

    /// Immutable snapshot of everything `FormatDaemonStats` needs to render.
    struct DaemonStatsSnapshot
    {
        /// Socket path of the live connection, or empty if not connected.
        std::string socketPath;
        /// Daemon-level summary from `ControlMessage::StatsResponse`.
        DaemonStats control;
        /// Cache stats from the shader service, if its client is wired.
        std::optional<ShaderCacheStats> shader;
        /// Cache stats from the asset service, if its client is wired.
        std::optional<AssetCacheStats> asset;
    };

    /**
     * @brief Render a snapshot as a multi-line human-readable string.
     *
     * The output shape is stable enough to assert against in tests but is
     * not a wire format — don't try to parse it. If `snapshot.socketPath`
     * is empty, the formatter assumes the engine isn't connected and
     * emits a single-line "daemon: not connected" string, ignoring the
     * other fields.
     */
    [[nodiscard]] std::string FormatDaemonStats(const DaemonStatsSnapshot& snapshot);

    /**
     * @brief Map a `ServiceId` numeric value to a short lowercase name.
     *
     * Used both by the formatter and by any diagnostic code that wants to
     * report registered service IDs symbolically. Unknown IDs render as
     * `"unknown(0xNNNN)"`.
     */
    [[nodiscard]] std::string ServiceIdName(uint16_t id);

} // namespace Spark::Daemon

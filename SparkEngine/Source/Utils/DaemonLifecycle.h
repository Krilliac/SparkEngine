/**
 * @file DaemonLifecycle.h
 * @brief Engine-side startup/shutdown glue for the SparkDaemon IPC layer.
 *
 * Single entry / exit point that the engine's boot sequence calls once the
 * console and gameplay systems are ready. Looks at the `spark.daemon.enabled`
 * CVar; if set, tries to connect via `DaemonConnection::TryConnect`, builds
 * a process-wide `ShaderServiceClient`, and wires it into
 * `Spark::Graphics::GetShaderDiskCache()` via `SetDaemonClient(&client)`.
 *
 * Connection failure is non-fatal — every subsystem that consults the daemon
 * keeps an in-process fallback, and the engine runs identically with or
 * without a running daemon.
 *
 * Shutdown reverses the wiring and calls `DaemonConnection::Shutdown`.
 *
 * Kept behind a single function pair to honour CLAUDE.md's "wire systems in
 * with minimal code — call the real function directly, don't wrap it in
 * another abstraction" guidance. No class, no abstraction layer.
 */

#pragma once

namespace Spark::Daemon
{

    /**
     * @brief Called during engine startup (after InitGameplaySystems).
     *
     * Probes the daemon socket and wires `ShaderDiskCache` into it when the
     * `spark.daemon.enabled` CVar is true and a live daemon is reachable.
     * Safe to call repeatedly; no-op on the second call.
     */
    void InitializeDaemonLifecycle();

    /**
     * @brief Called during engine shutdown (before ShutdownGameplaySystems).
     *
     * Clears the `ShaderDiskCache` daemon client pointer, destroys the
     * process-wide `ShaderServiceClient`, closes the connection.
     * Idempotent.
     */
    void ShutdownDaemonLifecycle();

} // namespace Spark::Daemon

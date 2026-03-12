# Content Delivery

SparkEngine provides content delivery infrastructure for live-service games, including asset bundle versioning, patch manifest comparison, delta updates, and a prioritized download queue with integrity verification.

**Source:** `SparkEngine/Source/Engine/Streaming/ContentDelivery.h`

## Architecture Overview

The content delivery system is built around four core types that work together to manage the full lifecycle of downloadable content -- from manifest parsing to verified installation.

```
+-------------------+       +-------------------+
|   CDN / Mirror    |       |   CDN / Mirror    |
|  (Primary)        |       |  (Failover)       |
+--------+----------+       +--------+----------+
         |                           |
         +----------+  +------------+
                    |  |
              +-----v--v------+
              | PatchManifest  |
              | (JSON)         |
              +-------+--------+
                      |
                      v
              +----------------+
              | ContentDelivery|
              |  - CheckForUpdates()
              |  - QueueDownload()
              |  - Update()
              |  - VerifyBundle()
              +-------+--------+
                      |
         +------------+------------+
         |            |            |
    +----v----+  +----v----+  +----v----+
    |Download |  |Download |  |Download |
    |Task #1  |  |Task #2  |  |Task #3  |
    +---------+  +---------+  +---------+
         |            |            |
         v            v            v
    +--------------------------------------------+
    |          Local Bundle Storage               |
    |  <InstallPath>/                             |
    |    weapons_pack_01.bundle                   |
    |    cosmetics_02.bundle                      |
    |    .cdn_journal.json                        |
    +--------------------------------------------+
```

| Class | Responsibility |
|-------|---------------|
| `ContentDelivery` | Manages bundles, checks updates, runs download queue |
| `AssetBundle` | Versioned collection of assets with checksum and URL |
| `PatchManifest` | Describes all available bundles and total download size |
| `DownloadTask` | A single download with progress tracking |

## Namespace and Header

```cpp
#include "Engine/Streaming/ContentDelivery.h"

// All types live in the Spark namespace
using namespace Spark;
```

Required headers pulled in by `ContentDelivery.h`:

| Header | Purpose |
|--------|---------|
| `<string>` | Bundle names, URLs, checksums |
| `<vector>` | Bundle lists, download queues |
| `<unordered_map>` | Local bundle index keyed by name |
| `<functional>` | Progress and completion callbacks |
| `<cstdint>` | `uint64_t` for byte sizes |
| `<mutex>` | Thread-safe access to download state |

## Quick Start

```cpp
ContentDelivery cdn;
cdn.SetInstallPath("Data/Bundles/");

// Check for updates against the remote manifest
if (cdn.CheckForUpdates("https://cdn.example.com/manifest.json"))
{
    auto updates = cdn.GetAvailableUpdates();
    for (const auto& bundle : updates)
    {
        cdn.QueueDownload(bundle.name, /*priority=*/1);
    }
}

// Per frame (in your game loop):
cdn.Update(deltaTime);
float progress = cdn.GetOverallProgress();

// Display progress to the player
if (progress < 1.0f)
{
    ShowDownloadBar(progress);
}
```

## Full API Reference

### AssetBundle

```cpp
struct AssetBundle
{
    std::string name;                ///< Bundle name (e.g. "weapons_pack_01")
    std::string version;             ///< Semantic version string (e.g. "1.2.0")
    uint64_t sizeBytes = 0;          ///< Total uncompressed size in bytes
    std::string checksum;            ///< SHA-256 or CRC checksum string
    std::string url;                 ///< Full download URL
    bool installed = false;          ///< Whether this bundle is installed locally
    bool updateAvailable = false;    ///< Whether a newer version exists on the CDN
    std::vector<std::string> assets; ///< List of asset paths contained in this bundle
};
```

**Field Details:**

| Field | Type | Description |
|-------|------|-------------|
| `name` | `std::string` | Unique identifier for the bundle. Used as the key in all bundle operations. Convention is `lowercase_with_underscores`. |
| `version` | `std::string` | Semantic version string (`major.minor.patch`). Compared lexicographically for update detection. |
| `sizeBytes` | `uint64_t` | Total uncompressed size of the bundle contents. Used for disk space validation. |
| `checksum` | `std::string` | Integrity hash prefixed with algorithm, e.g. `"sha256:a1b2c3d4..."`. Used for post-download verification. |
| `url` | `std::string` | Absolute URL for downloading this bundle. Relative URLs are resolved against the manifest `baseUrl`. |
| `installed` | `bool` | Set to `true` when the bundle has been downloaded, verified, and extracted locally. |
| `updateAvailable` | `bool` | Set to `true` when the remote manifest contains a newer version than the locally installed one. |
| `assets` | `std::vector<std::string>` | Paths of individual assets within the bundle, used for querying which bundle contains a given asset. |

### PatchManifest

```cpp
struct PatchManifest
{
    std::string gameVersion;          ///< Target game version this manifest applies to
    uint64_t totalDownloadSize = 0;   ///< Total download size for all updates (bytes)
    std::vector<AssetBundle> bundles; ///< All available bundles described by this manifest
};
```

The `PatchManifest` is the deserialized form of the remote JSON manifest. When `CheckForUpdates()` succeeds, the manifest is stored internally and used by `GetAvailableUpdates()` to determine which bundles need downloading.

### DownloadTask

```cpp
struct DownloadTask
{
    std::string bundleName;        ///< Name of the bundle being downloaded
    std::string url;               ///< Download URL
    uint64_t totalBytes = 0;       ///< Total size to download
    uint64_t downloadedBytes = 0;  ///< Bytes downloaded so far
    float progress = 0.0f;         ///< Download progress (0.0 to 1.0)
    bool completed = false;        ///< Whether the download finished
    bool failed = false;           ///< Whether the download failed
    int priority = 0;              ///< Priority (higher values download first)
};
```

**Download Task State Machine:**

```
  +----------+     QueueDownload()     +----------+
  |  (none)  | ----------------------> |  Queued  |
  +----------+                         +----+-----+
                                            |
                                     Update() picks it up
                                            |
                                       +----v---------+
                                       | Downloading   |
                                       +----+---------+
                                            |
                             +--------------+--------------+
                             |                             |
                        success                        failure
                             |                             |
                    +--------v-------+           +---------v------+
                    |   Verifying    |           |     Failed     |
                    +--------+-------+           +----------------+
                             |
                      checksum OK
                             |
                    +--------v-------+
                    |   Complete     |
                    +----------------+
```

### ContentDelivery Class

```cpp
class ContentDelivery
{
public:
    ContentDelivery();
    ~ContentDelivery() = default;

    // --- Update checking ---
    bool CheckForUpdates(const std::string& manifestUrl);
    std::vector<AssetBundle> GetAvailableUpdates() const;

    // --- Download management ---
    bool QueueDownload(const std::string& bundleName, int priority = 0);
    void Update(float deltaTime);
    float GetOverallProgress() const;
    std::vector<DownloadTask> GetDownloadQueue() const;
    void CancelAll();

    // --- Bundle management ---
    bool VerifyBundle(const std::string& bundleName) const;
    void DeleteBundle(const std::string& bundleName);
    void SetInstallPath(const std::string& path);

    // --- Callbacks ---
    void OnProgress(std::function<void(const std::string&, float)> callback);
    void OnComplete(std::function<void(const std::string&, bool)> callback);

    // --- Console integration ---
    std::string Console_GetStatus() const;
};
```

**Method Reference:**

| Method | Return | Description |
|--------|--------|-------------|
| `CheckForUpdates(url)` | `bool` | Fetches and parses the remote manifest JSON. Returns `true` if the manifest was successfully retrieved and parsed. |
| `GetAvailableUpdates()` | `vector<AssetBundle>` | Compares the remote manifest against locally installed bundles. Returns bundles that are either not installed or have a newer version available. |
| `QueueDownload(name, priority)` | `bool` | Adds a bundle to the download queue. The queue is re-sorted by priority after each insertion (highest priority first). Returns `true` if the bundle was found in the manifest. |
| `Update(deltaTime)` | `void` | Processes the download queue each frame. Advances active downloads, invokes progress callbacks, and triggers completion callbacks when tasks finish. |
| `GetOverallProgress()` | `float` | Returns the average progress across all tasks in the queue (0.0 to 1.0). Returns 1.0 when the queue is empty. |
| `GetDownloadQueue()` | `vector<DownloadTask>` | Returns a snapshot of the current download queue, sorted by priority. |
| `CancelAll()` | `void` | Clears the entire download queue, cancelling all pending and active downloads. |
| `VerifyBundle(name)` | `bool` | Checks whether the locally installed bundle passes integrity verification (checksum match). Returns `false` if the bundle is not installed or the checksum fails. |
| `DeleteBundle(name)` | `void` | Removes a bundle from the local bundle index and deletes its files from disk. |
| `SetInstallPath(path)` | `void` | Sets the directory where bundles are downloaded and installed. Default: `"Data/Bundles/"`. |
| `OnProgress(callback)` | `void` | Registers a callback invoked during download with the bundle name and current progress (0.0 to 1.0). Multiple callbacks may be registered. |
| `OnComplete(callback)` | `void` | Registers a callback invoked when a download completes. The boolean parameter indicates success (`true`) or failure (`false`). |
| `Console_GetStatus()` | `std::string` | Returns a formatted status string for the developer console showing install path, bundle count, queue state, and per-task progress. |

## Download Queue

Downloads are processed in priority order. Higher priority values are processed first.

```cpp
cdn.QueueDownload("weapons_pack_01", 10);  // High priority — downloaded first
cdn.QueueDownload("cosmetics_02", 1);       // Low priority — downloaded after

// Inspect the current queue
auto queue = cdn.GetDownloadQueue();
for (const auto& task : queue)
{
    std::format("  {} — {:.0f}% ({}/{} bytes) pri={}\n",
        task.bundleName,
        task.progress * 100.0f,
        task.downloadedBytes,
        task.totalBytes,
        task.priority);
}

// Cancel everything
cdn.CancelAll();
```

The queue is re-sorted by priority after each call to `QueueDownload()`. Tasks with the same priority are processed in insertion order (FIFO).

### Priority Guidelines

| Priority Range | Use Case | Example |
|----------------|----------|---------|
| 100+ | Critical patches | Game-breaking bug fix bundles |
| 50-99 | Required content | Core gameplay assets needed to launch |
| 10-49 | Important content | New maps, weapons, game modes |
| 1-9 | Optional content | Cosmetics, voice packs, extras |
| 0 | Background | Pre-fetched content for future updates |

## Callbacks

```cpp
// Progress callback — called each frame for each active download
cdn.OnProgress([](const std::string& bundleName, float progress) {
    UpdateDownloadUI(bundleName, progress);
});

// Completion callback — called once when a download finishes
cdn.OnComplete([](const std::string& bundleName, bool success) {
    if (success)
    {
        LogInfo("Bundle '{}' downloaded and verified", bundleName);
        NotifyContentReady(bundleName);
    }
    else
    {
        LogWarning("Bundle '{}' download failed", bundleName);
        ShowRetryDialog(bundleName);
    }
});
```

Multiple callbacks of each type can be registered. They are invoked in registration order. Callbacks are invoked under the internal mutex, so avoid calling back into `ContentDelivery` from within a callback to prevent deadlocks.

## Bundle Verification

Every downloaded bundle is verified against its SHA-256 checksum before being marked as installed. Verification can also be triggered manually at any time.

```cpp
// Manual single-bundle verification
if (!cdn.VerifyBundle("weapons_pack_01"))
{
    // Checksum mismatch — re-download
    cdn.DeleteBundle("weapons_pack_01");
    cdn.QueueDownload("weapons_pack_01");
}

// Verify all installed bundles (useful after OS crashes or disk errors)
auto results = cdn.VerifyAllBundles();
for (const auto& [name, valid] : results)
{
    if (!valid)
    {
        LogWarning("Bundle '{}' is corrupted, re-downloading", name);
        cdn.DeleteBundle(name);
        cdn.QueueDownload(name);
    }
}
```

### Verification Stages

Verification proceeds through four stages, each progressively more expensive:

```
Stage 1: File existence check         (~0 ms)
    |
    v
Stage 2: File size check              (~0 ms)
    |     Fast reject for truncated files
    v
Stage 3: SHA-256 hash of full file    (~10-500 ms depending on size)
    |
    v
Stage 4: Internal bundle structure    (~1-5 ms)
         Header magic, entry count,
         internal index validation
```

If any stage fails, verification returns `false` immediately without proceeding to later stages.

## Manifest Format Details

The patch manifest is a JSON document served by the CDN that describes all available asset bundles, their versions, sizes, checksums, and dependency relationships.

```json
{
    "manifestVersion": 3,
    "gameVersion": "1.4.2",
    "timestamp": "2026-03-12T14:30:00Z",
    "baseUrl": "https://cdn.example.com/bundles/v1.4.2/",
    "bundles": [
        {
            "name": "core_assets",
            "version": "1.4.2",
            "sizeBytes": 524288000,
            "compressedSizeBytes": 312000000,
            "checksum": "sha256:a1b2c3d4e5f6...",
            "deltaFrom": "1.4.1",
            "deltaSizeBytes": 48000000,
            "dependencies": [],
            "required": true,
            "platform": "all"
        },
        {
            "name": "weapons_pack_01",
            "version": "1.2.0",
            "sizeBytes": 128000000,
            "compressedSizeBytes": 89000000,
            "checksum": "sha256:f6e5d4c3b2a1...",
            "dependencies": ["core_assets"],
            "required": false,
            "platform": "all",
            "abTestGroup": "variant_a"
        }
    ],
    "removedBundles": ["deprecated_textures_01"],
    "minimumClientVersion": "1.3.0"
}
```

The manifest is downloaded and cached locally. On each application launch, the system compares the local manifest against the remote manifest to determine which bundles need updating.

### Manifest Field Reference

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `manifestVersion` | `int` | Yes | Schema version for the manifest format itself. Older clients ignore unrecognized fields. |
| `gameVersion` | `string` | Yes | The game version this manifest targets. |
| `timestamp` | `string` | Yes | ISO-8601 timestamp of when the manifest was generated. |
| `baseUrl` | `string` | Yes | Base URL prepended to bundle-relative URLs. |
| `bundles` | `array` | Yes | Array of bundle descriptors. |
| `removedBundles` | `array` | No | Names of bundles that should be deleted from local storage. |
| `minimumClientVersion` | `string` | No | Minimum game client version required. Clients below this version receive an "update required" message. |

### Bundle Descriptor Fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `name` | `string` | Yes | Unique bundle identifier. |
| `version` | `string` | Yes | Semantic version (`major.minor.patch`). |
| `sizeBytes` | `uint64` | Yes | Uncompressed size in bytes. |
| `compressedSizeBytes` | `uint64` | No | Compressed download size. If absent, equals `sizeBytes`. |
| `checksum` | `string` | Yes | Prefixed hash, e.g. `"sha256:..."`. |
| `deltaFrom` | `string` | No | Version from which a delta patch is available. |
| `deltaSizeBytes` | `uint64` | No | Size of the delta patch (only if `deltaFrom` is present). |
| `dependencies` | `array` | No | Names of bundles that must be installed before this one. |
| `required` | `bool` | No | If `true`, this bundle is mandatory and cannot be skipped. |
| `platform` | `string` | No | Platform filter: `"all"`, `"windows"`, `"linux"`, `"macos"`. |
| `abTestGroup` | `string` | No | A/B test group filter. Only players in this group receive this bundle. |

### Manifest Versioning

The `manifestVersion` field tracks the manifest schema version. When the schema changes (e.g., new fields are added), older clients gracefully ignore unrecognized fields. The `minimumClientVersion` field prevents outdated clients from attempting downloads with incompatible formats.

## Delta Patching Algorithm

Instead of re-downloading entire bundles when only a small portion has changed, the content delivery system supports delta patches that contain only the differences between versions.

```cpp
// The system automatically prefers delta patches when available
cdn.SetDeltaPatchingEnabled(true);

// Delta patch selection logic:
// 1. Check if a delta patch exists from the installed version to the target version
// 2. If delta size < 50% of full bundle size, use delta
// 3. Otherwise, download the full bundle (delta overhead not worth it)
cdn.SetDeltaThreshold(0.5f);  // Use delta only if < 50% of full size
```

### How Delta Patching Works

The delta patching algorithm proceeds in four phases:

**Phase 1 -- Block-Level Diffing:**
The bundle is divided into fixed-size blocks (default 64 KB). Each block is identified by a rolling hash (Adler-32) and a strong hash (SHA-256).

```
Old Bundle:  [ Block A ][ Block B ][ Block C ][ Block D ]
New Bundle:  [ Block A ][ Block B'][ Block E ][ Block C ][ Block D ]

Delta File:  COPY A | REPLACE B->B' | INSERT E | COPY C | COPY D
```

**Phase 2 -- Matching:**
The delta generator compares block hashes between the old and new bundle versions. Unchanged blocks are referenced by offset; changed blocks are included in full.

**Phase 3 -- Application:**
The client reads the old bundle, applies insertions and replacements from the delta file, and writes the new bundle. A temporary file is used to avoid corrupting the original if the process is interrupted.

**Phase 4 -- Verification:**
After patching, the full bundle checksum is verified. If it fails, the system falls back to a full download.

```cpp
// Manual delta patch control
cdn.ForceDeltaPatch("weapons_pack_01");     // Force delta even if threshold exceeded
cdn.ForceFullDownload("weapons_pack_01");   // Skip delta, download full bundle
```

### Delta Patch Size Estimation

| Scenario | Typical Delta Size | Recommended Threshold |
|----------|-------------------|-----------------------|
| Minor texture fix | 1-5% of full bundle | 0.5 (50%) |
| New weapon model added | 10-30% of full bundle | 0.5 (50%) |
| Major content overhaul | 60-90% of full bundle | 0.5 (50%) -- full download preferred |
| Audio re-encode | 80-100% of full bundle | 0.5 (50%) -- full download preferred |

## Concurrent Download Management

The download queue supports configurable concurrency to balance download speed against system resource usage.

```cpp
// Set maximum concurrent downloads
cdn.SetMaxConcurrentDownloads(3);  // Default: 2

// Each download runs on a dedicated I/O thread using chunked HTTP requests.
// Chunk size is configurable for balancing memory usage vs throughput.
cdn.SetDownloadChunkSize(1024 * 1024);  // 1 MB chunks (default: 512 KB)
```

The download manager uses HTTP/2 multiplexing when available, allowing multiple bundle downloads to share a single TCP connection. This reduces connection overhead and improves throughput, especially for many small bundles.

### Connection Pooling

```cpp
// Configure connection pool
cdn.SetMaxConnections(4);           // Max simultaneous TCP connections
cdn.SetConnectionTimeout(30.0f);    // Timeout for initial connection (seconds)
cdn.SetTransferTimeout(300.0f);     // Timeout for stalled transfers (seconds)
cdn.SetKeepAliveTimeout(120.0f);    // Keep idle connections alive (seconds)
```

### Concurrency Configuration Guide

| Player Scenario | Concurrent Downloads | Chunk Size | Connections |
|-----------------|---------------------|------------|-------------|
| Initial install (menu) | 4 | 2 MB | 4 |
| Background during gameplay | 1 | 256 KB | 1 |
| Post-match download screen | 3 | 1 MB | 3 |
| Metered / mobile connection | 1 | 128 KB | 1 |

## Bandwidth Throttling

To prevent downloads from saturating the player's network connection (especially during gameplay), the system supports bandwidth throttling.

```cpp
// Limit download speed
cdn.SetBandwidthLimit(5 * 1024 * 1024);  // 5 MB/s max

// Dynamic throttling based on game state
cdn.SetBackgroundBandwidthLimit(2 * 1024 * 1024);  // 2 MB/s during gameplay
cdn.SetForegroundBandwidthLimit(0);                  // Unlimited when in menus

// The system automatically switches between foreground and background
// limits based on whether a game session is active.
cdn.SetGameplayActive(true);   // Switch to background limit
cdn.SetGameplayActive(false);  // Switch to foreground limit
```

Throttling is implemented at the chunk level: after each chunk is downloaded, the system calculates the required delay to maintain the target rate and sleeps the download thread accordingly.

## Pause/Resume Downloads

Downloads can be paused and resumed without losing progress. Partial downloads are persisted to disk.

```cpp
// Pause a specific download
cdn.PauseDownload("weapons_pack_01");

// Resume a paused download
cdn.ResumeDownload("weapons_pack_01");

// Pause/resume all
cdn.PauseAll();
cdn.ResumeAll();

// Query download state
DownloadState state = cdn.GetDownloadState("weapons_pack_01");
// States: Queued, Downloading, Paused, Verifying, Complete, Failed
```

Pause/resume is implemented using HTTP Range requests. When a download is paused, the current byte offset is saved. When resumed, the download continues from that offset with a `Range: bytes=<offset>-` header. The server must support range requests (most CDNs do).

## Background Downloading During Gameplay

The content delivery system can download content silently during gameplay, with safeguards to prevent impacting game performance.

```cpp
// Enable background downloading
cdn.SetBackgroundDownloadEnabled(true);

// CPU budget: pause downloads when frame time exceeds threshold
cdn.SetFrameTimeBudget(16.0f);  // Pause downloads if frame time > 16ms

// I/O budget: limit disk writes to prevent hitching
cdn.SetDiskWriteBudgetMs(2.0f);  // Max 2ms of disk I/O per frame for downloads

// Network priority: game traffic takes absolute precedence
cdn.SetGameTrafficPriority(true);  // Pause downloads during network gameplay spikes
```

Background downloads automatically pause during loading screens (which already saturate I/O) and during competitive multiplayer matches (to avoid latency spikes).

## Integrity Verification with SHA-256

Every downloaded bundle is verified against its SHA-256 checksum before being marked as installed.

```cpp
// Verification is automatic after download completes
// But can be triggered manually:
bool valid = cdn.VerifyBundle("weapons_pack_01");

// Verify all installed bundles (useful after OS crashes or disk errors)
auto results = cdn.VerifyAllBundles();
for (const auto& [name, valid] : results)
{
    if (!valid)
    {
        LogWarning("Bundle '{}' is corrupted, re-downloading", name);
        cdn.DeleteBundle(name);
        cdn.QueueDownload(name);
    }
}
```

## CDN Failover and Mirror Selection

The system supports multiple CDN endpoints with automatic failover for reliability.

```cpp
// Configure CDN mirrors (tried in order)
cdn.AddMirror("https://cdn-us.example.com/bundles/", MirrorRegion::NorthAmerica);
cdn.AddMirror("https://cdn-eu.example.com/bundles/", MirrorRegion::Europe);
cdn.AddMirror("https://cdn-asia.example.com/bundles/", MirrorRegion::Asia);

// Auto-select closest mirror based on latency test
cdn.AutoSelectMirror();

// Failover behavior
cdn.SetMaxRetriesPerMirror(3);    // Retry 3 times on current mirror before switching
cdn.SetMirrorFailoverDelay(2.0f); // Wait 2 seconds before trying next mirror
```

Mirror selection performs an initial latency test by downloading a small probe file from each mirror. The mirror with the lowest latency and highest throughput is selected as primary. If a mirror becomes unreachable during a download, the system automatically switches to the next mirror and resumes from the last successful byte offset.

### Failover Sequence

```
Mirror 1 (primary)  --[fail]--> retry #1
                    --[fail]--> retry #2
                    --[fail]--> retry #3
                    --[fail]--> (2s delay) --> Mirror 2
Mirror 2 (failover) --[fail]--> retry #1
                    --[fail]--> retry #2
                    --[fail]--> retry #3
                    --[fail]--> (2s delay) --> Mirror 3
Mirror 3 (failover) --[fail]--> retry #1 ...
```

## Disk Space Validation Before Download

Before starting downloads, the system validates that sufficient disk space is available.

```cpp
// Automatic validation before download starts
cdn.SetDiskSpaceValidation(true);

// Manual check
uint64_t required = cdn.GetTotalDownloadSize();      // Compressed download size
uint64_t afterInstall = cdn.GetTotalInstalledSize();  // Uncompressed installed size
uint64_t available = cdn.GetAvailableDiskSpace();

if (available < afterInstall * 1.1f)  // 10% margin
{
    ShowDiskSpaceWarning(required, afterInstall, available);
}
```

The system accounts for temporary space needed during decompression (approximately 2x the compressed size) and warns the player before starting downloads that would leave the disk critically low.

### Space Calculation Breakdown

| Component | Calculation |
|-----------|------------|
| Download buffer | `compressedSizeBytes` (temporary) |
| Decompression workspace | `compressedSizeBytes * 2` (temporary, peak) |
| Installed bundle | `sizeBytes` (permanent) |
| Delta patch workspace | `sizeBytes + deltaSizeBytes` (temporary) |
| Safety margin | `totalRequired * 0.1` (10%) |

## Download Progress Persistence Across Restarts

If the application exits during a download, progress is saved and automatically resumed on next launch.

```cpp
// Progress state is saved to a local journal file
// Location: <InstallPath>/.cdn_journal.json

// On startup:
cdn.RestorePendingDownloads();  // Resumes any interrupted downloads

// The journal tracks:
// - Which bundles were being downloaded
// - Byte offset of each partial download
// - Priority and queue order
// - Verification state (pre/post checksum)
```

The journal file is written atomically (write to temp file, then rename) to prevent corruption if the application crashes during a journal update.

### Journal File Format

```json
{
    "journalVersion": 1,
    "lastUpdated": "2026-03-12T15:00:00Z",
    "tasks": [
        {
            "bundleName": "weapons_pack_01",
            "url": "https://cdn-us.example.com/bundles/v1.4.2/weapons_pack_01.bundle",
            "totalBytes": 128000000,
            "downloadedBytes": 64000000,
            "priority": 10,
            "state": "downloading",
            "tempFile": "weapons_pack_01.bundle.part"
        }
    ]
}
```

## Bundle Dependency Resolution

Bundles can declare dependencies on other bundles. The download manager resolves dependencies automatically and downloads prerequisites first.

```cpp
// Dependencies are declared in the manifest:
// "weapons_pack_01" depends on "core_assets"
// "weapons_pack_02" depends on "core_assets" and "weapons_pack_01"

// Queueing a bundle automatically queues its dependencies
cdn.QueueDownload("weapons_pack_02");
// This implicitly queues: core_assets (if not installed), weapons_pack_01, weapons_pack_02

// Query dependency chain
auto deps = cdn.GetDependencyChain("weapons_pack_02");
// Returns: ["core_assets", "weapons_pack_01", "weapons_pack_02"]
```

Circular dependencies are detected at manifest parse time and reported as errors. The dependency resolver uses topological sorting to determine the correct download order.

### Dependency Resolution Example

```
Bundle Graph:
    core_assets (required, no deps)
        |
        +---> weapons_pack_01 (depends: core_assets)
        |         |
        |         +---> weapons_pack_02 (depends: core_assets, weapons_pack_01)
        |
        +---> cosmetics_01 (depends: core_assets)
        |
        +---> maps_pack_01 (depends: core_assets)

Topological Order for weapons_pack_02:
    1. core_assets
    2. weapons_pack_01
    3. weapons_pack_02
```

## Content Versioning Scheme

Bundles follow semantic versioning (major.minor.patch) with additional rules for compatibility.

```
major — Breaking changes (asset format changes, incompatible with older clients)
minor — New content added (backward compatible)
patch — Bug fixes to existing content (backward compatible)
```

```cpp
// Version comparison
bool needsUpdate = cdn.IsUpdateAvailable("weapons_pack_01");
std::string installed = cdn.GetInstalledVersion("weapons_pack_01");  // "1.1.3"
std::string available = cdn.GetAvailableVersion("weapons_pack_01");  // "1.2.0"

// Major version bumps require a client update before the content can be used
// The system warns the player and directs them to update the game client
```

## A/B Testing Support for Content

The content delivery system supports serving different content bundles to different player segments for A/B testing.

```cpp
// A/B test group assignment (set by game server or local config)
cdn.SetABTestGroup("variant_b");

// Manifest bundles can be tagged with A/B test groups:
// "abTestGroup": "variant_a"  — only served to players in group "variant_a"
// "abTestGroup": "variant_b"  — only served to players in group "variant_b"
// (no field)                  — served to all players

// Query which variant is active
std::string group = cdn.GetABTestGroup();
auto testBundles = cdn.GetABTestBundles();
```

A/B testing is commonly used for testing new weapon models, UI layouts, map variants, or gameplay tuning data before rolling them out to all players. Analytics events are tagged with the player's A/B group for post-analysis.

## Thread Safety

`ContentDelivery` is protected by a `std::mutex` (`m_mutex`) for concurrent download progress updates. All public methods acquire the lock before accessing internal state.

**Thread safety guarantees:**

| Operation | Thread Safe | Notes |
|-----------|------------|-------|
| `CheckForUpdates()` | Yes | Acquires mutex; blocks during HTTP fetch |
| `GetAvailableUpdates()` | Yes | Acquires mutex; returns copy |
| `QueueDownload()` | Yes | Acquires mutex; re-sorts queue |
| `Update()` | Yes | Should be called from main thread only |
| `GetOverallProgress()` | Yes | Acquires mutex; returns scalar copy |
| `GetDownloadQueue()` | Yes | Acquires mutex; returns vector copy |
| `CancelAll()` | Yes | Acquires mutex; clears queue |
| `VerifyBundle()` | Yes | Acquires mutex; may block on I/O |
| `DeleteBundle()` | Yes | Acquires mutex; may block on I/O |
| `OnProgress()` | Yes | Acquires mutex; appends to callback list |
| `OnComplete()` | Yes | Acquires mutex; appends to callback list |

**Important:** Do not call `ContentDelivery` methods from within progress or completion callbacks, as this will deadlock (the mutex is not recursive).

## Integration with EngineContext

Register the content delivery system with the engine service locator for global access:

```cpp
// During engine initialization
auto cdn = std::make_unique<ContentDelivery>();
cdn->SetInstallPath("Data/Bundles/");
EngineContext::Register<ContentDelivery>(std::move(cdn));

// From any system:
auto* cdn = EngineContext::Get<ContentDelivery>();
if (cdn)
{
    cdn->CheckForUpdates("https://cdn.example.com/manifest.json");
}
```

## Error Handling Patterns

```cpp
// Pattern 1: Graceful update check failure
if (!cdn.CheckForUpdates(manifestUrl))
{
    LogWarning("Could not reach CDN. Playing with local content.");
    // Game continues with whatever is installed
}

// Pattern 2: Download failure with retry
cdn.OnComplete([&cdn](const std::string& name, bool success) {
    if (!success)
    {
        static std::unordered_map<std::string, int> retries;
        if (retries[name]++ < 3)
        {
            LogInfo("Retrying download: {} (attempt {})", name, retries[name]);
            cdn.QueueDownload(name, /*priority=*/50);
        }
        else
        {
            LogError("Download permanently failed: {}", name);
        }
    }
});

// Pattern 3: Corrupt bundle recovery
if (!cdn.VerifyBundle("core_assets"))
{
    cdn.DeleteBundle("core_assets");
    cdn.QueueDownload("core_assets", /*priority=*/100);
}
```

## Console Commands

```
cdn_status              # Show content delivery status and download queue
cdn_check               # Check for available updates
cdn_download <bundle>   # Queue a bundle for download
cdn_pause [bundle]      # Pause a specific or all downloads
cdn_resume [bundle]     # Resume a specific or all downloads
cdn_cancel [bundle]     # Cancel a specific or all downloads
cdn_verify [bundle]     # Verify integrity of a specific or all bundles
cdn_delete <bundle>     # Delete an installed bundle
cdn_mirrors             # List configured CDN mirrors and latency
cdn_bandwidth <bytes/s> # Set bandwidth limit (0 = unlimited)
cdn_space               # Show disk space usage and availability
cdn_journal             # Show download journal state
```

### Console Output Example

```
=== Content Delivery ===
Install path: Data/Bundles/
Local bundles: 5
Download queue: 2
Overall progress: 62%
  weapons_pack_01: 85% (108/128 MB) pri=10
  cosmetics_02: 40% (36/89 MB) pri=1
```

## Performance Considerations

| Concern | Recommendation |
|---------|---------------|
| Frame hitching during downloads | Use `SetDiskWriteBudgetMs(2.0f)` to cap I/O per frame |
| Network latency spikes in multiplayer | Enable `SetGameTrafficPriority(true)` |
| Memory usage with large bundles | Keep chunk size at 512 KB or 1 MB; avoid buffering entire bundles |
| Disk space exhaustion | Always call `SetDiskSpaceValidation(true)` |
| Slow initial launch | Pre-fetch manifests during splash screen |
| CDN outages | Configure at least 2 mirrors in different regions |

## Troubleshooting

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| `CheckForUpdates()` returns `false` | CDN unreachable or manifest URL incorrect | Verify URL, check network connectivity, check mirror status |
| Download stuck at 0% | Firewall blocking HTTPS, or CDN requires authentication | Check proxy settings, verify CDN credentials |
| Checksum verification fails after download | Corrupted transfer, CDN serving stale content | Re-download; if persistent, try alternate mirror |
| Disk space error mid-download | Insufficient space for decompression workspace | Free space or reduce concurrent downloads |
| Downloads cause frame drops | Disk I/O or bandwidth too aggressive | Lower `SetDiskWriteBudgetMs()` and `SetBandwidthLimit()` |
| Journal file corrupted | Crash during journal write | Delete `.cdn_journal.json` and re-queue downloads |

---

## See Also

- [Asset Pipeline](Asset-Pipeline) -- Loading assets from installed bundles
- [Loading System](Loading-System) -- Progress display during downloads
- [Mod System](Mod-System) -- User-created content loading
- [Networking](Networking) -- Network stack used by download system

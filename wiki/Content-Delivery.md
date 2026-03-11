# Content Delivery

SparkEngine provides content delivery infrastructure for live-service games, including asset bundle versioning, patch manifest comparison, delta updates, and a prioritized download queue with integrity verification.

**Source:** `SparkEngine/Source/Engine/Streaming/ContentDelivery.h`

## Overview

| Class | Responsibility |
|-------|---------------|
| `ContentDelivery` | Manages bundles, checks updates, runs download queue |
| `AssetBundle` | Versioned collection of assets with checksum and URL |
| `PatchManifest` | Describes all available bundles and total download size |
| `DownloadTask` | A single download with progress tracking |

## Quick Start

```cpp
ContentDelivery cdn;
cdn.SetInstallPath("Data/Bundles/");

// Check for updates
if (cdn.CheckForUpdates("https://cdn.example.com/manifest.json")) {
    auto updates = cdn.GetAvailableUpdates();
    for (const auto& bundle : updates) {
        cdn.QueueDownload(bundle.name, /*priority=*/1);
    }
}

// Per frame:
cdn.Update(deltaTime);
float progress = cdn.GetOverallProgress();
```

## Asset Bundles

```cpp
struct AssetBundle {
    std::string name;       // e.g. "weapons_pack_01"
    std::string version;    // Semantic version
    uint64_t sizeBytes;     // Total size
    std::string checksum;   // SHA-256 or CRC
    std::string url;        // Download URL
    bool installed;         // Locally installed
    bool updateAvailable;   // Newer version exists
};
```

## Download Queue

Downloads are processed in priority order:

```cpp
cdn.QueueDownload("weapons_pack_01", 10);  // High priority
cdn.QueueDownload("cosmetics_02", 1);       // Low priority

auto queue = cdn.GetDownloadQueue();
cdn.CancelAll();  // Cancel all pending downloads
```

## Callbacks

```cpp
cdn.OnProgress([](const std::string& bundleName, float progress) {
    UpdateDownloadUI(bundleName, progress);
});

cdn.OnComplete([](const std::string& bundleName, bool success) {
    if (success) NotifyContentReady(bundleName);
});
```

## Bundle Verification

```cpp
if (!cdn.VerifyBundle("weapons_pack_01")) {
    // Checksum mismatch — re-download
    cdn.DeleteBundle("weapons_pack_01");
    cdn.QueueDownload("weapons_pack_01");
}
```

## Thread Safety

`ContentDelivery` is protected by a mutex for concurrent download progress updates.

## Console Commands

```
cdn_status    # Show content delivery status and download queue
```

---

## See Also

- [Asset Pipeline](Asset-Pipeline) — Loading assets from installed bundles
- [Loading System](Loading-System) — Progress display during downloads
- [Mod System](Mod-System) — User-created content loading

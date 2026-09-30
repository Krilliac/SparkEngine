#pragma once

#include <map>
#include <string>

namespace SparkInstaller
{
    // On-disk record placed inside an installed engine tree at .sparkengine-install.json.
    // Minimal JSON, hand-written — no external dependency.
    //
    // Both markers live in an install tree the user (or anything else) can edit,
    // so their readers are strict and bounded: Load accepts only the flat object
    // Save writes (at most 64 KiB, each key once, schema exactly 1, only the
    // escapes Save produces). Save refuses output above that same limit or
    // strings containing controls it cannot encode. ReadPendingMarker reads
    // the ref and commit values WritePendingMarker writes (at most 4 KiB),
    // rejecting duplicate or unknown nonempty lines. A rejected read leaves
    // its outputs unchanged. The functions keep no shared state; the installer
    // calls them from the one thread that runs Installer::Run.
    struct InstallState
    {
        int schema = 1;
        std::string ref;
        std::string commit;
        std::string destination;
        std::string generator;
        std::string buildType;
        std::string builtAt;
        std::string installerVersion;
        std::map<std::string, bool> options;

        // Filename located at <destination>/.sparkengine-install.json.
        static std::string FileName() { return ".sparkengine-install.json"; }
        // Written into a fresh clone before it is activated at the destination
        // and removed only after the first build succeeds and the install state
        // is saved. Its presence without an install state means Resume install.
        static std::string PendingFileName() { return ".sparkengine-install.pending"; }
        // Written when a failed update could not restore and rebuild the
        // previous commit; the tree's binaries do not match its source.
        static std::string RepairRequiredFileName() { return ".sparkengine-install.repair-required"; }

        static bool Load(const std::string& destination, InstallState& out);
        bool Save(const std::string& destination) const;
        static bool Exists(const std::string& destination);

        // The pending marker holds the ref and the exact commit that was cloned,
        // one "key=value" line each. The writer refuses empty values and values
        // holding CR, LF or NUL, so every marker it writes reads back unchanged.
        static bool WritePendingMarker(const std::string& tree, const std::string& ref, const std::string& commit);
        static bool ReadPendingMarker(const std::string& tree, std::string& ref, std::string& commit);
    };
} // namespace SparkInstaller

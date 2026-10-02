# SparkInstaller

Standalone bootstrap installer and updater for SparkEngine.

Download it from the project's Releases page, run it, pick your options — it
clones the engine repository with all submodules, configures CMake to your
taste, and builds the engine on your machine.

## What it does

**Install mode** (empty destination):

1. Detect Git — on Windows, auto-download [MinGit][mingit] if absent; on
   Linux/macOS, print install instructions and exit cleanly.
2. Verify CMake 3.25+ runs from PATH (or from `CMakePath` in the SparkBuild
   configuration). The installer does not download CMake.
3. Prompt for destination directory and ref (branch / tag).
4. Let you pick a build preset: **Defaults**, **All on**, **Minimal**,
   **Linux-friendly**, **Shipping**, or **Development**.
5. `git clone --recurse-submodules --branch <ref>` the repository into a
   hidden sibling staging directory, `.<name>.sparkinstall-<pid>-<stamp>`, on
   the same volume as the destination. A failed or interrupted clone is removed
   and the destination is never created or changed.
6. Write `.sparkengine-install.pending` (the ref and the cloned commit) into the
   staging tree, then activate it: remove the empty destination directory, if
   there is one, and rename the staging tree into its place. A destination
   that is no longer empty is never overwritten; the run exits 4 and keeps the
   staging tree, whose path it reports.
7. Run `cmake` configure + build for the chosen options.
8. Write `.sparkengine-install.json` into the install root to mark it, then
   remove the pending marker. That removal is the install's commit point.

**Resume install mode** (destination holds `.sparkengine-install.pending` and
no `.sparkengine-install.json`): an earlier install activated its clone but its
configure or build failed or was interrupted, or it ran with `--skip-build`.
The installer never updates such a tree. It refuses to resume when `--ref`
differs from the pending ref (exit 4) or when `HEAD` is no longer the cloned
commit (exit 5). Otherwise it skips clone and fetch, reruns configure + build,
and then records the install as in steps 7 and 8. A failed resume keeps the
pending marker.

**Update mode** (destination contains an existing install):

1. Validate the prior `.sparkengine-install.json`; a marker that exists but
   does not parse refuses the update. The TUI and GUI wizards also read it to
   recover ref + options. A `--headless` update does not: it uses `--ref`
   (default `Working`) and the default build options.
2. Verify the existing checkout has no tracked or untracked changes; refuse the
   update when local changes could make rollback ambiguous.
3. Refuse an already inconsistent legacy install whose `HEAD` differs from its
   recorded build; preserve it for explicit repair rather than rebuilding it in place.
4. Clone the requested ref into a unique sibling, then fetch, check out the ref,
   and update submodules there. The live checkout is not fetched or checked out.
5. Configure and build in that sibling. Build paths must be below the install
   root and are remapped into staging; an explicit `-B` also overrides a preset's
   binary directory. Verify that `HEAD` still matches the staged commit, then
   preserve Git-listed ignored regular user files outside the build directories,
   then write and reload the install state with the final destination recorded.
   Linked files, special files, or collisions with the new tree refuse the update
   without replacing the working install.
6. Under an OS-held destination lock, rename the live tree to
   `<dest>.sparkinstall-previous-pending`, then rename staging to the destination.
   A failed second rename attempts to restore the previous tree immediately.
   A successful swap retains the previous tree under a unique
   `<dest>.sparkinstall-previous-*` name, including its original build outputs
   and ignored user files. It is never automatically deleted.

A fetch, checkout, configure, build, commit-verification or state-write failure
before activation leaves the existing install and its binaries unchanged; no
rollback rebuild runs. A process killed during staging can leave a staging
sibling, but it cannot replace the live install. On the next run, a pending
previous tree is restored if the destination is absent, or archived if a
verified replacement is already present. Ambiguous recovery exits **9** and
preserves both trees. The two renames are recoverable, not one atomic filesystem
operation; there can be a brief interval with no destination directory.

An update with `--skip-build` keeps its clone in staging and reports its path;
it never activates unverified source over a working install. Each later update
builds a fresh sibling. CMake's cache still names the original staging directory
after activation and must not be reused for an in-place rebuild. Build RPATHs
use origin-relative entries where CMake supports them. Runtime relocation still
requires native qualification. Git's ignored-file inventory determines retained
user files; the configured build directory, root `build/`, Git metadata, and
installer markers are excluded from copying. The complete previous tree also
retains these files for rollback. Active applications should be stopped during
an update so they cannot write user data while it is being copied. Retained
previous trees consume disk space until the owner deliberately removes them.

The same binary handles all three modes — it picks automatically based on
what's in the destination.

## Preflight

Before a new clone, fetch or build, the installer runs preflight checks and
reports failures with code **10**. Ordinary preflight refusals leave the
destination unchanged. When a previous tree awaits recovery, path/link and
writability checks run first; the retained working tree is restored before
checking CMake and the new-build disk budget. Recovery therefore still works
when build tools are missing or the volume has insufficient space for another
build. Those normal build gates remain mandatory after recovery.

| Check | Failure code |
|---|---|
| No existing component of the destination path is a symlink or NTFS junction (on Linux/macOS, root-owned system symlinks such as macOS `/var` are allowed). | `destination-link` |
| That directory exists as a directory and accepts a create + remove of a `.sparkinstaller-preflight-<pid>` probe file. | `destination-not-directory`, `destination-not-writable` |
| Free space on that volume meets the disk budget below. | `insufficient-free-space`, `free-space-unknown` |
| `cmake --version` runs (skipped with `--skip-build`). | `cmake-unavailable` |
| Update mode: an existing `.sparkengine-install.json` parses. | `corrupt-install-marker` |

### Disk budget

Measured on Windows on 2026-09-27: a fresh clone with submodules takes about
1.45 GiB (1.03 GiB git pack plus 0.42 GiB checkout), and a `windows-release`
tree built with the default options (tests and game modules on) takes about
40.5 GiB. The installer therefore requires:

| Run | Free space required |
|---|---|
| Install with a build | 40 GiB |
| Install with `--skip-build` | 2 GiB |
| Update with a build (new sibling build; previous tree retained) | 40 GiB |
| Update with `--skip-build` (staged source only) | 2 GiB |

### Exit codes

| Code | Meaning |
|---|---|
| 0 | Success. |
| 2 | Invalid arguments or unresolvable destination. |
| 3 | Git is unavailable and could not be bootstrapped. |
| 4 | Destination is unavailable, nonempty for a fresh install, locked by another installer, or a pending install was started for a different ref. |
| 5 | Clone, fetch, checkout or submodule update failed, the existing install has local changes, or a pending install is no longer at its cloned commit. |
| 6 | CMake configure failed, or an update build path is outside the install tree. |
| 7 | CMake build failed. |
| 8 | The installed commit, the install marker or the pending-install marker could not be recorded, read or removed. |
| 9 | Update activation or recovery failed or is ambiguous; the reported staged and previous trees are retained for recovery. |
| 10 | Preflight refused the run; nothing was changed. |

## Usage

```
sparkinstaller                         # interactive TUI
sparkinstaller --gui                   # interactive ImGui wizard (source builds only)
sparkinstaller --headless \
    --dest /opt/sparkengine --ref Working   # non-interactive
```

### Flags

| Flag | Meaning |
|---|---|
| `--dest <dir>` | Install destination (default: `./SparkEngine`). |
| `--ref <name>` | Git branch or tag to clone (default: `Working`). |
| `--repo <url>` | Override repo URL (defaults to `Krilliac/SparkEngine` on GitHub). |
| `--gui` | Launch the ImGui wizard instead of the terminal UI. Source-build only: compiled when `SPARKINSTALLER_ENABLE_GUI=ON`. The published installer is built with it OFF and answers `--gui` with exit 2. |
| `--headless` | Non-interactive; fails if required inputs are missing. |
| `--skip-build` | Clone only; do not configure or build. A fresh install stays pending; an update stays in a sibling without replacing the live tree. |
| `--skip-submodules` | Skip submodule update step in Update mode. |
| `--help`, `--version` | Help / version. |

## How it relates to SparkBuild

SparkInstaller **reuses** SparkBuild's machinery:

- `SparkBuildCore` static library (`Config`, `Downloader`, `ProcessRunner`,
  `Terminal`) — linked directly, no shelling out, no duplication.
- CMake configure and build commands come from the same `ConfigManager` that
  SparkBuild uses.

If you already have an engine checkout and just want to reconfigure the build,
run **SparkBuild** directly — it's the pure build-configuration TUI. Use
**SparkInstaller** for the first-time clone + build, or to update an existing
install to a new ref.

### Recovery and package-tree repeatability evidence

The installer transaction tests cover activation refusal when a destination
contains user data, pending-install resume after a failed build, byte-identical
live build outputs after a failed staged update, and a real installer process
killed after fake git checkout. Recovery fixtures cover the activation rename
gap and preservation of the previous tree and its user data. Install-state replacement uses a temporary file and
atomic rename on supported filesystems, and refuses non-regular marker targets.

`Tests/PackageSmoke/installer_repeatability.cmake` is the local package-tree
runner. It reuses the production manifest-driven CMake uninstall helper,
checks repeated installs are byte-identical, runs two uninstall cycles, and
verifies declared user data survives. This does not certify native Windows
CPack, NSIS/MSI repair or uninstall, signing, or clean-machine behavior; those
still require the Windows qualification job.

## Where it lives

Source lives in-tree at `SparkInstaller/`. A rolling Windows nightly installer
may be published for development evaluation; it is not a versioned stable release
and does not certify `stable-v1`. No versioned stable installer or release has
been published. Installer size and per-OS distribution remain future release
concerns outside the blocked `stable-v1` contract.

## Git bootstrap behaviour

| Platform | Behaviour when `git` is missing |
|---|---|
| Windows | Auto-downloads the SHA-256-pinned MinGit archive, extracts it into a unique staging directory under `%LOCALAPPDATA%/SparkInstaller/cache`, and only then renames it to `cache/mingit` with a hash activation marker. A cached `mingit` tree without a matching marker (for example after an interrupted extraction), or one that is a symlink/junction, is never used: it is renamed aside to `mingit.untrusted-*` (not deleted) and replaced by a fresh verified copy. The user's system PATH is never modified. |
| Linux | Prints a short message instructing `apt`/`dnf`/`pacman` install and exits. |
| macOS | Prints `xcode-select --install` instruction and exits. |

[mingit]: https://github.com/git-for-windows/git/releases

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
   recover ref + options.
2. Verify the existing checkout has no tracked or untracked changes; refuse the
   update when local changes could make rollback ambiguous.
3. Record the rollback target: the current `HEAD`, or the commit in
   `.sparkengine-install.json` when the two differ. A difference means an
   earlier update was interrupted after its checkout and before its build was
   recorded, so only the recorded commit is a verified build.
4. `git fetch` + `git checkout <ref>` + `git submodule update --init --recursive`.
5. Re-run configure + build with the stored options.
6. Update `.sparkengine-install.json` with the new commit + timestamp, and
   remove any `.sparkengine-install.repair-required` marker.

If any step after the fetch fails, the installer restores the rollback target
(`git checkout --detach` + submodule update), rebuilds it, and verifies that
`HEAD` is that commit again. The run then exits with the original failure code
and the install is the verified build it was before. If the restore, the
rebuild or the verification fails, the installer writes
`.sparkengine-install.repair-required` (with the reason) and exits **9**; a
later successful update clears it.

The same binary handles all three modes — it picks automatically based on
what's in the destination.

## Preflight

Before it bootstraps Git, creates a directory, clones or fetches, the installer
runs read-only checks and reports every failure at once. Any failure exits with
code **10** and leaves the destination exactly as it was.

| Check | Failure code |
|---|---|
| The destination, or its nearest existing ancestor when it does not exist yet, is not a symlink or NTFS junction. | `destination-link` |
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
| Update (the existing build tree is rebuilt in place) | 2 GiB |

### Exit codes

| Code | Meaning |
|---|---|
| 0 | Success. |
| 2 | Invalid arguments or unresolvable destination. |
| 3 | Git is unavailable and could not be bootstrapped. |
| 4 | Install destination exists and is not empty (the staged clone is kept and reported when it appeared during the clone), or a pending install was started for a different ref. |
| 5 | Clone, fetch, checkout or submodule update failed, the existing install has local changes, or a pending install is no longer at its cloned commit. |
| 6 | CMake configure failed. |
| 7 | CMake build failed. |
| 8 | The installed commit, the install marker or the pending-install marker could not be recorded, read or removed. |
| 9 | An update failed and its rollback could not restore and rebuild the previous commit; the install requires repair (see `.sparkengine-install.repair-required`). |
| 10 | Preflight refused the run; nothing was changed. |

## Usage

```
sparkinstaller                         # interactive TUI
sparkinstaller --gui                   # interactive ImGui wizard
sparkinstaller --headless \
    --dest /opt/sparkengine --ref Working   # non-interactive
```

### Flags

| Flag | Meaning |
|---|---|
| `--dest <dir>` | Install destination (default: `./SparkEngine`). |
| `--ref <name>` | Git branch or tag to clone (default: `Working`). |
| `--repo <url>` | Override repo URL (defaults to `Krilliac/SparkEngine` on GitHub). |
| `--gui` | Launch the ImGui wizard instead of the terminal UI. |
| `--headless` | Non-interactive; fails if required inputs are missing. |
| `--skip-build` | Clone only; do not configure or build. A fresh install stays pending and the next run without it resumes the build. |
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

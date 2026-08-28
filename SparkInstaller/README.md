# SparkInstaller

Standalone bootstrap installer and updater for SparkEngine.

Download it from the project's Releases page, run it, pick your options — it
clones the engine repository with all submodules, configures CMake to your
taste, and builds the engine on your machine.

## What it does

**Install mode** (empty destination):

1. Detect Git — on Windows, auto-download [MinGit][mingit] if absent; on
   Linux/macOS, print install instructions and exit cleanly.
2. Detect CMake — delegate to `SparkBuildCore` which can download a pinned
   CMake release if needed.
3. Prompt for destination directory and ref (branch / tag).
4. Let you pick a build preset: **Defaults**, **All on**, **Minimal**,
   **Linux-friendly**, **Shipping**, or **Development**.
5. `git clone --recurse-submodules --branch <ref>` the repository.
6. Run `cmake` configure + build for the chosen options.
7. Write `.sparkengine-install.json` into the install root to mark it.

**Update mode** (destination contains an existing install):

1. Read the prior `.sparkengine-install.json` to recover ref + options.
2. `git fetch` + `git checkout <ref>` + `git submodule update --init --recursive`.
3. Re-run configure + build with the stored options.
4. Update `.sparkengine-install.json` with the new commit + timestamp.

The same binary handles both modes — it picks automatically based on what's
in the destination.

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
| `--skip-build` | Clone only; do not configure or build. |
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
| Windows | Auto-downloads MinGit into `%LOCALAPPDATA%/SparkInstaller/cache/mingit` and uses it. The user's system PATH is never modified. |
| Linux | Prints a short message instructing `apt`/`dnf`/`pacman` install and exits. |
| macOS | Prints `xcode-select --install` instruction and exits. |

[mingit]: https://github.com/git-for-windows/git/releases

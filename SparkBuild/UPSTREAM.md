# SparkBuild Upstream Provenance

The source in this directory was vendored from the standalone
`Krilliac/SparkBuild` repository. This `SparkEngine/SparkBuild/` tree is now
the **authoritative source**: future changes are made here. The upstream
repo is kept only for historical reference.

## Pinned import

| Field | Value |
|-------|-------|
| Upstream | <https://github.com/Krilliac/SparkBuild> |
| Branch | `main` |
| Commit | `83060506c4041f9f30aff5abe221f87f5d22fa24` |
| License | MIT (per upstream README) |

## Layout

```
SparkBuild/
  CMakeLists.txt   # C++17 standalone project, builds bin/SparkBuild
  README.md        # Upstream README (feature list, usage)
  src/             # 12 translation units (main, Config, ProcessRunner, Downloader,
                   # Terminal, SparkBuild, Platform.h)
  resources/       # Windows VERSIONINFO resource (SparkBuild.rc, resource.h)
```

## Why in-tree

SparkBuild shells out to `cmake` and has zero dependency on any SparkEngine
header or library — so it can live here with no circular build dependency.
Keeping the source in this repo:

- Eliminates binary commits (`tools/SparkBuild*` were removed)
- Lets patches ship in a single PR instead of across two repos
- Keeps SparkBuild in lockstep with engine CMake option changes
- Is reviewable: the full source is visible alongside the engine it configures

## If you need to sync upstream

If an external change lands in `Krilliac/SparkBuild` that you want to pull
into this repo, do a diff-and-port rather than a blind overwrite:

```bash
git clone --depth 50 https://github.com/Krilliac/SparkBuild.git /tmp/sb
diff -ruN SparkBuild/ /tmp/sb/ | less   # review before applying
```

Cherry-pick the changes as a normal PR against this repository and bump the
pinned commit SHA in this file.

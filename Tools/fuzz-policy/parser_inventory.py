#!/usr/bin/env python3
"""SEC-120 — Authoritative parser inventory for SparkEngine.

Scans the source tree to discover file/packet/data parsers, classifies them
by trust boundary (untrusted-file, untrusted-network, trusted-internal), and
emits a machine-readable manifest consumed by check_fuzz_policy.py.

Every stable-v1 parser in the KNOWN_PARSERS registry must either:
  1. Map to a fuzz target (harness path), OR
  2. Carry an explicit human-authored blocker reason and ticket.

Parsers discovered in the source tree but absent from KNOWN_PARSERS are
flagged as UNCLASSIFIED — a fail-closed CI gate rejects that state.

Usage:
    python tools/fuzz-policy/parser_inventory.py [--emit-json] [--source-root .]
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from dataclasses import asdict, dataclass, field
from enum import Enum
from pathlib import Path
from typing import Optional


class TrustBoundary(str, Enum):
    UNTRUSTED_FILE = "untrusted-file"
    UNTRUSTED_NETWORK = "untrusted-network"
    TRUSTED_INTERNAL = "trusted-internal"


class FuzzStatus(str, Enum):
    FUZZED = "fuzzed"
    BLOCKED = "blocked"
    UNCLASSIFIED = "unclassified"


@dataclass(frozen=True)
class ParserEntry:
    parser_id: str
    description: str
    trust_boundary: TrustBoundary
    source_files: list[str]
    formats_handled: list[str]
    fuzz_status: FuzzStatus
    fuzz_target: Optional[str] = None
    blocker_reason: Optional[str] = None
    blocker_ticket: Optional[str] = None
    stability: str = "stable-v1"
    max_input_bytes: int = 0
    max_parse_time_ms: int = 0

    def validate(self) -> list[str]:
        errors: list[str] = []
        if self.fuzz_status == FuzzStatus.FUZZED and not self.fuzz_target:
            errors.append(f"{self.parser_id}: status=fuzzed but no fuzz_target")
        if self.fuzz_status == FuzzStatus.BLOCKED:
            if not self.blocker_reason:
                errors.append(f"{self.parser_id}: status=blocked but no blocker_reason")
            if not self.blocker_ticket:
                errors.append(f"{self.parser_id}: status=blocked but no blocker_ticket")
        if self.max_input_bytes <= 0 and self.fuzz_status == FuzzStatus.FUZZED:
            errors.append(f"{self.parser_id}: fuzzed parser must declare max_input_bytes > 0")
        if self.max_parse_time_ms <= 0 and self.fuzz_status == FuzzStatus.FUZZED:
            errors.append(f"{self.parser_id}: fuzzed parser must declare max_parse_time_ms > 0")
        return errors


# ---------------------------------------------------------------------------
# Authoritative parser registry — the single source of truth.
# Every parser surface that processes external data MUST appear here.
# ---------------------------------------------------------------------------

KNOWN_PARSERS: list[ParserEntry] = [
    # ── Mesh / Model parsers ──────────────────────────────────────────────
    ParserEntry(
        parser_id="mesh-obj-loader",
        description="Wavefront OBJ mesh importer via tinyobj",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Graphics/ModelLoadingWindows.cpp",
            "SparkEngine/Source/Graphics/ModelLoadingLinux.cpp",
        ],
        formats_handled=[".obj"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="No libFuzzer harness yet; tinyobj upstream has its own fuzz targets — need to wire engine-specific vertex pipeline",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),
    ParserEntry(
        parser_id="mesh-fbx-importer",
        description="FBX SDK mesh + animation importer",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Graphics/FBXImporter.cpp",
            "SparkEngine/Source/Graphics/FBXImporter.h",
        ],
        formats_handled=[".fbx"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="FBX SDK is proprietary — cannot instrument; blocked on Autodesk upstream fuzz coverage or GLTF migration",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),
    ParserEntry(
        parser_id="mesh-gltf-loader",
        description="GLTF/GLB static mesh loader",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Graphics/GLTFStaticMeshLoader.cpp",
        ],
        formats_handled=[".gltf", ".glb"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Parser exists but harness not yet written; needs structured seed corpus from GLTF sample models",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),

    # ── Texture parsers ───────────────────────────────────────────────────
    ParserEntry(
        parser_id="texture-loader-windows",
        description="Windows texture loading (WIC/DDS/stb_image)",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Graphics/TextureSystemWindows.cpp",
            "SparkEngine/Source/Graphics/TextureSystem.h",
        ],
        formats_handled=[".png", ".jpg", ".bmp", ".dds", ".tga", ".hdr"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="stb_image upstream has fuzzing but engine wrapping adds validation surfaces; WIC is OS-provided",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),
    ParserEntry(
        parser_id="texture-loader-linux",
        description="Linux texture loading (stb_image)",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Graphics/TextureSystemLinux.cpp",
        ],
        formats_handled=[".png", ".jpg", ".bmp", ".tga", ".hdr"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="stb_image upstream has fuzzing but engine wrapping adds validation surfaces",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),
    ParserEntry(
        parser_id="texture-exr-loader",
        description="EXR HDR image loader",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Graphics/EXRLoader.h",
        ],
        formats_handled=[".exr"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Header-only tinyexr; needs harness wrapping engine's LoadEXR path",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),
    ParserEntry(
        parser_id="texture-basis-transcoder",
        description="Basis Universal GPU texture transcoder",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Graphics/BasisTranscoder.h",
        ],
        formats_handled=[".basis", ".ktx2"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Upstream Basis has fuzz targets; engine wrapper needs its own harness",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),
    ParserEntry(
        parser_id="texture-streaming",
        description="Streaming texture tile loader",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Graphics/TextureStreaming.cpp",
        ],
        formats_handled=[".stex"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Custom streaming format — needs corpus generation and harness",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),

    # ── Material / Shader parsers ─────────────────────────────────────────
    ParserEntry(
        parser_id="material-loader",
        description="Material definition file loader (.mat/.json)",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Graphics/MaterialLoader.cpp",
            "SparkEngine/Source/Graphics/MaterialLoader.h",
            "SparkEngine/Source/Graphics/MaterialSystem.cpp",
        ],
        formats_handled=[".mat", ".json"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="JSON-based format — needs structured corpus from existing materials",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),
    ParserEntry(
        parser_id="shader-source-loader",
        description="HLSL/GLSL shader source compilation pipeline",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Graphics/ShaderWindows.cpp",
            "SparkEngine/Source/Graphics/ShaderCompilationWindows.cpp",
            "SparkEngine/Source/Graphics/ShaderCompilationLinux.cpp",
            "SparkEngine/Source/Graphics/ShaderHotReload.cpp",
        ],
        formats_handled=[".hlsl", ".glsl", ".vert", ".frag", ".comp"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Shader compilation delegates to platform compiler (fxc/dxc/glslang) — engine wrapper parses #include and preprocessor directives",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),
    ParserEntry(
        parser_id="svg-renderer",
        description="SVG vector graphics parser/renderer",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Graphics/SVGRenderer.h",
        ],
        formats_handled=[".svg"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="XML-based format with complex path data — high-value fuzz target, needs harness",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),

    # ── Audio parsers ─────────────────────────────────────────────────────
    ParserEntry(
        parser_id="audio-sound-effect",
        description="WAV/audio file loader for sound effects",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Audio/SoundEffect.cpp",
            "SparkEngine/Source/Audio/SoundEffect.h",
            "SparkEngine/Source/Audio/AudioEngine.cpp",
        ],
        formats_handled=[".wav", ".ogg"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="WAV header parsing is a classic fuzz target; needs corpus of malformed audio headers",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),

    # ── Scene / Serialization parsers ─────────────────────────────────────
    ParserEntry(
        parser_id="scene-serializer",
        description="Scene file load/save (JSON-based scene graph)",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/SceneManager/SceneManager.h",
            "SparkEngine/Source/SceneManager/ReflectedSceneSerializer.h",
        ],
        formats_handled=[".scene", ".json"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Scene files can reference arbitrary asset paths — path traversal risk; needs structured corpus",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),
    ParserEntry(
        parser_id="scene-snapshot-serializer",
        description="Editor scene snapshot save/restore",
        trust_boundary=TrustBoundary.TRUSTED_INTERNAL,
        source_files=[
            "SparkEngine/Source/Engine/Editor/SceneSnapshotSerializer.h",
        ],
        formats_handled=[".snapshot"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Trusted-internal but shares serialization paths with scene-serializer",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),
    ParserEntry(
        parser_id="reflection-serializer",
        description="Reflection-based generic object serializer",
        trust_boundary=TrustBoundary.TRUSTED_INTERNAL,
        source_files=[
            "SparkEngine/Source/Core/ReflectionSerializer.h",
        ],
        formats_handled=[".json"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Used by scene/prefab/save pipelines — fuzz via those entry points",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),

    # ── Save / Persistence parsers ────────────────────────────────────────
    ParserEntry(
        parser_id="save-system",
        description="Game save file serializer/deserializer",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Engine/SaveSystem/SaveSystem.h",
            "SparkEngine/Source/Engine/SaveSystem/SaveSystemTypes.h",
        ],
        formats_handled=[".sav", ".save"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Save files are user-modifiable — classic deserialization attack surface",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),
    ParserEntry(
        parser_id="replay-system",
        description="Replay file recorder/player",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Engine/Replay/ReplaySystem.cpp",
            "SparkEngine/Source/Engine/Replay/ReplaySystem.h",
        ],
        formats_handled=[".replay"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Replay files contain serialized input sequences — needs fuzz harness",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),

    # ── Config / Data parsers ─────────────────────────────────────────────
    ParserEntry(
        parser_id="config-parser",
        description="Engine configuration file parser (INI-style)",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Utils/ConfigParser.cpp",
            "SparkEngine/Source/Utils/ConfigParser.h",
        ],
        formats_handled=[".ini", ".cfg", ".config"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Config files are user-editable — parsing bugs could corrupt engine state",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),
    ParserEntry(
        parser_id="engine-settings",
        description="Engine settings loader (JSON)",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Core/EngineSettings.h",
        ],
        formats_handled=[".json"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Settings JSON is user-editable; shares JSON parsing path with other loaders",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),
    ParserEntry(
        parser_id="datatable-system",
        description="DataTable CSV/JSON loader for game data",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Engine/DataTable/DataTableSystem.h",
        ],
        formats_handled=[".csv", ".json"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="DataTables can be modded — CSV and JSON parsing needs fuzz coverage",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),
    ParserEntry(
        parser_id="dialogue-system",
        description="Dialogue tree file loader (JSON/custom)",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Engine/Dialogue/DialogueSystem.cpp",
            "SparkEngine/Source/Engine/Dialogue/DialogueSystem.h",
        ],
        formats_handled=[".dialogue", ".json"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Dialogue files are moddable content — needs structured corpus",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),
    ParserEntry(
        parser_id="localization-system",
        description="Localization string table loader",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Engine/Localization/LocalizationSystem.cpp",
            "SparkEngine/Source/Engine/Localization/LocalizationSystem.h",
        ],
        formats_handled=[".loc", ".csv", ".json"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Localization files are user/modder-supplied; format string injection risk",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),
    ParserEntry(
        parser_id="achievement-system",
        description="Achievement definition loader",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Engine/Gameplay/AchievementSystem.h",
        ],
        formats_handled=[".json"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Achievement data is moddable content",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),
    ParserEntry(
        parser_id="event-response-system",
        description="Event response rule file loader",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Engine/Gameplay/EventResponseSystem.h",
            "SparkEngine/Source/Engine/Gameplay/EventResponseSystem.cpp",
        ],
        formats_handled=[".json"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Event rules can be modded — arbitrary callback references",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),
    ParserEntry(
        parser_id="input-bindings",
        description="Input binding configuration loader",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Input/InputBindings.cpp",
            "SparkEngine/Source/Input/InputBindings.h",
        ],
        formats_handled=[".json", ".ini"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Input binding files are user-editable",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),

    # ── Modding / VFS parsers ─────────────────────────────────────────────
    ParserEntry(
        parser_id="mod-system",
        description="Mod manifest and package loader",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Engine/Modding/ModSystem.h",
            "SparkEngine/Source/Engine/Modding/ModSystemIO.cpp",
        ],
        formats_handled=[".json", ".mod"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Mod packages are fully untrusted — path traversal and zip-slip risk",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),
    ParserEntry(
        parser_id="archive-resource-provider",
        description="Archive (PAK/ZIP) resource provider for VFS",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Engine/Modding/ArchiveResourceProvider.h",
            "SparkEngine/Source/Engine/Modding/ArchiveResourceProvider.cpp",
        ],
        formats_handled=[".pak", ".zip"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Archive extraction is a zip-slip / path-traversal attack surface",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),
    ParserEntry(
        parser_id="sparkpak",
        description="SparkPak custom archive format",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Core/SparkPak.cpp",
            "SparkEngine/Source/Core/SparkPak.h",
        ],
        formats_handled=[".spak"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Custom binary archive — high-value target for structured fuzzing",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),
    ParserEntry(
        parser_id="virtual-filesystem",
        description="Virtual filesystem mount/overlay parser",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Engine/Modding/VirtualFileSystem.h",
            "SparkEngine/Source/Engine/Modding/VirtualFileSystem.cpp",
        ],
        formats_handled=[".vfs"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="VFS path resolution — path traversal and mount-escape risk",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),

    # ── Scripting parsers ─────────────────────────────────────────────────
    ParserEntry(
        parser_id="angelscript-engine",
        description="AngelScript source file compiler/loader",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Engine/Scripting/AngelScriptEngine.h",
        ],
        formats_handled=[".as", ".angel"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Script files are user/modder-supplied — sandbox escape risk; upstream AngelScript has limited fuzz coverage",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),

    # ── Network parsers ──────────────────────────────────────────────────
    ParserEntry(
        parser_id="network-packet-parser",
        description="UDP network packet deserializer",
        trust_boundary=TrustBoundary.UNTRUSTED_NETWORK,
        source_files=[
            "SparkEngine/Source/Engine/Networking/NetworkManager.h",
            "SparkEngine/Source/Engine/Networking/NetworkIntegration.h",
        ],
        formats_handled=["udp-wire-format"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Network packets are fully untrusted; wire format is unauthenticated per SECURITY.md/NET-100",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),
    ParserEntry(
        parser_id="entity-replicator",
        description="Entity replication packet parser",
        trust_boundary=TrustBoundary.UNTRUSTED_NETWORK,
        source_files=[
            "SparkEngine/Source/Engine/Networking/EntityReplicator.h",
            "SparkEngine/Source/Engine/Networking/ReplicationFields.h",
        ],
        formats_handled=["replication-wire-format"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Replication data is deserialized from untrusted network peers",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),
    ParserEntry(
        parser_id="datablock-registry",
        description="Network datablock definition deserializer",
        trust_boundary=TrustBoundary.UNTRUSTED_NETWORK,
        source_files=[
            "SparkEngine/Source/Engine/Networking/DatablockRegistry.h",
        ],
        formats_handled=["datablock-wire-format"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Datablock definitions arrive over untrusted network",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),
    ParserEntry(
        parser_id="daemon-framing",
        description="SparkDaemon IPC framing protocol parser",
        trust_boundary=TrustBoundary.TRUSTED_INTERNAL,
        source_files=[
            "SparkEngine/Source/Utils/DaemonFraming.h",
        ],
        formats_handled=["daemon-ipc-frames"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="IPC framing is localhost-only but length-prefix parsing is classic fuzz target",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),

    # ── Prefab / ECS parsers ──────────────────────────────────────────────
    ParserEntry(
        parser_id="runtime-prefab",
        description="Runtime prefab instantiation loader",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Engine/ECS/RuntimePrefab.h",
        ],
        formats_handled=[".prefab", ".json"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Prefab files can be modded; reference arbitrary components",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),
    ParserEntry(
        parser_id="entity-archetype-loader",
        description="Entity archetype definition loader",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Engine/ECS/EntityArchetypeLoader.h",
        ],
        formats_handled=[".archetype", ".json"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Archetype files define component layouts from untrusted data",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),

    # ── Streaming / Manifest parsers ──────────────────────────────────────
    ParserEntry(
        parser_id="scene-manifest",
        description="Area streaming scene manifest parser",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Engine/Streaming/SceneManifest.h",
        ],
        formats_handled=[".manifest", ".json"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Manifests reference asset paths — path traversal risk",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),

    # ── Asset pipeline parsers ────────────────────────────────────────────
    ParserEntry(
        parser_id="asset-pipeline",
        description="Asset pipeline import/process orchestrator",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Graphics/AssetPipeline.h",
            "SparkEngine/Source/Graphics/AssetPipelineWindows.cpp",
            "SparkEngine/Source/Graphics/AssetPipelineLinux.cpp",
        ],
        formats_handled=["*"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Orchestrator delegates to per-format parsers; fuzz those directly",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),
    ParserEntry(
        parser_id="asset-migration",
        description="Asset version migration transformer",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Core/AssetMigration.h",
        ],
        formats_handled=[".json"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Migration transforms old-format data — needs corpus of version-spanning assets",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),

    # ── Persistence parsers ───────────────────────────────────────────────
    ParserEntry(
        parser_id="async-database",
        description="Async database query result parser",
        trust_boundary=TrustBoundary.TRUSTED_INTERNAL,
        source_files=[
            "SparkEngine/Source/Engine/Persistence/AsyncDatabase.cpp",
            "SparkEngine/Source/Engine/Persistence/AsyncDatabase.h",
        ],
        formats_handled=["sqlite-rows"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Trusted-internal but database files could be tampered; SQL injection surface",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),

    # ── NavMesh parser ────────────────────────────────────────────────────
    ParserEntry(
        parser_id="navmesh-loader",
        description="Navigation mesh file loader (Recast/Detour)",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Engine/AI/NavMesh.h",
        ],
        formats_handled=[".navmesh", ".bin"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="NavMesh binary format from Recast — needs structured fuzzing of tile headers",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),

    # ── UI parsers ────────────────────────────────────────────────────────
    ParserEntry(
        parser_id="ui-layout-extensions",
        description="UI layout definition loader (JSON/XML)",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Engine/UI/UILayoutExtensions.h",
            "SparkEngine/Source/Engine/UI/UILayoutExtensions.cpp",
        ],
        formats_handled=[".uilayout", ".json"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="UI layouts can be modded; recursive widget trees could stack-overflow",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),
    ParserEntry(
        parser_id="ui-factory",
        description="UI widget factory/template instantiator",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Engine/UI/UIFactory.cpp",
            "SparkEngine/Source/Engine/UI/UIFactory.h",
        ],
        formats_handled=[".json"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Widget templates from modded content",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),

    # ── JSON utilities (shared infrastructure) ────────────────────────────
    ParserEntry(
        parser_id="json-utils",
        description="Central JSON parsing utilities (nlohmann/json wrapper)",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Utils/JsonUtils.h",
        ],
        formats_handled=[".json"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Shared JSON parsing layer — upstream nlohmann/json has fuzz targets; engine wrapper adds type coercion",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),

    # ── Core serialization primitives ─────────────────────────────────────
    ParserEntry(
        parser_id="binary-reader-serializer",
        description="BinaryReader/BinaryWriter generic serialization primitives",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Utils/Serializer.h",
        ],
        formats_handled=["binary-blob"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Core serialization layer used by save/scene/prefab/pak — fuzz via those entry points but also needs direct harness for length/bounds checking",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),

    # ── Crash reporter ────────────────────────────────────────────────────
    ParserEntry(
        parser_id="crash-manifest-parser",
        description="Crash reporter manifest JSON parser",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkCrashReporter/src/CrashReporterApp.h",
        ],
        formats_handled=[".json"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Crash manifests are written by the crashing process and read by the reporter — could be corrupted; SEC-120 FuzzCrashManifest selector",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),

    # ── Shader compiler (standalone tool) ─────────────────────────────────
    ParserEntry(
        parser_id="shader-compiler-tool",
        description="SparkShaderCompiler standalone shader compilation tool",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkShaderCompiler/src/main.cpp",
        ],
        formats_handled=[".hlsl", ".glsl"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Standalone tool that processes user-supplied shader files — separate entry point from engine shader loading",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),

    # ── SparkPak writer (archive creation) ────────────────────────────────
    ParserEntry(
        parser_id="sparkpak-writer",
        description="SparkPak archive writer/packer",
        trust_boundary=TrustBoundary.UNTRUSTED_FILE,
        source_files=[
            "SparkEngine/Source/Core/SparkPakWriter.cpp",
        ],
        formats_handled=[".spak"],
        fuzz_status=FuzzStatus.BLOCKED,
        blocker_reason="Writer processes file lists that could contain path traversal entries",
        blocker_ticket="SEC-120",
        max_input_bytes=0,
        max_parse_time_ms=0,
    ),

]


# ---------------------------------------------------------------------------
# Source-tree scanner — discovers parser-like functions to cross-check
# ---------------------------------------------------------------------------

_PARSER_PATTERNS = [
    re.compile(r"\b(?:Load|Parse|Import|Deserialize|Read)(?:File|From|Config|Scene|Mesh|Texture|Shader|Audio|Data|Asset)\b"),
    re.compile(r"\bifstream\b"),
    re.compile(r"\bfopen\b"),
    re.compile(r"\bLoadFrom(?:File|Buffer|Stream)\b"),
]


def scan_source_tree(source_root: Path) -> list[dict]:
    """Walk source dirs and find files containing parser-like patterns."""
    hits: list[dict] = []
    search_dirs = [
        source_root / "SparkEngine" / "Source",
        source_root / "SparkEditor" / "Source",
    ]
    for search_dir in search_dirs:
        if not search_dir.exists():
            continue
        for root, _dirs, files in os.walk(search_dir):
            for fname in files:
                if not fname.endswith((".h", ".cpp")):
                    continue
                fpath = Path(root) / fname
                rel = fpath.relative_to(source_root).as_posix()
                try:
                    text = fpath.read_text(encoding="utf-8", errors="replace")
                except OSError:
                    continue
                for pat in _PARSER_PATTERNS:
                    if pat.search(text):
                        hits.append({"file": rel, "pattern": pat.pattern})
                        break
    return hits


def get_known_source_files() -> set[str]:
    """All source files referenced by the known parser registry."""
    files: set[str] = set()
    for p in KNOWN_PARSERS:
        files.update(p.source_files)
    return files


def find_unclassified_parsers(source_root: Path) -> list[dict]:
    """Find source files with parser patterns not in the known registry."""
    known = get_known_source_files()
    scanned = scan_source_tree(source_root)
    unclassified = []
    for hit in scanned:
        if hit["file"] not in known:
            unclassified.append(hit)
    return unclassified


def get_parser_ids() -> set[str]:
    ids = set()
    for p in KNOWN_PARSERS:
        ids.add(p.parser_id)
    return ids


def check_duplicate_ids() -> list[str]:
    seen: dict[str, int] = {}
    for p in KNOWN_PARSERS:
        seen[p.parser_id] = seen.get(p.parser_id, 0) + 1
    return [pid for pid, count in seen.items() if count > 1]


def emit_manifest(source_root: Path) -> dict:
    """Build the full manifest for CI consumption."""
    errors: list[str] = []

    dupes = check_duplicate_ids()
    if dupes:
        errors.append(f"Duplicate parser_ids: {dupes}")

    for p in KNOWN_PARSERS:
        errors.extend(p.validate())

    unclassified = find_unclassified_parsers(source_root)

    by_boundary: dict[str, int] = {}
    by_status: dict[str, int] = {}
    for p in KNOWN_PARSERS:
        by_boundary[p.trust_boundary.value] = by_boundary.get(p.trust_boundary.value, 0) + 1
        by_status[p.fuzz_status.value] = by_status.get(p.fuzz_status.value, 0) + 1

    return {
        "schema_version": "1.0.0",
        "total_parsers": len(KNOWN_PARSERS),
        "by_trust_boundary": by_boundary,
        "by_fuzz_status": by_status,
        "parsers": [asdict(p) for p in KNOWN_PARSERS],
        "unclassified_sources": unclassified,
        "validation_errors": errors,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emit-json", action="store_true", help="Print JSON manifest to stdout")
    parser.add_argument("--source-root", default=".", help="Repository root")
    args = parser.parse_args()

    source_root = Path(args.source_root).resolve()
    manifest = emit_manifest(source_root)

    if args.emit_json:
        print(json.dumps(manifest, indent=2))
        return 0

    print(f"Parser inventory: {manifest['total_parsers']} parsers")
    print(f"  By trust boundary: {manifest['by_trust_boundary']}")
    print(f"  By fuzz status:    {manifest['by_fuzz_status']}")

    if manifest["unclassified_sources"]:
        print(f"\n  UNCLASSIFIED source files ({len(manifest['unclassified_sources'])}):")
        for u in manifest["unclassified_sources"][:20]:
            print(f"    {u['file']}")
        if len(manifest["unclassified_sources"]) > 20:
            print(f"    ... and {len(manifest['unclassified_sources']) - 20} more")

    if manifest["validation_errors"]:
        print(f"\n  VALIDATION ERRORS ({len(manifest['validation_errors'])}):")
        for e in manifest["validation_errors"]:
            print(f"    {e}")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())

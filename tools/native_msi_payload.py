"""Inventory all File-table payloads of a self-contained CPack/WiX MSI.

The caller supplies final MSI bytes, never an installation/staging directory.
The read-only database and cabinet APIs run against a verified private copy.
Feature selection/conditions are deliberately not evaluated: every File row and
every embedded Media cabinet is required, including optional components.
External, uncompressed, spanned, patched and transformed layouts fail closed.
This is static payload evidence, not proof of installation/custom-action behavior.
"""
from __future__ import annotations

from contextlib import contextmanager
from bisect import bisect_left
import hashlib
import importlib.util
import json
from pathlib import Path, PurePosixPath
import re
import stat
import tempfile

_spec = importlib.util.spec_from_file_location("spark_native_msi_api", Path(__file__).with_name("native_msi_api.py"))
api = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(api)
NativeMsiError, require = api.NativeMsiError, api.require

METHOD = "windows-msi-readonly-embedded-cabinets-v2"
MAX_BYTES = 8 << 30
MAX_FILES = 32767
PROFILE_PATH = "cmake/PackageProfiles.json"
RULES_PATH = "cmake/PackageNoticeCoverageRules.json"
IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_.]*")
DIGEST = re.compile(r"[0-9a-f]{64}")
RESERVED = {"con", "prn", "aux", "nul", *(f"com{i}" for i in range(1, 10)), *(f"lpt{i}" for i in range(1, 10))}


def digest(path):
    value = hashlib.sha256()
    remaining = regular(path).st_size
    require(remaining <= MAX_BYTES, "native payload exceeds byte limit")
    with path.open("rb") as stream:
        while remaining:
            chunk = stream.read(min(1024 * 1024, remaining))
            require(chunk, "native payload shrank while hashing")
            value.update(chunk)
            remaining -= len(chunk)
        require(not stream.read(1), "native payload grew while hashing")
    return value.hexdigest()


def regular(path):
    info = path.lstat()
    require(stat.S_ISREG(info.st_mode) and not getattr(info, "st_file_attributes", 0) & 0x400,
            f"not a regular non-reparse file: {path}")
    require(info.st_nlink == 1, f"hard-linked native payload is unsupported: {path}")
    return info


def safe_path(value):
    require(isinstance(value, str) and value and len(value) <= 2048, "invalid MSI payload path")
    parts = value.split("/")
    for part in parts:
        require(part not in ("", ".", "..") and not part.endswith((".", " "))
                and not any(ord(char) < 32 or char in '\\:*?"<>|' for char in part)
                and part.split(".", 1)[0].casefold() not in RESERVED, f"unsafe MSI payload path: {value!r}")
    return value.casefold()


def long_name(value, directory=False):
    # DefaultDir's target:source grammar affects only the source side, which is
    # never used for embedded cabinet IDs. Validate both sides nevertheless.
    names = value.split(":") if directory else [value]
    require(len(names) <= 2, f"unsupported MSI name: {value!r}")
    for name in names:
        forms = name.split("|")
        require(len(forms) <= 2, f"unsupported MSI name: {value!r}")
        for form in forms:
            if not (directory and form == "."):
                safe_path(form)
                require("/" not in form, f"MSI name must be one component: {value!r}")
    return names[0].split("|")[-1]


def number(value, label, minimum=0, maximum=MAX_BYTES):
    require(isinstance(value, str) and len(value) <= 10 and value.isascii() and value.isdigit(), f"invalid MSI {label}")
    result = int(value)
    require(minimum <= result <= maximum, f"MSI {label} out of range")
    return result


def keyed(rows, label):
    result, folded = {}, set()
    for row in rows:
        require(row and IDENTIFIER.fullmatch(row[0]), f"invalid MSI {label} identifier")
        require(row[0].casefold() not in folded, f"duplicate MSI {label} identifier")
        result[row[0]] = row[1:]
        folded.add(row[0].casefold())
    return result


def payload_plan(database):
    tables = {row[0] for row in database.rows("_Tables", ["Name"])}
    require({"File", "Component", "Directory", "Media"} <= tables, "missing MSI payload tables")
    for table in ("DuplicateFile", "MoveFile", "Patch", "PatchPackage", "MsiPatchMetadata"):
        require(table not in tables, f"unsupported MSI payload table: {table}")
    require(not database.rows("_Storages", ["Name"]), "embedded MSI transforms/storages are unsupported")
    word_count = database.word_count()
    require(word_count & 2 and not word_count & ~11, "MSI must contain original compressed media")
    directories = keyed(database.rows("Directory", ["Directory", "Directory_Parent", "DefaultDir"]), "Directory")
    components = keyed(database.rows("Component", ["Component", "Directory_"]), "Component")
    files = keyed(database.rows("File", ["File", "Component_", "FileName", "FileSize", "Attributes", "Sequence"]), "File")
    require(files and "INSTALL_ROOT" in directories, "MSI has no File payload or INSTALL_ROOT")
    for _parent, name in directories.values():
        long_name(name, directory=True)

    def directory_path(key):
        segments, seen = [], set()
        while key != "INSTALL_ROOT":
            require(key in directories and key not in seen, "MSI directory is cyclic or outside INSTALL_ROOT")
            require(len(seen) < 128, "MSI directory nesting exceeds 128")
            seen.add(key)
            key, name = directories[key]
            target = long_name(name, directory=True)
            if target != ".":
                segments.append(target)
        return "/".join(reversed(segments))

    media, previous, disks, streams = [], 0, set(), set()
    rows = database.rows("Media", ["DiskId", "LastSequence", "Cabinet", "Source"])
    for disk, last, cabinet, source in sorted(rows, key=lambda row: number(row[0], "DiskId", 1, 32767)):
        disk, last = number(disk, "DiskId", 1, 32767), number(last, "LastSequence", 1, 32767)
        require(disk not in disks and last > previous, "ambiguous MSI Media sequence range")
        require(not source and cabinet.startswith("#") and len(cabinet) > 1,
                "MSI external/uncompressed cabinets require verified sources and are unsupported")
        stream = cabinet[1:]
        safe_path(stream)
        require("/" not in stream and stream.casefold() not in streams, "duplicate/unsafe MSI cabinet")
        media.append({"stream": stream, "first": previous + 1, "last": last})
        previous = last
        disks.add(disk)
        streams.add(stream.casefold())
    require(media, "MSI has no embedded Media cabinets")
    boundaries = [item["last"] for item in media]
    payload, paths, sequences, total = [], set(), set(), 0
    for file_id, (component, name, size, attributes, sequence) in files.items():
        require(component in components, f"missing MSI component: {component}")
        folder = directory_path(components[component][0])
        path = "/".join(part for part in (folder, long_name(name)) if part)
        key = safe_path(path)
        require(key not in paths, f"colliding MSI payload path: {path}")
        paths.add(key)
        size, sequence = number(size, "FileSize"), number(sequence, "Sequence", 1, 32767)
        attributes = number(attributes or "0", "Attributes", maximum=32767)
        require(not attributes & (8192 | 4096), "uncompressed/patch-added MSI file is unsupported")
        require(sequence not in sequences, "duplicate MSI file sequence")
        sequences.add(sequence)
        cabinet_index = bisect_left(boundaries, sequence)
        require(cabinet_index < len(media), f"no cabinet for MSI File row: {file_id}")
        total += size
        require(total <= MAX_BYTES, "MSI expands beyond byte limit")
        payload.append({"fileId": file_id, "component": component, "path": path,
                        "size": size, "sequence": sequence, "cabinet": media[cabinet_index]["stream"]})
    for path in paths:
        require(not any(parent.as_posix() in paths for parent in PurePosixPath(path).parents if parent.as_posix() != "."),
                f"MSI file/directory collision: {path}")
    require({item["cabinet"] for item in payload} == {item["stream"] for item in media}, "MSI cabinet has no File rows")
    return sorted(payload, key=lambda item: item["path"]), media


def manifest_digest(payload):
    return hashlib.sha256(json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()).hexdigest()


def feature_layout(database):
    features = keyed(database.rows("Feature", ["Feature", "Feature_Parent", "Level", "Attributes"]), "Feature")
    components = keyed(database.rows("Component", ["Component", "Directory_"]), "Component")
    associations = {component: [] for component in components}
    for feature, component in database.rows("FeatureComponents", ["Feature_", "Component_"]):
        require(feature in features and component in associations, "orphan MSI FeatureComponents reference")
        require(feature not in associations[component], "duplicate MSI FeatureComponents reference")
        associations[component].append(feature)
    require(all(len(names) == 1 for names in associations.values()),
            "MSI components must each belong to exactly one feature")
    rows = [{"id": name, "parent": parent, "level": number(level, "Feature level", maximum=32767),
             "attributes": number(attributes or "0", "Feature attributes", maximum=32767)}
            for name, (parent, level, attributes) in sorted(features.items())]
    return rows, {component: names[0] for component, names in sorted(associations.items())}


def parse_profiles(raw):
    require(len(raw) <= 65536, "package profile policy exceeds size limit")
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, f"duplicate package profile key: {key}")
            result[key] = value
        return result
    policy = json.loads(raw, object_pairs_hook=unique)
    require(isinstance(policy, dict) and set(policy) == {"schema", "components", "archives", "windowsRuntime"}
            and type(policy["schema"]) is int and policy["schema"] == 1, "invalid package profile schema")
    known = {"runtime", "sdk", "tools", "templates", "samples", "redist"}
    def names(value):
        require(isinstance(value, list) and value and all(isinstance(name, str) and name in known for name in value)
                and len(set(value)) == len(value), "invalid package profile component set")
        return set(value)
    require(names(policy["components"]) == known, "package profile component vocabulary drift")
    archive, runtime = policy["archives"], policy["windowsRuntime"]
    require(isinstance(archive, dict) and set(archive) == {"components", "msvcAdditionalComponents"}, "invalid archive profile")
    require(names(archive["components"]) | names(archive["msvcAdditionalComponents"]) == known
            and not names(archive["components"]) & names(archive["msvcAdditionalComponents"]), "ambiguous archive profile")
    require(isinstance(runtime, dict) and set(runtime) == {"components", "msiRootFeature", "msiFeaturePrefix"}, "invalid runtime profile")
    names(runtime["components"])
    require(isinstance(runtime["msiRootFeature"], str) and IDENTIFIER.fullmatch(runtime["msiRootFeature"])
            and isinstance(runtime["msiFeaturePrefix"], str) and IDENTIFIER.fullmatch(runtime["msiFeaturePrefix"]),
            "invalid MSI profile feature identity")
    return policy


def profile_authority(read_committed):
    """Read the profile and classification rules from the report's immutable SHA."""
    raw_policy, raw_rules = read_committed(PROFILE_PATH), read_committed(RULES_PATH)
    require(len(raw_rules) <= 262144, "package notice rules exceed size limit")
    return parse_profiles(raw_policy), raw_rules, {
        "id": "windows-runtime", "profileSha256": hashlib.sha256(raw_policy).hexdigest(),
        "rulesSha256": hashlib.sha256(raw_rules).hexdigest()}


def prove_profile(evidence, policy):
    """Return per-file install components only after proving the exact MSI feature graph."""
    runtime = policy["windowsRuntime"]
    root, prefix = runtime["msiRootFeature"], runtime["msiFeaturePrefix"]
    feature_components = {prefix + component: component for component in runtime["components"]}
    features = {item["id"]: item for item in evidence["features"]}
    require(set(features) == {root, *feature_components}, "MSI features do not match committed windows-runtime profile")
    require(features[root]["parent"] == "" and all(features[name]["parent"] == root for name in feature_components),
            "MSI feature hierarchy does not match committed profile")
    assignments = evidence["componentFeatures"]
    require(all(feature in features for feature in assignments.values()), "unknown MSI component feature")
    result = {}
    for item in evidence["files"]:
        feature = assignments.get(item["component"])
        require(feature in feature_components, "MSI file belongs to an unclassified/root feature")
        result[item["path"]] = feature_components[feature]
    return result


def validate_evidence(evidence, count):
    """Publication rejects outer-hash-only MSI reports and incomplete manifests."""
    require(isinstance(evidence, dict) and set(evidence) == {"method", "scope", "root", "cabinets", "files", "manifestSha256",
                                                          "features", "componentFeatures"},
            "missing/invalid MSI extraction evidence")
    require(evidence["method"] == METHOD and evidence["scope"] == "all-file-table-components"
            and evidence["root"] == "INSTALL_ROOT", "unsupported MSI extraction method/scope")
    payload, cabinets = evidence["files"], evidence["cabinets"]
    require(type(count) is int and isinstance(payload, list) and len(payload) == count
            and 0 < count <= MAX_FILES, "MSI manifest file count mismatch")
    require(isinstance(cabinets, list) and 0 < len(cabinets) <= count, "missing/invalid MSI cabinet identities")
    paths, ids, sequences, streams = set(), set(), set(), set()
    for cabinet in cabinets:
        require(isinstance(cabinet, dict) and set(cabinet) == {"stream", "sha256", "size", "files"}, "invalid MSI cabinet identity")
        stream = cabinet["stream"]
        safe_path(stream)
        require("/" not in stream and stream.casefold() not in streams, "duplicate MSI cabinet identity")
        streams.add(stream.casefold())
        require(isinstance(cabinet["sha256"], str) and DIGEST.fullmatch(cabinet["sha256"])
                and type(cabinet["size"]) is int and 0 < cabinet["size"] <= MAX_BYTES
                and type(cabinet["files"]) is int and cabinet["files"] > 0, "invalid MSI cabinet digest/count")
    cabinet_counts = {cabinet["stream"]: 0 for cabinet in cabinets}
    for item in payload:
        require(isinstance(item, dict) and set(item) == {"fileId", "component", "path", "size", "sequence", "cabinet", "sha256"},
                "invalid MSI payload identity")
        path = safe_path(item["path"])
        require(path not in paths and isinstance(item["fileId"], str) and IDENTIFIER.fullmatch(item["fileId"])
                and item["fileId"].casefold() not in ids, "duplicate/invalid MSI payload identity")
        paths.add(path)
        ids.add(item["fileId"].casefold())
        require(isinstance(item["component"], str) and IDENTIFIER.fullmatch(item["component"]), "invalid MSI component identity")
        require(type(item["sequence"]) is int and 0 < item["sequence"] <= 32767
                and item["sequence"] not in sequences, "invalid MSI payload sequence")
        sequences.add(item["sequence"])
        require(type(item["size"]) is int and 0 <= item["size"] <= MAX_BYTES
                and isinstance(item["sha256"], str) and DIGEST.fullmatch(item["sha256"]), "invalid MSI payload digest/size")
        require(isinstance(item["cabinet"], str) and item["cabinet"] in cabinet_counts, "unbound MSI payload cabinet")
        cabinet_counts[item["cabinet"]] += 1
    for path in paths:
        require(not any(parent.as_posix() in paths for parent in PurePosixPath(path).parents if parent.as_posix() != "."),
                "MSI manifest file/directory collision")
    require(sum(item["size"] for item in payload) <= MAX_BYTES, "MSI manifest exceeds byte limit")
    require(all(cab["files"] == cabinet_counts[cab["stream"]] for cab in cabinets),
            "MSI cabinet file count mismatch")
    require(payload == sorted(payload, key=lambda item: item["path"])
            and evidence["manifestSha256"] == manifest_digest(payload), "MSI payload manifest digest/order mismatch")
    features, assignments = evidence["features"], evidence["componentFeatures"]
    require(isinstance(features, list) and 0 < len(features) <= MAX_FILES
            and isinstance(assignments, dict) and 0 < len(assignments) <= MAX_FILES, "missing MSI feature/component evidence")
    feature_ids = set()
    for feature in features:
        require(isinstance(feature, dict) and set(feature) == {"id", "parent", "level", "attributes"}, "invalid MSI feature record")
        name = feature["id"]
        require(isinstance(name, str) and IDENTIFIER.fullmatch(name) and name.casefold() not in feature_ids,
                "duplicate/invalid MSI feature identifier")
        feature_ids.add(name.casefold())
        require(isinstance(feature["parent"], str) and (not feature["parent"] or IDENTIFIER.fullmatch(feature["parent"]))
                and all(type(feature[key]) is int and 0 <= feature[key] <= 32767 for key in ("level", "attributes")),
                "invalid MSI feature metadata")
    require(all(isinstance(component, str) and IDENTIFIER.fullmatch(component) and isinstance(feature, str)
                and feature.casefold() in feature_ids for component, feature in assignments.items()), "invalid MSI component membership")
    require(len({component.casefold() for component in assignments}) == len(assignments)
            and all(item["component"] in assignments for item in payload), "missing/colliding MSI component membership")


@contextmanager
def extracted_payload(artifact):
    """Yield evidence, numeric notice file, artifact digest, and trusted raw-file map."""
    before = regular(artifact)
    require(artifact.suffix.lower() == ".msi" and before.st_size <= MAX_BYTES, "unsupported native package or size")
    outer_digest = digest(artifact)
    with tempfile.TemporaryDirectory(prefix="spark-msi-") as raw:
        workspace = Path(raw).resolve()
        private = workspace / "package.msi"
        with artifact.open("rb") as source, private.open("xb") as destination:
            remaining = before.st_size
            while remaining:
                chunk = source.read(min(1024 * 1024, remaining))
                require(chunk, "MSI shrank during private copy")
                destination.write(chunk)
                remaining -= len(chunk)
            require(not source.read(1), "MSI grew during private copy")
        regular(private)
        require(digest(private) == outer_digest and digest(artifact) == outer_digest, "MSI changed during private copy")
        with api.Database(private) as database:
            payload, media = payload_plan(database)
            features, component_features = feature_layout(database)
            by_cabinet = {cabinet["stream"]: [] for cabinet in media}
            for item in payload:
                by_cabinet[item["cabinet"]].append(item)
            cabinets, notice = [], None
            for index, cabinet in enumerate(media):
                cab = workspace / f"cabinet-{index}.cab"
                database.stream(cabinet["stream"], cab, MAX_BYTES)
                cab_digest = digest(cab)
                members = sorted(by_cabinet[cabinet["stream"]], key=lambda item: item["sequence"])
                targets = {item["fileId"]: (workspace / f"payload-{item['sequence']}", item["size"]) for item in members}
                api.extract_cabinet(cab, targets)
                require(digest(cab) == cab_digest, "embedded cabinet mutated during extraction")
                cabinets.append({"stream": cabinet["stream"], "sha256": cab_digest,
                                 "size": regular(cab).st_size, "files": len(members)})
                for item in members:
                    target = targets[item["fileId"]][0]
                    require(regular(target).st_size == item["size"], "extracted MSI file size mismatch")
                    item["sha256"] = digest(target)
                    if item["path"] == "THIRD_PARTY_NOTICES.txt":
                        notice = target
            require(notice is not None, "MSI has no root THIRD_PARTY_NOTICES.txt")
        evidence = {"method": METHOD, "scope": "all-file-table-components", "root": "INSTALL_ROOT",
                    "cabinets": cabinets, "files": payload, "manifestSha256": manifest_digest(payload),
                    "features": features, "componentFeatures": component_features}
        validate_evidence(evidence, len(payload))
        payload_files = {item["path"]: workspace / f"payload-{item['sequence']}" for item in payload}
        yield evidence, notice, outer_digest, payload_files
        # Include the caller's notice reconciliation in the mutation window.
        expected = {"package.msi", *(f"cabinet-{index}.cab" for index in range(len(media))),
                    *(f"payload-{item['sequence']}" for item in payload)}
        require({path.name for path in workspace.iterdir()} == expected, "unexpected extracted MSI payload")
        for item in payload:
            target = workspace / f"payload-{item['sequence']}"
            require(regular(target).st_size == item["size"] and digest(target) == item["sha256"], "extracted MSI payload mutated")
        after = regular(artifact)
        regular(private)
        require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns)
                == (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns)
                and digest(artifact) == outer_digest and digest(private) == outer_digest,
                "MSI changed while it was being reconciled")

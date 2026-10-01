#!/usr/bin/env python3
"""Bind final package inventories to published bytes and the committed source lock.

Archives are reconciled after extraction. Opaque installers fail closed: their
outer hashes do not prove an inventory, and a caller-supplied stage is not proof
of their final contents. A blocked record is retained for diagnosis, never
accepted by the publication verifier.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = "spark-package-inventory-reconciliation-v2"
SHA = re.compile(r"[0-9a-f]{40}")
DIGEST = re.compile(r"[0-9a-f]{64}")


def load_tool(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / filename)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


provenance = load_tool("release_reconciliation_provenance", "release_build_provenance.py")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate report field: {key}")
        result[key] = value
    return result


def source_identity(root, sha):
    require(SHA.fullmatch(sha), "source SHA must be lowercase 40-hex")
    require(provenance._git(root, "rev-parse", "HEAD").decode().strip() == sha,
            "source SHA does not match the checkout")
    return {"sha": sha, "dependencyLockSha256": provenance._committed_lock_digest(root, sha)}


def regular(path):
    require(path.is_file() and not path.is_symlink(), f"not a regular package file: {path}")
    return path


def safe_name(name):
    require(isinstance(name, str) and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", name),
            "artifact name must be a safe basename")
    return name


def create(packages, root, sha, platform, configuration, not_configured):
    sbom = load_tool("release_reconciliation_sbom", "generate-sbom.py")
    source = source_identity(root, sha)
    paths = sorted(regular(path) for path in packages.iterdir() if provenance._is_distributable(path.name))
    require(paths, "no final distributable packages found")
    require(len({path.name.casefold() for path in paths}) == len(paths), "package names collide ignoring case")
    inventory, rules = None, None
    artifacts, errors = [], []
    for path in paths:
        safe_name(path.name)
        digest = provenance._sha256_file(path)
        if path.name.lower().endswith((".zip", ".tar.gz", ".tgz")):
            if inventory is None:
                inventory, rules = sbom.load_inventory(root), sbom.load_rules()
            report = sbom.reconcile_archive(inventory, rules, path, not_configured, root)
            require(report.get("artifact", {}).get("sha256") == digest,
                    f"package changed during reconciliation: {path.name}")
            require(report.get("source") == source, "archive source identity drifted")
            errors.extend(f"{path.name}: {error}" for error in report["errors"])
        else:
            report = None
            errors.append(f"{path.name}: final native/standalone payload inventory has not been verified")
        artifacts.append({"name": path.name, "sha256": digest, "inventory": report})
    return {"schema": SCHEMA, "platform": platform, "configuration": configuration,
            "source": source, "artifacts": artifacts, "errors": errors}


def verify_documents(documents, published, source):
    """Require closed, nonempty reports and exact published-byte coverage, aliases included."""
    require(documents, "no reconciliation reports found")
    require(published, "no distributable release assets found")
    recorded = {}
    for report in documents:
        require(isinstance(report, dict) and set(report) == {
            "schema", "platform", "configuration", "source", "artifacts", "errors"}, "invalid report schema")
        require(report["schema"] == SCHEMA and report["source"] == source, "report source/lock or schema drift")
        require(report["errors"] == [], "final package reconciliation is blocked or failed")
        entries = report["artifacts"]
        require(isinstance(entries, list) and entries, "empty final package inventory")
        for entry in entries:
            require(isinstance(entry, dict) and set(entry) == {"name", "sha256", "inventory"},
                    "invalid artifact record")
            name, digest = safe_name(entry["name"]), entry["sha256"]
            require(name not in recorded, f"duplicate recorded artifact: {name}")
            require(isinstance(digest, str) and DIGEST.fullmatch(digest), "invalid artifact digest")
            inventory = entry["inventory"]
            require(isinstance(inventory, dict) and inventory.get("schema") == "spark-package-inventory-reconciliation-v1"
                    and inventory.get("errors") == [] and type(inventory.get("packageFiles")) is int
                    and inventory["packageFiles"] > 0 and inventory.get("source") == source,
                    f"missing or failed final payload inventory: {name}")
            artifact = inventory.get("artifact", {})
            require(artifact.get("name") == name and artifact.get("sha256") == digest,
                    f"inventory does not bind final artifact bytes: {name}")
            recorded[name] = digest
    # Every original must be published by name; an alias can supplement an
    # original only when its actual bytes are identical to a reconciled artifact.
    require(all(published.get(name) == digest for name, digest in recorded.items()),
            "recorded artifact missing from publication or changed")
    require(all(digest in set(recorded.values()) for digest in published.values()),
            "published artifact has no reconciled final inventory")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    record = sub.add_parser("record")
    record.add_argument("--packages", type=Path, required=True)
    record.add_argument("--platform", required=True)
    record.add_argument("--configuration", required=True)
    record.add_argument("--not-configured", action="append", default=[])
    record.add_argument("--report", type=Path, required=True)
    check = sub.add_parser("verify")
    check.add_argument("--reports-dir", type=Path, required=True)
    check.add_argument("--assets-dir", type=Path, required=True)
    check.add_argument("--assets-file", type=Path, required=True)
    for command in (record, check):
        command.add_argument("--source-root", type=Path, default=ROOT)
        command.add_argument("--source-sha", required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "record":
            result = create(args.packages, args.source_root, args.source_sha,
                            args.platform, args.configuration, args.not_configured)
            args.report.parent.mkdir(parents=True, exist_ok=True)
            with args.report.open("x", encoding="utf-8") as output:
                json.dump(result, output, indent=2, sort_keys=True)
                output.write("\n")
            require(not result["errors"], "; ".join(result["errors"]))
        else:
            documents = []
            for path in sorted(args.reports_dir.glob("reconcile-*.json")):
                require(regular(path).stat().st_size <= 8 * 1024 * 1024, "report exceeds size limit")
                documents.append(json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique_object))
            names = args.assets_file.read_text(encoding="utf-8").splitlines()
            require(len(names) == len(set(names)), "duplicate published asset name")
            published = {safe_name(name): provenance._sha256_file(regular(args.assets_dir / name))
                         for name in names if provenance._is_distributable(name)}
            verify_documents(documents, published, source_identity(args.source_root, args.source_sha))
    except (OSError, ValueError, provenance.ProvenanceError) as error:
        print(f"PACKAGE RECONCILIATION FAILURE: {error}", file=sys.stderr)
        return 1
    print("Final package inventory reconciles with the source lock and published bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

"""Bounded diagnostic orchestration; never certifies Windows 11 or installs MSI."""
import argparse
import hashlib
import importlib.util
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

SOURCE = "d90b86e42fd586a56d53f077bbca6a457200d9bd"
TARGETS = "SparkEngine SparkGameFPS SparkEditor SparkConsole SparkShaderCompiler SparkCrashReporter SparkCooker SparkWorker SparkAutomation SparkLauncher SparkBuild SparkInstaller SparkMismatchedModuleFixture SparkPreviousSdkModuleFixture".split()


# Keep cleanup inside each existing workflow step ceiling.
PHASE_SECONDS = {"build": 185 * 60 - 30, "sdk": 30 * 60 - 30,
                 "abi": 5 * 60 - 30, "closure": 15 * 60 - 30}
CLEANUP_SECONDS = 10
WRAPPER = (
    "import subprocess,sys; "
    "token=sys.stdin.buffer.readline(); "
    "sys.exit(subprocess.call(sys.argv[1:]) if token == b'GO\\n' else 125)"
)


def job_for(source):
    # Reuse the pinned product implementation; do not duplicate Windows ABI structs.
    name = "_spark_qualification_docs_contract"
    if name not in sys.modules:
        spec = importlib.util.spec_from_file_location(name, source / "tools/docs_contract.py")
        module = importlib.util.module_from_spec(spec)
        sys.modules[name] = module
        try:
            spec.loader.exec_module(module)
        except BaseException:
            del sys.modules[name]
            raise
    return sys.modules[name]._WindowsProcessJob()


def owned_run(command, *, cwd, stdout, stderr, timeout, check=True):
    """Release work only after the waiting wrapper belongs to a kill-on-close job."""
    job = job_for(cwd)
    process = None
    assigned = False
    try:
        process = subprocess.Popen([sys.executable, "-c", WRAPPER, *command],
                                   cwd=cwd, stdin=subprocess.PIPE, stdout=stdout, stderr=stderr)
        assigned = job.assign(process)
        if not assigned:
            raise RuntimeError("Windows process job assignment unavailable; command not released")
        process.stdin.write(b"GO\n")
        process.stdin.flush()
        process.stdin.close()
        code = process.wait(timeout=timeout)
        if check and code:
            raise subprocess.CalledProcessError(code, command)
        return subprocess.CompletedProcess(command, code)
    finally:
        # Close on success too: a command must not leave detached compiler children.
        job.close()
        if process is not None:
            if not assigned:
                process.kill()  # The unassigned wrapper is still waiting, with no children.
            try:
                if process.stdin is not None and not process.stdin.closed:
                    process.stdin.close()
            except OSError:
                pass
            process.wait(timeout=CLEANUP_SECONDS)


def remaining(deadline, ceiling):
    budget = min(ceiling, deadline - time.monotonic())
    if budget <= 0:
        raise TimeoutError("Qualification phase deadline exhausted")
    return budget


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def same_host(expected, *paths):
    if any(digest(path) != expected for path in paths):
        raise ValueError("Installed/build host identity changed")


def bind_graph(path, expected):
    graph = json.loads(path.read_text(encoding="utf-8"))
    hosts = [row for row in graph["images"] if row["path"].casefold() == "sparkengine.exe"]
    if len(hosts) != 1 or hosts[0]["sha256"] != expected:
        raise ValueError("Closure graph host identity mismatch")


def weather_source_identity(identity, git):
    if git("rev-parse", "HEAD") != identity["source_sha"] or git("status", "--porcelain", "--untracked-files=no"):
        raise ValueError("Weather source is not clean exact recorded source")
    for key, env in (("workflow_sha", "GITHUB_SHA"), ("run_id", "GITHUB_RUN_ID"), ("run_attempt", "GITHUB_RUN_ATTEMPT")):
        if identity.get(key) != os.environ.get(env):
            raise ValueError("Weather workflow/run identity changed")


def proof_json(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("Duplicate proof key")
            result[key] = value
        return result
    with path.open("rb") as stream:
        raw = stream.read(131073)
    if len(raw) > 131072 or b"\0" in raw:
        raise ValueError("Invalid bounded proof JSON")
    return json.loads(raw.decode("utf-8"), object_pairs_hook=unique,
                      parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))


def weather_proof(root, identity):
    receipt = proof_json(root / "sdk/weather-identity.json")
    expected = {key: identity[key] for key in ("source_sha", "workflow_sha", "run_id", "run_attempt", "host_sha256")}
    expected.update(module="SparkGeneratedGame", headless="unavailable", windowed="accepted", lifecycle="passed")
    if any(receipt.get(key) != value for key, value in expected.items()) or type(receipt.get("sdk_version")) is not int or receipt["sdk_version"] != 10:
        raise ValueError("Weather receipt identity/result mismatch")
    for key in ("consumer_sha256", "host_sha256", "module_sha256", "sidecar_sha256"):
        if not isinstance(receipt.get(key), str) or not re.fullmatch(r"[0-9a-f]{64}", receipt[key]):
            raise ValueError("Invalid weather image digest")
    names = ("runtime-stdout.log", "runtime-stderr.log", "runtime-weather-stdout.log", "runtime-weather-stderr.log")
    if not isinstance(receipt.get("streams"), dict) or set(receipt["streams"]) != set(names):
        raise ValueError("Incomplete weather streams")
    texts = []
    for name in names:
        with (root / "sdk" / name).open("rb") as stream:
            raw = stream.read(131073)
        if len(raw) > 131072 or b"\0" in raw or hashlib.sha256(raw).hexdigest() != receipt["streams"][name]:
            raise ValueError("Weather stream digest/bounds mismatch")
        texts.append(raw.decode("utf-8"))
    for available, pair in ((0, texts[:2]), (1, texts[2:])):
        records = [line for text in pair for line in text.splitlines() if "SPARK_SDK_WEATHER" in line]
        expected_record = (f"SPARK_SDK_WEATHER module=SparkGeneratedGame available={available} accepted={available} "
                           f"invalid_rejected={available} clear_accepted={available} callback=OnLoad")
        if records != [expected_record]:
            raise ValueError("Weather stream result mismatch")
    return receipt


def compact(root, weather_consumer=False, source_sha=None):
    identity = proof_json(root / "identity.json") if (root / "identity.json").exists() else {}
    enabled = identity.get("weather_consumer", False)
    if type(enabled) is not bool or (identity and enabled is not weather_consumer) or (weather_consumer and not identity and not (root / "build-failure.json").exists()):
        raise ValueError("Weather mode identity missing or inconsistent")
    if identity and source_sha is not None and identity.get("source_sha") != source_sha:
        raise ValueError("Collector source pin mismatch")
    # Failed SDK attempts retain available diagnostics, never a success receipt.
    if enabled and not (root / "sdk-binding.json").exists() and not any((root / name).exists() for name in ("build-failure.json", "sdk-failure.json")):
        raise ValueError("Weather SDK phase has no success binding or failure record")
    if enabled and (root / "sdk-binding.json").exists():
        binding = proof_json(root / "sdk-binding.json")
        if binding.get("passed") is not True or (root / "sdk-failure.json").exists():
            raise ValueError("Contradictory weather phase result")
        receipt = weather_proof(root, identity)
        if binding.get("weather_identity_sha256") != digest(root / "sdk/weather-identity.json") or binding.get("source_sha") != identity.get("source_sha") or binding.get("host_sha256") != receipt["host_sha256"]:
            raise ValueError("Weather phase binding mismatch")
    # Explicit evidence allowlist: never traverse a prefix, build tree or user data.
    files = list(root.glob("*.json")) + list(root.glob("*.log"))
    files += list((root / "sdk").glob("runtime-*.log"))
    files += list((root / "sdk").glob("weather-identity.json"))
    files += list((root / "abi").glob("*/report.json"))
    files += list((root / "abi").glob("*/*/*.log"))
    files += list((root / "closure").glob("*/*.json"))
    files += list((root / "closure").glob("*/*.log"))
    payloads = {}
    inventory = []
    for path in sorted(set(files)):
        name = path.relative_to(root).as_posix()
        parts = path.relative_to(root).parts
        runtime_proof = (name in ("sdk/runtime-stdout.log", "sdk/runtime-stderr.log",
                                 "sdk/runtime-weather-stdout.log", "sdk/runtime-weather-stderr.log") or
                         (len(parts) == 4 and parts[0] == "abi" and parts[2] in ("newer", "previous")
                          and parts[3] in ("stdout.log", "stderr.log")))
        size = path.stat().st_size
        # Complete proof must remain replayable and byte-identical to its hash.
        # Both structured reports and native stdout/stderr share a 128 KiB cap.
        if path.suffix == ".json" or runtime_proof:
            if size > 131072:
                kind = "Runtime proof" if runtime_proof else "Structured evidence"
                raise ValueError(kind + " exceeds compact budget: " + name)
            data = path.read_bytes()
            data.decode("utf-8")  # Fail on invalid proof text; never rewrite its bytes.
        else:
            with path.open("rb") as stream:
                stream.seek(max(0, size - 16384))
                data = stream.read()
            data = data.decode("utf-8", errors="replace").encode("utf-8")
        payloads[name] = data
        inventory.append({"path": name, "bytes": size, "sha256": digest(path),
                          "tail_only": len(data) != size})
    payloads["inventory.json"] = json.dumps(inventory, indent=2).encode()
    if sum(map(len, payloads.values())) > 900000:
        raise ValueError("Text evidence exceeds 900000-byte budget")
    archive = root / "diagnostics-size-check.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as out:
        for name, data in payloads.items():
            out.writestr(name, data)
    if archive.stat().st_size > 1000000:
        archive.unlink()
        raise ValueError("Diagnostics exceeds archive budget")
    frozen = root / "diagnostics-text"
    frozen.mkdir(exist_ok=False)
    for name, data in payloads.items():
        target = frozen / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)


def run_phase(phase, source, root, weather_consumer=False, source_sha=SOURCE):
    deadline = time.monotonic() + PHASE_SECONDS[phase]
    build = source / "build/windows-shipping"
    binary = build / "bin/MinSizeRel"
    installed = root / "sdk/prefix/bin/SparkEngine.exe"
    built = binary / "SparkEngine.exe"
    identity_path = root / "identity.json"

    def run(label, command, timeout):
        with (root / (label + ".log")).open("wb") as log:
            owned_run([str(x) for x in command], cwd=source, stdout=log,
                      stderr=subprocess.STDOUT, timeout=remaining(deadline, timeout), check=True)

    def git(*args):
        with tempfile.TemporaryFile() as output:
            owned_run(["git", "-C", str(source), *args], cwd=source, stdout=output,
                      stderr=subprocess.STDOUT, timeout=remaining(deadline, 30), check=True)
            output.seek(0)
            return output.read().decode("utf-8").strip()

    def cmake_script(name, extra):
        return ["cmake", f"-DSPARK_ENGINE_BUILD_DIR={build}", f"-DSPARK_SOURCE_ROOT={source}",
                "-DSPARK_CONFIG=MinSizeRel", f"-DSPARK_PYTHON_EXECUTABLE={sys.executable}",
                *extra, "-P", source / "Tests/PackageSmoke" / name]

    if phase == "build":
        if git("rev-parse", "HEAD") != source_sha or git("status", "--porcelain", "--untracked-files=no"):
            raise ValueError("Source checkout is not clean exact pinned source")
        submodules = git("submodule", "status", "--recursive")
        if any(line.startswith(("-", "+", "U")) for line in submodules.splitlines()):
            raise ValueError("Uninitialized or mismatched submodule")
        identity = {"source_sha": source_sha, "source_tree": git("rev-parse", "HEAD^{tree}"),
                    "workflow_sha": os.environ["GITHUB_SHA"], "helper_sha256": digest(__file__), "submodules": submodules,
                    "run_id": os.environ["GITHUB_RUN_ID"], "run_attempt": os.environ["GITHUB_RUN_ATTEMPT"],
                    "configuration": "MinSizeRel", "weather_consumer": weather_consumer, "scope": "hosted diagnostic, not Windows 11 certification",
                    "tools": {tool: shutil.which(tool) for tool in ("cl", "ninja", "cmake", "python")}}
        if not all(identity["tools"].values()):
            raise ValueError("Required preinstalled tool missing")
        save(identity_path, identity)
        for tool in ("cmake", "ninja", sys.executable):
            run("version-" + Path(tool).stem, [tool, "--version"], 15)
        run("configure", ["cmake", "--preset", "windows-shipping", "-DBUILD_TESTS=ON"], 900)
        run("build", ["cmake", "--build", build, "--config", "MinSizeRel", "--parallel", "1", "--target", *TARGETS], 10800)
        identity["images"] = {str(binary / name): digest(binary / name) for name in
                              ("SparkEngine.exe", "SparkGameFPS.dll", "SparkGameFPS.dll.sparkabi",
                               "SparkMismatchedModuleFixture.dll", "SparkMismatchedModuleFixture.dll.sparkabi",
                               "SparkPreviousSdkModuleFixture.dll", "SparkPreviousSdkModuleFixture.dll.sparkabi")}
        identity["host_sha256"] = digest(built)
        save(identity_path, identity)
        return
    identity = proof_json(identity_path)
    if identity.get("source_sha") != source_sha or identity.get("weather_consumer", False) is not weather_consumer:
        raise ValueError("Weather mode changed across phases")
    if weather_consumer:
        weather_source_identity(identity, git)
    expected = identity["host_sha256"]
    for path, recorded in identity["images"].items():
        if digest(path) != recorded:
            raise ValueError("Build input changed since identity capture: " + path)
    same_host(expected, built)
    if phase == "sdk":
        weather_args = []
        if weather_consumer:
            weather_args = ["-DSPARK_SDK_WEATHER_CONSUMER=ON", f"-DSPARK_WEATHER_SOURCE_SHA={source_sha}",
                            f"-DSPARK_WEATHER_WORKFLOW_SHA={identity['workflow_sha']}",
                            f"-DSPARK_WEATHER_RUN_ID={identity['run_id']}",
                            f"-DSPARK_WEATHER_RUN_ATTEMPT={identity['run_attempt']}"]
        run("sdk", cmake_script("RunInstalledSDKTemplate.cmake", [f"-DSPARK_TEST_ROOT={root / 'sdk'}",
            "-DSPARK_CONSUMER_GENERATOR=Ninja Multi-Config",
            f"-DSPARK_CONSUMER_MAKE_PROGRAM={shutil.which('ninja')}",
            f"-DSPARK_CONSUMER_COMPILER={shutil.which('cl')}",
            f"-DSPARK_REFERENCE_SIDECAR={binary / 'SparkGameFPS.dll.sparkabi'}", *weather_args]), 1780)
        same_host(expected, built, installed)
    elif phase == "abi":
        same_host(expected, built, installed)
        for component in ("samples", "redist"):
            run("install-" + component, ["cmake", "--install", build, "--config", "MinSizeRel", "--prefix",
                                        root / "sdk/prefix", "--component", component], 20)
        same_host(expected, built, installed)
        for name in ("SparkGameFPS.dll", "SparkGameFPS.dll.sparkabi"):
            if digest(root / "sdk/prefix/bin" / name) != digest(binary / name):
                raise ValueError("Installed reference differs from exact build: " + name)
        (root / "abi").mkdir(exist_ok=False)
        run("abi", [sys.executable, source / "Tests/PackageSmoke/run_installed_module_abi_rejection.py",
                    "--installed-root", root / "sdk/prefix", "--newer", binary / "SparkMismatchedModuleFixture.dll",
                    "--previous", binary / "SparkPreviousSdkModuleFixture.dll", "--evidence-root", root / "abi",
                    "--source-sha", source_sha, "--configuration", "MinSizeRel"], 250)
        same_host(expected, built, installed)
        reports = list((root / "abi").glob("*/report.json"))
        if len(reports) != 1:
            raise ValueError("Expected exactly one ABI report")
        report = json.loads(reports[0].read_text())
        if report["passed"] is not True or report["inputs"].get(str(installed.resolve())) != expected:
            raise ValueError("ABI report does not bind passing run to expected host")
    elif phase == "closure":
        run("closure", cmake_script("VerifyWindowsPackageClosure.cmake", [f"-DSPARK_TEST_ROOT={root / 'closure'}"]), 880)
        graphs = list((root / "closure").glob("*/*.import-graph.json"))
        if len(graphs) != 2:
            raise ValueError("Expected both closure graphs")
        for graph in graphs:
            bind_graph(graph, expected)
        same_host(expected, built, installed)
    else:
        raise ValueError("Unknown phase")
    for path, recorded in identity["images"].items():
        if digest(path) != recorded:
            raise ValueError("Build input changed during qualification: " + path)
    binding = {"passed": True, "host_sha256": expected, "source_sha": source_sha}
    if phase == "sdk" and weather_consumer:
        weather_proof(root, identity)
        binding["weather_identity_sha256"] = digest(root / "sdk/weather-identity.json")
    save(root / (phase + "-binding.json"), binding)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("phase", choices=("build", "sdk", "abi", "closure", "pack"))
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--weather-consumer", action="store_true")
    parser.add_argument("--source-sha")
    args = parser.parse_args()
    if args.weather_consumer and args.source_sha is None:
        parser.error("Weather mode requires explicit --source-sha")
    source_sha = SOURCE if args.source_sha is None else args.source_sha
    if not re.fullmatch(r"[0-9a-f]{40}", source_sha):
        parser.error("Expected exact lowercase 40-hex source SHA")
    root = args.root.resolve()
    source = args.source.resolve()
    runner_temp = Path(os.environ["RUNNER_TEMP"]).resolve(strict=True)
    if (root == source or root.is_relative_to(source) or source.is_relative_to(root)
            or root == runner_temp or not root.is_relative_to(runner_temp)
            or not str(root).isascii() or len(str(root)) > 80):
        raise ValueError("Evidence must be a short ASCII child of RUNNER_TEMP, disjoint from source")
    root.mkdir(parents=True, exist_ok=True)
    try:
        if args.phase == "pack":
            if args.weather_consumer and (root / "identity.json").exists():
                def pack_git(*git_args):
                    with tempfile.TemporaryFile() as output:
                        owned_run(["git", "-C", str(source), *git_args], cwd=source, stdout=output,
                                  stderr=subprocess.STDOUT, timeout=30, check=True)
                        output.seek(0)
                        return output.read().decode("utf-8").strip()
                weather_source_identity(proof_json(root / "identity.json"), pack_git)
            compact(root, args.weather_consumer, source_sha)
        else:
            run_phase(args.phase, source, root, args.weather_consumer, source_sha)
    except Exception as error:
        save(root / (args.phase + "-failure.json"), {"error": str(error), "phase": args.phase})
        raise


if __name__ == "__main__":
    main()

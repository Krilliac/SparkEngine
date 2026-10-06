"""Bounded diagnostic orchestration; never certifies Windows 11 or installs MSI."""
import argparse
import hashlib
import importlib.util
import json
import math
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile
import xml.etree.ElementTree as ET

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
    if type(receipt.get("boundary_scanned")) is not int or receipt["boundary_scanned"] < 2 or type(receipt.get("boundary_violations")) is not int or receipt["boundary_violations"] != 0:
        raise ValueError("Weather SDK boundary result missing or failed")
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


FOCUSED = (
    ("host", "TestSdkWeatherService.cpp", "SdkWeatherService_", 4),
    ("fps", "TestFPSWeatherPort.cpp", "FPSWeather", 8),
    ("state", "TestPrototypeModuleKitReal.cpp", "PrototypeModuleKit_StateRulesRegisterThroughSdkContext", 1),
    ("lifecycle", "TestModuleLifecycleReal.cpp", "ModuleLifecycleRecord_", 6),
    ("reflected", "TestSceneManagerReflectedReal.cpp", "SceneManager_ReflectedGameplay", 10),
)
FOCUSED_SECONDS = 60
FOCUSED_RESERVE = len(FOCUSED) * FOCUSED_SECONDS


def complete_text(path):
    with path.open("rb") as stream:
        data = stream.read(131073)
    if len(data) > 131072 or b"\0" in data:
        raise ValueError("Focused proof exceeds bounds or contains NUL: " + str(path))
    return data.decode("utf-8").replace("\r\n", "\n")


def focused_names(source, filename, prefix, count):
    text = (source / "Tests" / filename).read_text(encoding="utf-8")
    names = [name for name in re.findall(r"^TEST\(([A-Za-z_][A-Za-z_0-9]*)\)", text, re.M) if name.startswith(prefix)]
    if len(names) != count or len(set(names)) != count:
        raise ValueError("Focused source test census changed: " + filename)
    return names


def validate_focused(directory, label, names):
    streams = {kind: complete_text(directory / f"{label}-{kind}.{extension}")
               for kind, extension in (("stdout", "log"), ("stderr", "log"), ("output", "log"), ("junit", "xml"))}
    xml = streams["junit"]
    if "<!DOCTYPE" in xml or "<!ENTITY" in xml:
        raise ValueError("Unexpected focused XML declaration")
    root = ET.fromstring(xml)
    if root.tag != "testsuites" or len(root) != 1 or root[0].tag != "testsuite":
        raise ValueError("Focused JUnit suite shape mismatch")
    suite = root[0]
    for node in (root, suite):
        if node.get("tests") != str(len(names)) or any(node.get(key) != "0" for key in ("failures", "skipped", "flaky", "empty")) or node.get("errors", "0") != "0":
            raise ValueError("Focused JUnit counts/faults mismatch")
        elapsed = float(node.attrib["time"])
        if not math.isfinite(elapsed) or not 0 <= elapsed <= FOCUSED_SECONDS:
            raise ValueError("Focused JUnit invalid duration")
    cases = list(suite)
    if len(cases) != len(names) or {row.get("name") for row in cases} != set(names):
        raise ValueError("Focused JUnit cases mismatch")
    for row in cases:
        duration = float(row.attrib["time"])
        if row.tag != "testcase" or len(row) or row.get("status", "run") != "run" or not math.isfinite(duration) or not 0 <= duration <= FOCUSED_SECONDS:
            raise ValueError("Focused JUnit skipped/failed/empty case or duration")
    for kind in ("stdout", "output"):
        text = streams[kind]
        ok = re.findall(r"^\[   OK   \] ([A-Za-z_][A-Za-z_0-9]*) \([^\n]*, ([1-9][0-9]*) assertions\)$", text, re.M)
        if len(ok) != len(names) or {name for name, _ in ok} != set(names):
            raise ValueError("Focused plain case/assertion census mismatch")
        summaries = re.findall(r"^Tests: +([^\n]+)$", text, re.M)
        if summaries != [f"{len(names)} passed, 0 failed, {len(names)} total"]:
            raise ValueError("Focused plain summary mismatch")
        assertions = re.findall(r"^Assertions: +([1-9][0-9]*) passed, 0 failed$", text, re.M)
        if len(assertions) != 1 or int(assertions[0]) != sum(int(value) for _, value in ok):
            raise ValueError("Focused assertion summary mismatch")
    combined = "\n".join(streams[kind] for kind in ("stdout", "stderr", "output"))
    if re.search(r"\[\s*(?:FAILED|CRASH|SKIPPED|WARN|RETRY|EMPTY)\s*\]|^Error:|\.\.\. Output truncated|bytes of output removed", combined, re.M):
        raise ValueError("Focused proof contains fault/truncation")
    return {f"{label}-{kind}.{extension}": digest(directory / f"{label}-{kind}.{extension}")
            for kind, extension in (("stdout", "log"), ("stderr", "log"), ("output", "log"), ("junit", "xml"))}


def focused_native(source, root, image, identity, deadline):
    directory = root / "focused"
    directory.mkdir(exist_ok=False)
    before = digest(image)
    report = {key: identity[key] for key in ("source_sha", "workflow_sha", "run_id", "run_attempt")}
    report.update(test_image_sha256=before, families=[], passed=False)
    for label, filename, prefix, count in FOCUSED:
        names = focused_names(source, filename, prefix, count)
        user = directory / (label + "-user")
        for folder in ("tmp", "local", "roaming"):
            (user / folder).mkdir(parents=True, exist_ok=False)
        command = ["cmake", "-E", "env", "--unset=SPARK_TEST_NAME", "--unset=SPARK_TEST_LIMIT", "--unset=SPARK_TEST_EXCLUDE",
                   f"SPARK_TEST_FILE={filename}", f"SPARK_TEST_NAME_PREFIX={prefix}", f"SPARK_TEST_EXPECT_COUNT={count}",
                   f"TEMP={user / 'tmp'}", f"TMP={user / 'tmp'}", f"LOCALAPPDATA={user / 'local'}", f"APPDATA={user / 'roaming'}",
                   str(image), "--warn-is-error", "--empty-is-error", "--output-file", str(directory / f"{label}-output.log"),
                   "--junit-xml", str(directory / f"{label}-junit.xml")]
        timeout = remaining(deadline, FOCUSED_SECONDS)
        command_record = {"argv": command, "cwd": str(source), "timeout_seconds": timeout, "names": names}
        save(directory / f"{label}-command.json", command_record)
        if digest(image) != before:
            raise ValueError("Focused test image changed before execution")
        with (directory / f"{label}-stdout.log").open("wb") as output, (directory / f"{label}-stderr.log").open("wb") as error:
            result = owned_run(command, cwd=source, stdout=output, stderr=error, timeout=timeout, check=True)
        if result.returncode != 0 or digest(image) != before:
            raise ValueError("Focused native result/image mismatch")
        command_record["exit_code"] = result.returncode
        save(directory / f"{label}-command.json", command_record)
        hashes = validate_focused(directory, label, names)
        hashes[f"{label}-command.json"] = digest(directory / f"{label}-command.json")
        report["families"].append(dict(label=label, source_file=filename, source_sha256=digest(source / "Tests" / filename), names=names, files=hashes, exit_code=0))
    report["passed"] = True
    save(directory / "report.json", report)
    return {"test_image_sha256": before, "report_sha256": digest(directory / "report.json")}


def focused_proof(root, identity):
    bound = identity["focused"]
    directory = root / "focused"
    if digest(directory / "report.json") != bound["report_sha256"]:
        raise ValueError("Focused report digest mismatch")
    report = proof_json(directory / "report.json")
    images = [value for path, value in identity.get("images", {}).items() if path.replace("\\", "/").rsplit("/", 1)[-1] == "SparkTests.exe"]
    if images != [bound["test_image_sha256"]]:
        raise ValueError("Focused test image not bound to build identity")
    if report.get("passed") is not True or any(report.get(key) != identity[key] for key in ("source_sha", "workflow_sha", "run_id", "run_attempt")) or report.get("test_image_sha256") != bound["test_image_sha256"]:
        raise ValueError("Focused identity/result mismatch")
    if len(report["families"]) != len(FOCUSED):
        raise ValueError("Focused family census mismatch")
    for row, (label, filename, prefix, count) in zip(report["families"], FOCUSED):
        names = row["names"]
        if row["label"] != label or row["source_file"] != filename or type(row.get("exit_code")) is not int or row["exit_code"] != 0 or len(names) != count or len(set(names)) != count or not all(name.startswith(prefix) for name in names):
            raise ValueError("Focused family binding mismatch")
        command = proof_json(directory / f"{label}-command.json")
        timeout = command.get("timeout_seconds")
        argv = command.get("argv", [])
        if (type(timeout) not in (int, float) or not math.isfinite(timeout) or not 0 < timeout <= FOCUSED_SECONDS
                or type(command.get("exit_code")) is not int or command["exit_code"] != 0 or command.get("names") != names
                or argv[:3] != ["cmake", "-E", "env"]
                or not all(argv.count(value) == 1 for value in (f"SPARK_TEST_FILE={filename}", f"SPARK_TEST_NAME_PREFIX={prefix}", f"SPARK_TEST_EXPECT_COUNT={count}", "--warn-is-error", "--empty-is-error", "--unset=SPARK_TEST_NAME", "--unset=SPARK_TEST_LIMIT", "--unset=SPARK_TEST_EXCLUDE"))):
            raise ValueError("Focused command contract mismatch")
        image_paths = [path for path in identity["images"] if path.replace("\\", "/").rsplit("/", 1)[-1] == "SparkTests.exe"]
        if len(image_paths) != 1 or argv.count(image_paths[0]) != 1:
            raise ValueError("Focused command image mismatch")
        hashes = validate_focused(directory, label, names)
        hashes[f"{label}-command.json"] = digest(directory / f"{label}-command.json")
        if hashes != row["files"]:
            raise ValueError("Focused proof file digest mismatch")


def weather_deadline(root, phase):
    clock = proof_json(root / "clock.json")
    now = time.monotonic()
    end = clock.get("deadline")
    if type(end) not in (int, float) or not math.isfinite(end) or end <= 0 or end > now + 240 * 60:
        raise ValueError("Invalid qualification shared clock")
    reserve = 30 if phase == "pack" else 330
    ceiling = 270 if phase == "pack" else PHASE_SECONDS[phase]
    deadline = min(now + ceiling, end - reserve)
    remaining(deadline, ceiling)  # Never release a child after the shared budget expires.
    return deadline


INPUT_SECONDS = 90
INPUT_RESERVE = 150  # Owner90 plus asset/identity/proof checks; within closure ceiling.
INPUT_SOURCES = (
    "Tests/PackageSmoke/RunFPSInputDispatch.py", "Tests/PackageSmoke/fps_owned_save_decode.py",
    "Tests/TestFPSOwnedSaveDecode.cpp", "Tests/CMakeLists.txt",
    "tools/asset-integrity/verify_asset_integrity.py", "tools/asset-integrity/package_closure.py",
    "tools/asset-integrity/asset_references.py", "tools/asset-integrity/package-profiles.json",
    "tools/asset-integrity/provenance.json", "Assets/assets.integrity.json",
    "SparkEngine/Source/Input/InputManager.h", "SparkEngine/Source/Input/InputManager.cpp",
    "GameModules/SparkGameFPS/Source/Game/Game.h", "GameModules/SparkGameFPS/Source/Game/Game.cpp",
    "GameModules/SparkGameFPS/Source/Game/GameEngineSystems.cpp",
)


def input_plain(path):
    for part in (path, *path.parents):
        if part.is_symlink() or (hasattr(part, "is_junction") and part.is_junction()):
            raise ValueError("Linked input evidence/source path")
    return path


def input_source_identity(identity, source, capture=False, git=None):
    if git is not None and set(git("ls-files", "--error-unmatch", *INPUT_SOURCES).splitlines()) != set(INPUT_SOURCES):
        raise ValueError("Input helper/source is not tracked in pinned product")
    current = {name: digest(input_plain(source / name)) for name in INPUT_SOURCES}
    if capture:
        identity["input_sources"] = current
    elif current != identity.get("input_sources"):
        raise ValueError("Input source/helper identity changed")


def input_module(name, path):
    spec = importlib.util.spec_from_file_location(name, input_plain(path))
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def input_text(path):
    input_plain(path)
    with path.open("rb") as stream:
        raw = stream.read(131073)
    if len(raw) > 131072 or b"\0" in raw:
        raise ValueError("Input proof exceeds complete text cap")
    return raw.decode("utf-8")


def input_images(identity, root, source):
    prefix = root / "sdk/prefix/bin"
    binary = source / "build/windows-shipping/bin/MinSizeRel"
    result = {}
    for name in ("SparkEngine.exe", "SparkGameFPS.dll", "SparkGameFPS.dll.sparkabi", "SparkTests.exe"):
        built = binary / name
        if digest(input_plain(built)) != identity["images"].get(str(built)):
            raise ValueError("Input binary differs from qualified build: " + name)
        target = built if name == "SparkTests.exe" else prefix / name
        if digest(input_plain(target)) != identity["images"][str(built)]:
            raise ValueError("Input installed image differs from qualified build: " + name)
        result[str(target)] = identity["images"][str(built)]
    return result


def input_package(root, deadline):
    prefix = input_plain(root / "sdk/prefix")
    for parent in (prefix, prefix / "bin"):
        if any((parent / name).exists() for name in ("Startup.sparkscene", "Saves")):
            raise ValueError("Installed input closure contains startup override or inherited saves")
    result = {}
    for path in sorted(prefix.rglob("*")):
        remaining(deadline, 1)
        input_plain(path)
        if path.is_file():
            if len(result) >= 10000:
                raise ValueError("Installed input closure inventory exceeds bound")
            result[path.relative_to(prefix).as_posix()] = digest(path)
    if not all(name in result for name in ("bin/SparkEngine.exe", "bin/SparkGameFPS.dll",
                                           "bin/SparkGameFPS.dll.sparkabi",
                                           "bin/Assets/Models/pistol.obj", "bin/Assets/Models/rifle.obj")):
        raise ValueError("Installed input closure lacks required actual FPS content")
    # Bind the complete installed tree without uploading its binary contents.
    raw = json.dumps(result, sort_keys=True, separators=(",", ":")).encode()
    manifest = prefix / "bin/Assets/assets.integrity.json"
    assets = proof_json(manifest)
    return {"files": len(result), "sha256": hashlib.sha256(raw).hexdigest(),
            "asset_manifest_sha256": digest(manifest), "asset_entries": len(assets["entries"])}


def input_sequence(driver, replay, saved, missing=False):
    frames = replay.frames
    if not frames or any(frames[0][key] != 0 for key in ("mask", "pressed", "released", "operation")):
        raise ValueError("Input trace lacks released initial frame")
    edges = [(i, f) for i, f in enumerate(frames) if f["pressed"] or f["released"]]
    expected = [(2, 2, 0), (0, 0, 2)] if missing else [
        (4, 4, 0), (0, 0, 4), (1, 1, 0), (0, 0, 1), (8, 8, 0), (0, 0, 8),
        (2, 2, 0), (0, 0, 2), (2, 2, 0), (0, 0, 2)]
    if [(f["mask"], f["pressed"], f["released"]) for _, f in edges] != expected:
        raise ValueError("Incomplete or unexpected input edge sequence")
    mask = 0
    for frame in frames:
        if frame["pressed"]:
            if mask or frame["released"]:
                raise ValueError("Overlapping input keys")
            mask = frame["pressed"]
        elif frame["released"]:
            if frame["released"] != mask:
                raise ValueError("Release without held key")
            mask = 0
        if frame["mask"] != mask:
            raise ValueError("Unobserved input transition")
    if missing:
        return
    if edges[0][1]["profile"]["fps.profile.class"] != driver.SCOUT or saved["fps.profile.class"] != driver.SCOUT:
        raise ValueError("F5 did not select saved Scout")
    if (edges[4][1]["profile"]["fps.profile.class"] != driver.VANGUARD
            or driver.VANGUARD == saved["fps.profile.class"]):
        raise ValueError("F9 did not mutate live class")
    for edge_index, action, serial in ((2, 1, 1), (6, 2, 2), (8, 2, 3)):
        index, first = edges[edge_index]
        release_index, release = edges[edge_index + 1]
        if first["action"] != action or first["operation"] != serial or first["result"] != 1:
            raise ValueError("Wrong input dispatch result")
        if release_index < index + 3:
            raise ValueError("Missing two completed held frames")
        previous = first
        for frame in frames[index + 1:index + 3]:
            if (frame["input"] != previous["input"] + 1 or frame["update"] != previous["update"] + 1
                    or frame["mask"] != first["mask"] or frame["action"] != 0 or frame["operation"] != serial):
                raise ValueError("Nonconsecutive or repeated held dispatch")
            previous = frame
        if action == 2:
            operation = replay.operations[serial - 1]
            if operation["profile"] != saved or operation["transfer"] != saved:
                raise ValueError("Immediate restored profile mismatch")
            for frame in frames[index:index + 3] + [release]:
                driver.restored_frame(frame, saved, first["update"])


def input_proof(root, identity, source):
    input_source_identity(identity, source)
    driver = input_module("_qualified_input_replay", source / INPUT_SOURCES[0])
    decoder = input_module("_qualified_input_decode", source / INPUT_SOURCES[1])
    base = input_plain(root / "input")
    report = proof_json(base / "receipt.json")
    negative = proof_json(base / "missing-slot/receipt.json")
    images = input_images(identity, root, source)
    expected_images = {name: value for name, value in images.items() if not name.endswith(".sparkabi")}
    if (report.get("passed") is not True or report.get("fullShippingQualification") is not False
            or report.get("source") != identity["source_sha"] or report.get("identities") != expected_images
            or report.get("decoderHelperSha256") != identity["input_sources"][INPUT_SOURCES[1]]
            or report.get("missingSlot") != negative or negative.get("passed") is not True
            or negative.get("case") != "missing-slot"):
        raise ValueError("Input receipt/image/source binding mismatch")
    elapsed = report.get("elapsedSeconds")
    if type(elapsed) not in (int, float) or not math.isfinite(elapsed) or not 0 <= elapsed <= INPUT_SECONDS:
        raise ValueError("Input owner time invalid")
    members = [base / "receipt.json", base / "game.log", base / "missing-slot/receipt.json",
               base / "missing-slot/game.log"]
    for directory, missing in ((base, False), (base / "missing-slot", True)):
        replay = driver.Replay(directory / "local/SparkEngine/Saves", missing_only=missing)
        lines = input_text(directory / "game.log").splitlines(keepends=True)
        for line in lines:
            replay.feed(line.replace("\r\n", "\n"))
        driver.check_lifecycle(lines)
        scene_rows = [line for line in lines if "FPS scene identity:" in line]
        if len(scene_rows) != 1:
            raise ValueError("Missing/ambiguous authored FPS scene")
        scene = re.search(r'FPS scene identity: authored scene "FPS Arena" \(([1-9][0-9]*) nodes\) from (.+?)\s*$', scene_rows[0])
        expected_scene = root / "sdk/prefix/bin/Assets/Scenes/level1.scene"
        if scene is None or Path(scene[2]).resolve() != expected_scene.resolve():
            raise ValueError("Input ran fallback or wrong authored scene")
        if not replay.ended or [r["action"] for r in replay.operations] != ([2] if missing else [1, 2, 2]):
            raise ValueError("Input complete trace/action mismatch")
        input_sequence(driver, replay, report.get("savedProfile"), missing)
        if missing:
            if replay.operations[0] != negative.get("operation"):
                raise ValueError("Negative operation differs from actual trace")
        elif replay.operations[0]["transfer"] != report.get("savedProfile"):
            raise ValueError("Saved profile differs from actual dispatch")
    work = Path(report.get("decoderWork", ""))
    if (not work.is_absolute() or work.parent != base or not work.name.startswith("save-decode-")
            or list(base.glob("save-decode-*")) != [work]):
        raise ValueError("Ambiguous/unowned decoder evidence")
    input_plain(work)
    decoded = proof_json(work / "content-receipt.json")
    if decoded != report.get("decoded") or decoded.get("canonicalProfile") != report.get("savedProfile"):
        raise ValueError("Decoded profile receipt mismatch")
    binary = source / "build/windows-shipping/bin/MinSizeRel/SparkTests.exe"
    if (decoded.get("decoderExecutable", {}).get("sha256") != images[str(binary)]
            or decoded.get("input", {}).get("sha256") != report.get("slotSha256")):
        raise ValueError("Decoder/save identity mismatch")
    stdout, stderr = input_text(work / "stdout.log"), input_text(work / "stderr.log")
    parsed = decoder.parse_receipt(stdout, stderr, decoded["input"])
    decoder.framework(work / "framework.xml")
    if parsed["canonicalProfile"] != decoded["canonicalProfile"]:
        raise ValueError("Native decoded content mismatch")
    junit = work / "framework.xml"
    if decoded.get("frameworkJUnit") != {"sha256": digest(junit), "bytes": junit.stat().st_size}:
        raise ValueError("Decoder JUnit binding mismatch")
    members += [work / name for name in ("stdout.log", "stderr.log", "framework.xml", "content-receipt.json")]
    return {str(path.relative_to(root).as_posix()): {"sha256": digest(path), "bytes": len(input_text(path).encode("utf-8"))}
            for path in members}


def run_input_phase(root, identity, source, deadline, run):
    if remaining(deadline, INPUT_RESERVE) < INPUT_RESERVE:
        raise TimeoutError("Closure has no complete input owner/verification reserve")
    abi = proof_json(root / "abi-binding.json")
    if abi.get("passed") is not True or abi.get("source_sha") != identity["source_sha"]:
        raise ValueError("Input requires successful same-source installed ABI phase")
    input_source_identity(identity, source)
    images = input_images(identity, root, source)
    run("input-assets", [sys.executable, "-B", source / "tools/asset-integrity/verify_asset_integrity.py",
        "verify", root / "sdk/prefix/bin/Assets/assets.integrity.json", "--root", root / "sdk/prefix/bin/Assets",
        "--profile", "stable-v1", "--source-manifest", source / "Assets/assets.integrity.json",
        "--require-provenance"], 30)
    package = input_package(root, deadline)
    if remaining(deadline, INPUT_SECONDS + 10) < INPUT_SECONDS + 10:
        raise TimeoutError("Input identity verification consumed owner allowance")
    run("input-dispatch", [sys.executable, source / INPUT_SOURCES[0], "--source", source,
        "--source-sha", identity["source_sha"], "--host", root / "sdk/prefix/bin/SparkEngine.exe",
        "--module", root / "sdk/prefix/bin/SparkGameFPS.dll",
        "--decoder", source / "build/windows-shipping/bin/MinSizeRel/SparkTests.exe",
        "--decoder-helper", source / INPUT_SOURCES[1], "--decoder-helper-sha256",
        identity["input_sources"][INPUT_SOURCES[1]], "--out", root / "input"], INPUT_SECONDS)
    members = input_proof(root, identity, source)
    assets_log = root / "input-assets.log"
    members["input-assets.log"] = {"bytes": len(input_text(assets_log).encode("utf-8")), "sha256": digest(assets_log)}
    if input_package(root, deadline) != package:
        raise ValueError("Installed closure changed during input verification")
    remaining(deadline, 1)
    binding = {key: identity[key] for key in ("source_sha", "workflow_sha", "run_id", "run_attempt", "helper_sha256")}
    binding.update(passed=True, images=images, input_sources=identity["input_sources"], package=package, members=members)
    save(root / "input-binding.json", binding)


def lineage_adapter():
    return input_module('_installed_lineage_adapter', Path(__file__).with_name('qualify_installed_lineage.py'))


def compact(root, weather_consumer=False, source_sha=None, deadline=None, input_dispatch=False, source=None, editor_lineage=False):
    def check_budget():
        if deadline is not None:
            remaining(deadline, 270)
    check_budget()
    identity = proof_json(root / "identity.json") if (root / "identity.json").exists() else {}
    if identity and identity.get('editor_lineage', False) is not editor_lineage:
        raise ValueError('Lineage mode changed at collection')
    lineage_files = []
    if editor_lineage:
        if source is None:
            raise ValueError('Lineage collection requires product source')
        if (root/'lineage/binding.json').exists():
            if (root/'lineage/failure.json').exists() or (root/'build-failure.json').exists():
                raise ValueError('Contradictory lineage success/failure')
            lineage_files = lineage_adapter().proof(sys.modules[__name__], root, source, identity)
            if identity.get('lineage_binding_sha256') != digest(root/'lineage/binding.json'):
                raise ValueError('Lineage result not bound to build identity')
        elif not (root/'build-failure.json').exists():
            raise ValueError('Missing complete installed lineage result')
    enabled = identity.get("weather_consumer", False)
    if type(enabled) is not bool or (identity and enabled is not weather_consumer) or (weather_consumer and not identity and not (root / "build-failure.json").exists()):
        raise ValueError("Weather mode identity missing or inconsistent")
    if identity and source_sha is not None and identity.get("source_sha") != source_sha:
        raise ValueError("Collector source pin mismatch")
    if enabled and "focused" in identity:
        focused_proof(root, identity)
    elif enabled and not (root / "build-failure.json").exists():
        raise ValueError("Weather build lacks focused native proof")
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
    input_members = {}
    if identity and identity.get("input_dispatch", False) is not input_dispatch:
        raise ValueError("Input mode changed at collection")
    if input_dispatch:
        preidentity_failure = not identity and (root / "build-failure.json").is_file()
        if (not enabled and not preidentity_failure) or source is None:
            raise ValueError("Input collection requires pinned weather source")
        if (root / "input-binding.json").exists():
            binding = proof_json(root / "input-binding.json")
            for key in ("source_sha", "workflow_sha", "run_id", "run_attempt", "helper_sha256"):
                if binding.get(key) != identity.get(key):
                    raise ValueError("Stale input binding: " + key)
            if binding.get("passed") is not True or any((root / name).exists() for name in
                    ("build-failure.json", "sdk-failure.json", "abi-failure.json", "closure-failure.json")):
                raise ValueError("Contradictory input success/failure")
            input_members = input_proof(root, identity, source)
            assets_log = root / "input-assets.log"
            input_members["input-assets.log"] = {"bytes": len(input_text(assets_log).encode("utf-8")), "sha256": digest(assets_log)}
            if binding.get("members") != input_members or binding.get("images") != input_images(identity, root, source):
                raise ValueError("Input proof changed since closure")
            if binding.get("input_sources") != identity.get("input_sources"):
                raise ValueError("Input source binding changed")
            closure = proof_json(root / "closure-binding.json")
            if closure.get("input_binding_sha256") != digest(root / "input-binding.json"):
                raise ValueError("Closure lacks exact input result binding")
        elif not any((root / name).exists() for name in
                ("build-failure.json", "sdk-failure.json", "abi-failure.json", "closure-failure.json")):
            raise ValueError("Input mode lacks required complete result")
    # Explicit evidence allowlist: never traverse a prefix, build tree or user data.
    files = list(root.glob("*.json")) + list(root.glob("*.log"))
    files += lineage_files
    if editor_lineage:
        files += [p for p in (root/'lineage').glob('*.json') if p.name != 'binding.json']
        files += list((root/'lineage').glob('*.log'))
        files += list((root/'lineage/diagnostics').rglob('*.log'))
        files += list((root/'lineage/failure-proof').glob('*.xml'))
    files += list((root / "focused").glob("*.json")) + list((root / "focused").glob("*.log")) + list((root / "focused").glob("*.xml"))
    files += list((root / "sdk").glob("runtime-*.log"))
    files += list((root / "sdk").glob("weather-identity.json"))
    files += list((root / "abi").glob("*/report.json"))
    files += list((root / "abi").glob("*/*/*.log"))
    files += list((root / "closure").glob("*/*.json"))
    files += list((root / "closure").glob("*/*.log"))
    if input_dispatch:
        files += [root / name for name in input_members]
        if not input_members:
            # Partial failure retains only bounded named text; binary saves are excluded.
            for relative in ("input/game.log", "input/receipt.json", "input/missing-slot/game.log",
                             "input/missing-slot/receipt.json"):
                if (root / relative).is_file():
                    files.append(root / relative)
            for work in (root / "input").glob("save-decode-*"):
                for name in ("stdout.log", "stderr.log", "framework.xml", "content-receipt.json"):
                    if (work / name).is_file():
                        files.append(work / name)
    payloads = {}
    inventory = []
    for path in sorted(set(files)):
        check_budget()
        name = path.relative_to(root).as_posix()
        parts = path.relative_to(root).parts
        input_plain(path)
        runtime_proof = (path in lineage_files or parts[:2] == ('lineage','failure-proof') or parts[0] in ("focused", "input") or name == "input-assets.log" or name in ("sdk/runtime-stdout.log", "sdk/runtime-stderr.log",
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
    check_budget()
    payloads["inventory.json"] = json.dumps(inventory, indent=2).encode()
    if sum(map(len, payloads.values())) > 900000:
        raise ValueError("Text evidence exceeds 900000-byte budget")
    archive = root / "diagnostics-size-check.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as out:
        for name, data in payloads.items():
            check_budget()
            out.writestr(name, data)
    if archive.stat().st_size > 1000000:
        archive.unlink()
        raise ValueError("Diagnostics exceeds archive budget")
    check_budget()
    frozen = root / "diagnostics-text"
    frozen.mkdir(exist_ok=False)
    for name, data in payloads.items():
        check_budget()
        target = frozen / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    check_budget()


def run_phase(phase, source, root, weather_consumer=False, source_sha=SOURCE, input_dispatch=False, editor_lineage=False):
    if editor_lineage and not weather_consumer:
        raise ValueError('Installed lineage requires weather qualification mode')
    if input_dispatch and not weather_consumer:
        raise ValueError("Input dispatch requires weather mode")
    deadline = weather_deadline(root, phase) if weather_consumer else time.monotonic() + PHASE_SECONDS[phase]
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
                    "configuration": "MinSizeRel", "weather_consumer": weather_consumer, "input_dispatch": input_dispatch, "editor_lineage": editor_lineage, "scope": "hosted diagnostic, not Windows 11 certification",
                    "tools": {tool: shutil.which(tool) for tool in ("cl", "ninja", "cmake", "python")}}
        if not all(identity["tools"].values()):
            raise ValueError("Required preinstalled tool missing")
        if input_dispatch:
            input_source_identity(identity, source, capture=True, git=git)
        if editor_lineage:
            identity['lineage_adapter_sha256'] = digest(Path(__file__).with_name('qualify_installed_lineage.py'))
            identity['lineage_validator_sha256'] = digest(Path(__file__).with_name('qualify-editor-fps.py'))
        save(identity_path, identity)
        for tool in ("cmake", "ninja", sys.executable):
            run("version-" + Path(tool).stem, [tool, "--version"], 15)
        configure = ["cmake", "--preset", "windows-shipping", "-DBUILD_TESTS=ON"]
        if editor_lineage:
            configure.append('-DPython3_EXECUTABLE='+sys.executable)
        run("configure", configure, 900)
        if editor_lineage:
            run('lineage-prebuild', ['ctest','--test-dir',build,'-C','MinSizeRel','-R','^EditorFPSLineage_InstalledRuntime$','--show-only=json-v1'],30)
            configured = proof_json(root/'lineage-prebuild.log')
            lineage_adapter().validators().discovery(configured, True, source, build, source_sha)
            save(root/'lineage-prebuild.json', configured)
        targets = [*TARGETS, "SparkTests"] if weather_consumer else TARGETS
        build_seconds = remaining(deadline, 10800)
        if weather_consumer:
            build_seconds -= FOCUSED_RESERVE
            if editor_lineage:
                build_seconds -= lineage_adapter().RESERVE
            if build_seconds <= 0:
                raise TimeoutError("No build allowance remains after focused native reserve")
        run("build", ["cmake", "--build", build, "--config", "MinSizeRel", "--parallel", "1", "--target", *targets], build_seconds)
        if weather_consumer:
            weather_source_identity(identity, git)
            identity["focused"] = focused_native(source, root, binary / "SparkTests.exe", identity, deadline)
        identity["images"] = {str(binary / name): digest(binary / name) for name in
                              ("SparkEngine.exe", "SparkGameFPS.dll", "SparkGameFPS.dll.sparkabi",
                               "SparkMismatchedModuleFixture.dll", "SparkMismatchedModuleFixture.dll.sparkabi",
                               "SparkPreviousSdkModuleFixture.dll", "SparkPreviousSdkModuleFixture.dll.sparkabi")}
        if weather_consumer:
            identity["images"][str(binary / "SparkTests.exe")] = identity["focused"]["test_image_sha256"]
        if editor_lineage:
            for name in ('SparkCooker.exe', 'SparkCrashReporter.exe'):
                identity['images'][str(binary/name)] = digest(binary/name)
        identity["host_sha256"] = digest(built)
        save(identity_path, identity)
        if editor_lineage:
            identity['lineage_binding_sha256'] = lineage_adapter().run(sys.modules[__name__],root,source,identity,deadline)
            weather_source_identity(identity, git)
            for path, recorded in identity['images'].items():
                if digest(path) != recorded:
                    raise ValueError('Build image changed during installed lineage')
            save(identity_path, identity)
        return
    identity = proof_json(identity_path)
    if identity.get('editor_lineage', False) is not editor_lineage:
        raise ValueError('Lineage mode changed across phases')
    if editor_lineage and identity.get('lineage_adapter_sha256') != digest(Path(__file__).with_name('qualify_installed_lineage.py')):
        raise ValueError('Lineage adapter changed across phases')
    if editor_lineage and identity.get('lineage_validator_sha256') != digest(Path(__file__).with_name('qualify-editor-fps.py')):
        raise ValueError('Lineage validator changed across phases')
    if identity.get("source_sha") != source_sha or identity.get("weather_consumer", False) is not weather_consumer:
        raise ValueError("Weather mode changed across phases")
    if weather_consumer:
        weather_source_identity(identity, git)
    if identity.get("input_dispatch", False) is not input_dispatch:
        raise ValueError("Input mode changed across phases")
    if input_dispatch:
        input_source_identity(identity, source, git=git)
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
        closure_seconds = remaining(deadline, 880)
        if input_dispatch:
            closure_seconds -= INPUT_RESERVE
            if closure_seconds <= 0:
                raise TimeoutError("No closure allowance after input reserve")
        run("closure", cmake_script("VerifyWindowsPackageClosure.cmake", [f"-DSPARK_TEST_ROOT={root / 'closure'}"]), closure_seconds)
        graphs = list((root / "closure").glob("*/*.import-graph.json"))
        if len(graphs) != 2:
            raise ValueError("Expected both closure graphs")
        for graph in graphs:
            bind_graph(graph, expected)
        same_host(expected, built, installed)
        if input_dispatch:
            run_input_phase(root, identity, source, deadline, run)
    else:
        raise ValueError("Unknown phase")
    for path, recorded in identity["images"].items():
        if digest(path) != recorded:
            raise ValueError("Build input changed during qualification: " + path)
    binding = {"passed": True, "host_sha256": expected, "source_sha": source_sha}
    if phase == "sdk" and weather_consumer:
        weather_proof(root, identity)
        binding["weather_identity_sha256"] = digest(root / "sdk/weather-identity.json")
    if phase == "closure" and input_dispatch:
        binding["input_binding_sha256"] = digest(root / "input-binding.json")
    save(root / (phase + "-binding.json"), binding)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("phase", choices=("build", "sdk", "abi", "closure", "pack"))
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--weather-consumer", action="store_true")
    parser.add_argument("--input-dispatch", action="store_true")
    parser.add_argument("--editor-lineage", action="store_true")
    parser.add_argument("--source-sha")
    args = parser.parse_args()
    if args.editor_lineage and (not args.weather_consumer or args.source_sha is None):
        parser.error('--editor-lineage requires explicit source and weather mode')
    if args.input_dispatch and (not args.weather_consumer or args.source_sha is None):
        parser.error("Input dispatch requires weather mode and explicit --source-sha")
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
            pack_deadline = weather_deadline(root, "pack") if args.weather_consumer else None
            if args.weather_consumer and (root / "identity.json").exists():
                def pack_git(*git_args):
                    with tempfile.TemporaryFile() as output:
                        owned_run(["git", "-C", str(source), *git_args], cwd=source, stdout=output,
                                  stderr=subprocess.STDOUT, timeout=remaining(pack_deadline, 30), check=True)
                        output.seek(0)
                        return output.read().decode("utf-8").strip()
                weather_source_identity(proof_json(root / "identity.json"), pack_git)
                if args.input_dispatch:
                    input_source_identity(proof_json(root / "identity.json"), source, git=pack_git)
            compact(root, args.weather_consumer, source_sha, pack_deadline, args.input_dispatch, source, args.editor_lineage)
        else:
            run_phase(args.phase, source, root, args.weather_consumer, source_sha, args.input_dispatch, args.editor_lineage)
    except Exception as error:
        save(root / (args.phase + "-failure.json"), {"error": str(error), "phase": args.phase})
        raise


if __name__ == "__main__":
    main()

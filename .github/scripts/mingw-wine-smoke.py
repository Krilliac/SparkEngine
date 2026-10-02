#!/usr/bin/env python3
"""Experimental CI-100 Wine execution: fresh CPU-render/control evidence, never a native fallback."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
FRAMES = 60
MINIMUM_TESTS = 7500


def exclusions():
    groups = json.loads((ROOT / ".github/scripts/mingw-wine-exclusions.json").read_text(encoding="utf-8"))
    return ",".join(name for names in groups.values() for name in names)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def wine_path(path):
    return "Z:" + Path(path).resolve().as_posix()


def check_cpu_log(directory):
    logs = list(directory.glob("*_d3d11.log"))
    require(logs, "No fresh DXVK D3D11 log")
    for log in logs:
        # DXVK 3.x writes its version in DXGI's log and the selected device in
        # D3D11's log. Require the matching pair from this fresh launch directory.
        dxgi = log.with_name(log.name.removesuffix("_d3d11.log") + "_dxgi.log")
        require(dxgi.is_file(), "No matching fresh DXVK DXGI log")
        version = dxgi.read_text(encoding="utf-8", errors="replace")
        require(re.search(r"DXVK:\s+v?3\.1\.1\b", version), "Pinned DXVK did not run")
        device = log.read_text(encoding="utf-8", errors="replace")
        require(re.search(r"info:\s+Creating device:\s*\ninfo:\s+(?:llvmpipe|lavapipe)\b", device, re.I),
                "DXVK did not identify a CPU adapter")


def check_capture(path):
    """Validate bounded 8-bit RGB/RGBA PNGs emitted by ScreenCapture's miniz encoder."""
    require(path.stat().st_size <= 4 * 1024 * 1024, "Capture is oversized")
    data = path.read_bytes()
    require(data[:8] == b"\x89PNG\r\n\x1a\n", "Capture is not a PNG")
    offset, header, compressed, ended = 8, None, bytearray(), False
    while offset + 12 <= len(data):
        length = struct.unpack_from(">I", data, offset)[0]
        kind = data[offset + 4:offset + 8]
        end = offset + 8 + length
        require(end + 4 <= len(data), "Truncated PNG chunk")
        body = data[offset + 8:end]
        require(zlib.crc32(kind + body) == struct.unpack_from(">I", data, end)[0], "PNG checksum mismatch")
        if kind == b"IHDR":
            require(header is None and offset == 8 and length == 13, "Invalid PNG header")
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            require(header is not None, "PNG data precedes its header")
            compressed.extend(body)
        elif kind == b"IEND":
            require(length == 0 and end + 4 == len(data), "Invalid PNG end")
            ended = True
        offset = end + 4
    require(ended and header is not None, "Incomplete PNG")
    width, height, depth, color, compression, filtering, interlace = header
    # -window-size is the Win32 outer window; title bars/borders reduce the
    # backbuffer's client dimensions (CreateWindowW in SparkEngineWindowsWin32.cpp).
    require(320 <= width <= 640 and 240 <= height <= 480, "Unexpected engine client capture dimensions")
    require(depth == 8 and color in (2, 6) and (compression, filtering, interlace) == (0, 0, 0),
            "Unsupported capture PNG format")
    channels = 4 if color == 6 else 3
    stride = width * channels
    expected = (stride + 1) * height
    decoder = zlib.decompressobj()
    raw = decoder.decompress(compressed, expected + 1)
    require(len(raw) == expected and decoder.eof and not decoder.unused_data, "Invalid PNG pixel stream")
    previous, first, varied = bytearray(stride), None, False
    for y in range(height):
        start = y * (stride + 1)
        mode, row = raw[start], bytearray(raw[start + 1:start + 1 + stride])
        require(mode <= 4, "Invalid PNG filter")
        for x in range(stride):
            left = row[x - channels] if x >= channels else 0
            above = previous[x]
            corner = previous[x - channels] if x >= channels else 0
            if mode == 1:
                row[x] = (row[x] + left) & 255
            elif mode == 2:
                row[x] = (row[x] + above) & 255
            elif mode == 3:
                row[x] = (row[x] + (left + above) // 2) & 255
            elif mode == 4:
                prediction = left + above - corner
                nearest = min((left, above, corner), key=lambda value: abs(prediction - value))
                row[x] = (row[x] + nearest) & 255
        for x in range(0, stride, channels):
            rgb = bytes(row[x:x + 3])
            if first is None:
                first = rgb
            varied |= rgb != first
        previous = row
    require(varied, "Capture contains only the clear color")
    return hashlib.sha256(data).hexdigest()


def check_engine(directory):
    text = (directory / "exec-audit.log").read_text(encoding="utf-8")
    for command in ("gfx_vsync off", "gfx_screenshot", "gfx_metrics"):
        require(re.search(r"^frame \d+ t=[\d.]+s \| ok  \| " + command, text, re.M),
                f"Missing successful agent command: {command}")
    require("VSync disabled" in text, "Agent command did not change VSync")
    require(re.search(r"^frame 59 t=[\d.]+s \| ok  \| gfx_metrics$", text, re.M),
            "Engine did not reach the last requested frame")
    require(re.search(r"Draw Calls: [1-9][0-9]*", text), "Scene produced no draw calls")
    require(re.search(r"Triangles: [1-9][0-9]*", text), "Scene produced no triangles")
    launch = (directory / "engine.log").read_text(encoding="utf-8", errors="replace")
    require(re.search(r"^SPARK_SCENE_LOADED entities=3 renderables=1\s*$", launch, re.M),
            "The authored scene was not loaded")
    check_cpu_log(directory)
    return {"launched": True, "framesReached": FRAMES, "cpuRendering": True, "agentControl": True,
            "captures": {p.name: check_capture(p) for p in
                         (directory / "frame-05.png", directory / "frame-55.png")}}


def check_editor(directory):
    receipt = json.loads((directory / "editor-result.json").read_text(encoding="utf-8"))
    require(receipt.get("schema") == 1 and receipt.get("status") == "passed"
            and receipt.get("projectLoaded") is True and receipt.get("runResult") == 0,
            "Editor did not load its project and exit cleanly")
    require(receipt.get("graphicsBackend") == "d3d11" and receipt.get("renderedFrames", 0) >= FRAMES
            and receipt.get("presentFailures") == 0, "Editor did not present the requested D3D11 frames")
    check_cpu_log(directory)
    return {"launched": True, "cpuRendering": True, "agentControl": True, "receipt": receipt}


def check_tests(text):
    matches = re.findall(r"^Tests:\s+(\d+) passed, (\d+) failed([^\n]*)$", text, re.M)
    require(len(matches) == 1, "Expected one terminal SparkTests summary")
    passed, failed, rest = matches[0]
    require(int(passed) >= MINIMUM_TESTS, f"Fewer than {MINIMUM_TESTS} tests passed")
    require(int(failed) == 0 and not re.search(r"[1-9][0-9]* warned", rest), "Wine tests failed or warned")
    return {"testsRun": True, "passedTests": int(passed), "minimumPassedTests": MINIMUM_TESTS}


def run_wine(executable, args, directory, env, log_name, seconds, working_directory=None):
    command = ["xvfb-run", "-a", "-s", "-screen 0 1280x720x24", "bash",
               str(ROOT / "tools/wine-run.sh"), str(executable), *args]
    with (directory / log_name).open("w", encoding="utf-8") as log:
        process = subprocess.Popen(command, cwd=working_directory or directory, env=env, stdout=log,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        try:
            code = process.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            raise ValueError(f"Wine process exceeded {seconds}s") from None
    require(code == 0, f"Wine exited {code}; see {log_name} (255 is not success)")


def execute(phase, build, directory):
    require(os.name == "posix", "Run this smoke on Linux/WSL with Wine and Xvfb")
    icd = next((p for p in (Path("/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"),
                           Path("/usr/share/vulkan/icd.d/lvp_icd.json")) if p.is_file()), None)
    require(icd is not None, "Mesa Lavapipe ICD is missing")
    env = os.environ.copy()
    # Never allow shell defaults to turn this proof into native, NullRHI or hardware execution.
    env.update(SPARK_SKIP_WINE="0", SPARK_WINE_AUTO_TIER4="0", SPARK_WINE_GVISOR_SHIM="0",
               SPARK_WINE_NO_AUTO_FLAGS="1", SPARK_WINE_BACKEND="dxvk", SPARK_FORCE_SOFTWARE_GFX="1",
               SPARK_RHI_BACKEND="d3d11", VK_ICD_FILENAMES=str(icd), VK_DRIVER_FILES=str(icd),
               DXVK_FILTER_DEVICE_NAME="llvmpipe", DXVK_LOG_LEVEL="info", DXVK_LOG_PATH=wine_path(directory),
               DXVK_PATH=str(ROOT / "ThirdParty/dxvk/x64"), WINEDEBUG="-all",
               WINEPREFIX=str(build / ".wineprefix-mingw"), LIBGL_ALWAYS_SOFTWARE="1")
    for key in ("SPARK_TEST_FILE", "SPARK_TEST_NAME", "SPARK_TEST_NAME_PREFIX", "SPARK_TEST_LIMIT",
                "SPARK_TEST_EXPECT_COUNT", "SPARK_WINE_PROBE", "WINEDLLOVERRIDES"):
        env.pop(key, None)
    exe = build / "bin" / {"engine": "SparkEngine.exe", "editor": "SparkEditor.exe",
                           "tests": "SparkTests.exe"}[phase]
    require(exe.is_file(), f"Missing executable: {exe}")
    if phase == "tests":
        require(env.get("SPARK_TEST_EXCLUDE") == exclusions(), "Supply exactly the reviewed Wine-only exclusions")
        run_wine(exe, ["--warn-is-error"], directory, env, "tests.log", 1800, ROOT)
        return check_tests((directory / "tests.log").read_text(encoding="utf-8", errors="replace"))
    if phase == "engine":
        # Run from the repository root for assets. Do not symlink the asset
        # tree into evidence: artifact uploaders would recursively include it.
        script = directory / "agent.exec"
        script.write_text('0 gfx_vsync off\n5 gfx_screenshot "' + wine_path(directory / "frame-05.png")
                          + '"\n55 gfx_screenshot "' + wine_path(directory / "frame-55.png")
                          + '"\n59 gfx_metrics\n', encoding="utf-8")
        run_wine(exe, ["-scene", wine_path(ROOT / "Tests/Fixtures/EditorScene/Scenes/EditorSeeded.sparkscene"),
                       "-window-size", "640x480", "-test-frames", str(FRAMES), "-threads", "1",
                       "-no-subprocess", "-no-jobsystem", "-exec", wine_path(script),
                       "-exec-audit", wine_path(directory / "exec-audit.log")],
                 directory, env, "engine.log", 180, ROOT)
        return check_engine(directory)
    project = directory / "WineSmoke.sparkproject"
    project.write_text(json.dumps({"projectFileVersion": 1, "name": "WineSmoke", "version": "1.0.0",
                                  "engineVersion": "1.0.0", "template": "empty", "defaultScene": "",
                                  "lastOpenedScene": "", "createdTime": 0, "lastModified": 0,
                                  "modules": [], "scenes": []}), encoding="utf-8")
    (directory / "Assets").mkdir()
    (directory / "Scenes").mkdir()
    scene = directory / "Scenes/WineSmoke.sparkscene"
    # Drive the editor's real save command, then reopen that scene through the real open command.
    run_wine(exe, ["--test-mode", "--project", wine_path(project), "--save-scene", wine_path(scene)],
             directory, env, "editor-save.log", 180)
    require(scene.is_file() and json.loads(scene.read_text(encoding="utf-8")).get("entities"),
            "Editor did not save the seeded scene")
    # The second launch must create its own DXVK log, not reuse save-only device initialization.
    for log in (*directory.glob("*_d3d11.log"), *directory.glob("*_dxgi.log")):
        log.unlink()
    run_wine(exe, ["--test-mode", "--test-frames", str(FRAMES), "--project", wine_path(project),
                   "--open-scene", wine_path(scene), "--smoke-result", wine_path(directory / "editor-result.json")],
             directory, env, "editor.log", 180)
    return check_editor(directory)


def summarize(output):
    report = {"experimental": True, "commit": os.environ.get("GITHUB_SHA", "local-uncommitted"),
              "configured": os.environ.get("MINGW_CONFIGURED", "not-run"),
              "builtEngine": os.environ.get("MINGW_ENGINE_BUILT", "not-run"),
              "builtEditor": os.environ.get("MINGW_EDITOR_BUILT", "not-run"),
              "builtTests": os.environ.get("MINGW_TESTS_BUILT", "not-run")}
    for phase in ("engine", "editor", "tests"):
        path = output / (phase + ".json")
        report[phase] = json.loads(path.read_text()) if path.is_file() else {"status": "not-run"}
    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    markdown = "## MinGW/Wine (experimental)\n\nCPU rendering for GPU-less servers and agents.\n\n```json\n"
    markdown += json.dumps(report, indent=2) + "\n```\n\nA build or launch alone is not runtime proof.\n"
    markdown += "Boolean stages mean confirmed evidence; false means not verified.\n"
    (output / "summary.md").write_text(markdown, encoding="utf-8")
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as stream:
            stream.write(markdown)
    print(markdown)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase", choices=("engine", "editor", "tests", "summary", "exclusions"))
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/linux-mingw-release")
    parser.add_argument("--output", type=Path, default=ROOT / "build/mingw-wine-evidence")
    args = parser.parse_args()
    if args.phase == "exclusions":
        print(exclusions())
        return 0
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if args.phase == "summary":
        summarize(output)
        return 0
    # Never accept screenshots, logs or receipts left by an earlier process.
    directory = Path(tempfile.mkdtemp(prefix=args.phase + "-", dir=output))
    report = {"status": "running", "directory": str(directory), "launched": False,
              "cpuRendering": False, "testsRun": False}
    receipt = output / (args.phase + ".json")
    receipt.write_text(json.dumps(report), encoding="utf-8")
    try:
        report.update(execute(args.phase, args.build_dir.resolve(), directory))
        report["status"] = "passed"
    except (OSError, ValueError, TypeError, ImportError, zlib.error, struct.error) as error:
        report.update(status="failed", error=str(error))
        # A later rendering/check failure must not erase proof of an earlier successful launch.
        if args.phase == "engine" and (directory / "engine.log").is_file():
            launch = (directory / "engine.log").read_text(encoding="utf-8", errors="replace")
            report["launched"] = bool(re.search(r"^SPARK_SCENE_LOADED entities=3 renderables=1\s*$", launch, re.M))
        elif args.phase == "editor" and (directory / "editor-result.json").is_file():
            try:
                report["launched"] = json.loads((directory / "editor-result.json").read_text()).get("projectLoaded") is True
            except (ValueError, OSError):
                pass
        elif args.phase == "tests" and (directory / "tests.log").is_file():
            output_text = (directory / "tests.log").read_text(encoding="utf-8", errors="replace")
            report["testsRun"] = bool(re.search(r"^\[\s*RUN\s*\]", output_text, re.M))
            report["testSummaryPresent"] = bool(re.search(r"^Tests:", output_text, re.M))
    receipt.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())

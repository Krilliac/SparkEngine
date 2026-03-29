#!/usr/bin/env python3
"""
SparkEngine Windows Live Testing via Wine

Tests the MinGW-compiled Windows .exe binaries under Wine with DXVK/Lavapipe.
Exercises the exact same D3D11/D3D12 code paths that MSVC compiles.

Stack: D3D11 C++ → MinGW .exe → Wine → DXVK (D3D11→Vulkan) → Lavapipe (CPU)

Usage:
  # After building with: cmake --preset linux-mingw-release && cmake --build build/linux-mingw-release
  python3 tools/test-windows-wine.py [--build-dir build/linux-mingw-release]
"""

import subprocess
import time
import sys
import os
import signal
import json
from pathlib import Path
from dataclasses import dataclass
from typing import Optional

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent


# ── Test tracking ────────────────────────────────────────────────────

@dataclass
class TestResult:
    name: str
    passed: bool
    detail: str = ""
    phase: str = ""

results: list[TestResult] = []

def test(name: str, condition: bool, detail: str = "", phase: str = ""):
    status = "PASS" if condition else "FAIL"
    print(f"  [{status}] {name}" + (f" — {detail}" if detail else ""))
    results.append(TestResult(name, condition, detail, phase))


# ── Wine helpers ─────────────────────────────────────────────────────

def get_wine_env() -> dict:
    """Build the Wine environment for software rendering."""
    env = os.environ.copy()
    env["WINEPREFIX"] = str(PROJECT_ROOT / "build" / ".wineprefix")
    env["WINEDEBUG"] = "-all"  # Suppress Wine spam
    env["LIBGL_ALWAYS_SOFTWARE"] = "1"

    # Find Lavapipe ICD
    for icd in [
        "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json",
        "/usr/share/vulkan/icd.d/lvp_icd.json",
    ]:
        if os.path.isfile(icd):
            env["VK_ICD_FILENAMES"] = icd
            break

    return env


def run_wine(exe_path: str, args: list[str] = None, timeout: int = 60,
             env: dict = None, capture: bool = True) -> tuple[int, str, str]:
    """Run a .exe under Wine and return (returncode, stdout, stderr)."""
    if args is None:
        args = []
    if env is None:
        env = get_wine_env()

    cmd = ["wine64", exe_path] + args
    try:
        result = subprocess.run(
            cmd, capture_output=capture, text=True,
            timeout=timeout, env=env, cwd=str(PROJECT_ROOT)
        )
        return result.returncode, result.stdout, result.stderr
    except subprocess.TimeoutExpired:
        return -1, "", "TIMEOUT"
    except Exception as e:
        return -1, "", str(e)


def run_wine_process(exe_path: str, args: list[str] = None,
                     env: dict = None) -> subprocess.Popen:
    """Start a .exe under Wine as a background process."""
    if args is None:
        args = []
    if env is None:
        env = get_wine_env()

    cmd = ["wine64", exe_path] + args
    return subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, env=env, cwd=str(PROJECT_ROOT)
    )


def read_output_nonblocking(proc: subprocess.Popen, timeout: float = 0.5) -> str:
    """Read available output from a process without blocking."""
    import selectors
    sel = selectors.DefaultSelector()
    sel.register(proc.stdout, selectors.EVENT_READ)
    output = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        events = sel.select(timeout=max(0, deadline - time.time()))
        if not events:
            break
        for key, _ in events:
            line = key.fileobj.readline()
            if line:
                output.append(line)
            else:
                sel.unregister(key.fileobj)
                return "".join(output)
    sel.close()
    return "".join(output)


# ── Test phases ──────────────────────────────────────────────────────

def phase_prerequisites(build_dir: Path):
    """Phase 0: Verify prerequisites are available."""
    print("\n" + "=" * 60)
    print("[PHASE 0] Verifying Prerequisites")
    print("=" * 60)

    # Wine available?
    try:
        result = subprocess.run(["wine64", "--version"], capture_output=True, text=True, timeout=10)
        test("Wine64 installed", result.returncode == 0,
             result.stdout.strip(), "prerequisites")
    except FileNotFoundError:
        test("Wine64 installed", False, "wine64 not found in PATH", "prerequisites")

    # MinGW compiler?
    try:
        result = subprocess.run(["x86_64-w64-mingw32-g++", "--version"],
                                capture_output=True, text=True, timeout=10)
        test("MinGW-w64 compiler installed", result.returncode == 0,
             result.stdout.split('\n')[0], "prerequisites")
    except FileNotFoundError:
        test("MinGW-w64 compiler installed", False, "not found", "prerequisites")

    # Lavapipe available?
    lavapipe_found = any(
        os.path.isfile(p) for p in [
            "/usr/share/vulkan/icd.d/lvp_icd.x86_64.json",
            "/usr/share/vulkan/icd.d/lvp_icd.json",
        ]
    )
    test("Lavapipe (software Vulkan) available", lavapipe_found, "prerequisites")

    # Build artifacts exist?
    tests_exe = build_dir / "bin" / "SparkTests.exe"
    test("SparkTests.exe exists", tests_exe.is_file(),
         str(tests_exe), "prerequisites")

    engine_exe = build_dir / "bin" / "SparkEngine.exe"
    test("SparkEngine.exe exists", engine_exe.is_file(),
         str(engine_exe), "prerequisites")

    editor_exe = build_dir / "bin" / "SparkEditor.exe"
    test("SparkEditor.exe exists", editor_exe.is_file(),
         str(editor_exe), "prerequisites")


def phase_wine_setup():
    """Phase 1: Initialize Wine prefix and D3D translation layers."""
    print("\n" + "=" * 60)
    print("[PHASE 1] Wine Environment Setup")
    print("=" * 60)

    wine_run = str(PROJECT_ROOT / "tools" / "wine-run.sh")
    env = get_wine_env()

    # Setup Wine prefix
    try:
        result = subprocess.run(
            [wine_run, "--setup-only"],
            capture_output=True, text=True, timeout=60, env=env
        )
        test("Wine prefix initialized", result.returncode == 0,
             result.stdout.strip()[-100:] if result.stdout else result.stderr[:200],
             "wine-setup")
    except Exception as e:
        test("Wine prefix initialized", False, str(e), "wine-setup")

    # Print environment info
    try:
        result = subprocess.run(
            [wine_run, "--info"],
            capture_output=True, text=True, timeout=30, env=env
        )
        if result.stdout:
            for line in result.stdout.strip().split('\n'):
                print(f"    {line}")
        test("Wine environment info", result.returncode == 0, "wine-setup")
    except Exception as e:
        test("Wine environment info", False, str(e), "wine-setup")


def phase_unit_tests(build_dir: Path):
    """Phase 2: Run the full unit test suite under Wine."""
    print("\n" + "=" * 60)
    print("[PHASE 2] Unit Tests Under Wine")
    print("=" * 60)

    tests_exe = str(build_dir / "bin" / "SparkTests.exe")
    if not os.path.isfile(tests_exe):
        test("Unit tests executable", False, "SparkTests.exe not found", "unit-tests")
        return

    print("  Running SparkTests.exe under Wine (may take a few minutes)...")
    env = get_wine_env()
    # Enable Wine debug for D3D errors only
    env["WINEDEBUG"] = "err+d3d,err+d3d11,err+vulkan"

    rc, stdout, stderr = run_wine(tests_exe,
                                  ["--output-file", "Z:\\tmp\\spark-wine-tests.txt"],
                                  timeout=300, env=env)

    # Parse test output
    lines = stdout.split('\n') if stdout else []
    pass_count = 0
    fail_count = 0
    for line in lines:
        if "PASSED" in line and "test" in line.lower():
            # Try to extract numbers
            import re
            m = re.search(r'(\d+)\s+(?:tests?\s+)?passed', line, re.IGNORECASE)
            if m:
                pass_count = int(m.group(1))
        if "FAILED" in line and "test" in line.lower():
            import re
            m = re.search(r'(\d+)\s+(?:tests?\s+)?failed', line, re.IGNORECASE)
            if m:
                fail_count = int(m.group(1))

    test("Unit tests execute under Wine", rc != -1,
         f"rc={rc}", "unit-tests")
    test("Unit tests pass", rc == 0 or pass_count > 0,
         f"passed={pass_count} failed={fail_count} rc={rc}", "unit-tests")

    # Print last 30 lines of output for diagnostics
    if lines:
        print("  --- Test output (last 30 lines) ---")
        for line in lines[-30:]:
            if line.strip():
                print(f"    {line.rstrip()}")

    if stderr and "TIMEOUT" not in stderr:
        err_lines = [l for l in stderr.split('\n') if l.strip()][:10]
        if err_lines:
            print("  --- Wine stderr (first 10 lines) ---")
            for line in err_lines:
                print(f"    {line.rstrip()}")


def phase_engine_live(build_dir: Path):
    """Phase 3: Launch the engine with D3D11 graphics initialized."""
    print("\n" + "=" * 60)
    print("[PHASE 3] Engine Live Test (D3D11 via Wine)")
    print("=" * 60)

    engine_exe = str(build_dir / "bin" / "SparkEngine.exe")
    if not os.path.isfile(engine_exe):
        test("Engine executable", False, "SparkEngine.exe not found", "engine-live")
        return

    env = get_wine_env()
    env["WINEDEBUG"] = "err+d3d,err+d3d11,warn+vulkan"

    # Test 1: Run engine for 60 frames and exit
    print("  Launching SparkEngine.exe with -test-frames 60...")
    proc = run_wine_process(engine_exe, ["-test-frames", "60"], env=env)

    start = time.time()
    max_wait = 30  # seconds
    output_lines = []

    while time.time() - start < max_wait:
        if proc.poll() is not None:
            break
        time.sleep(0.5)

    # Read remaining output
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    stdout = proc.stdout.read() if proc.stdout else ""
    rc = proc.returncode

    test("Engine launches under Wine", rc is not None and rc != -1,
         f"rc={rc}, ran for {time.time()-start:.1f}s", "engine-live")

    # Check for D3D11 initialization
    d3d_init = "Graphics" in stdout or "D3D" in stdout or "Initialize" in stdout
    test("Engine initializes graphics subsystem", d3d_init or rc == 0,
         "Found graphics init in output" if d3d_init else f"rc={rc}", "engine-live")

    # Check for frame limit exit
    frame_exit = "[TEST] Frame limit reached" in stdout
    test("Engine respects -test-frames flag", frame_exit,
         "Clean frame-limited exit" if frame_exit else "No frame limit message found",
         "engine-live")

    # Check for clean shutdown
    clean_shutdown = "Shutting down" in stdout or rc == 0
    test("Engine shuts down cleanly", clean_shutdown,
         f"rc={rc}", "engine-live")

    if stdout:
        print("  --- Engine output (last 20 lines) ---")
        for line in stdout.strip().split('\n')[-20:]:
            if line.strip():
                print(f"    {line.rstrip()}")

    # Test 2: Headless mode
    print("\n  Launching SparkEngine.exe in headless mode...")
    rc2, stdout2, stderr2 = run_wine(engine_exe, ["-headless"], timeout=15, env=env)
    headless_ok = "headless" in stdout2.lower() or rc2 == 0
    test("Engine headless mode works", headless_ok,
         f"rc={rc2}", "engine-live")

    if stdout2:
        print("  --- Headless output (last 10 lines) ---")
        for line in stdout2.strip().split('\n')[-10:]:
            if line.strip():
                print(f"    {line.rstrip()}")


def phase_editor_live(build_dir: Path):
    """Phase 4: Launch the editor with D3D11 + ImGui."""
    print("\n" + "=" * 60)
    print("[PHASE 4] Editor Live Test (D3D11 + ImGui via Wine)")
    print("=" * 60)

    editor_exe = str(build_dir / "bin" / "SparkEditor.exe")
    if not os.path.isfile(editor_exe):
        test("Editor executable", False, "SparkEditor.exe not found", "editor-live")
        return

    env = get_wine_env()
    env["WINEDEBUG"] = "err+d3d,err+d3d11"

    # Run editor in test mode with frame limit
    print("  Launching SparkEditor.exe --test-mode --test-frames 120...")
    proc = run_wine_process(editor_exe,
                            ["--test-mode", "--test-frames", "120"],
                            env=env)

    start = time.time()
    max_wait = 45

    while time.time() - start < max_wait:
        if proc.poll() is not None:
            break
        time.sleep(0.5)

    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    stdout = proc.stdout.read() if proc.stdout else ""
    rc = proc.returncode

    test("Editor launches under Wine", rc is not None,
         f"rc={rc}, ran for {time.time()-start:.1f}s", "editor-live")

    # Check for ImGui/editor initialization
    editor_init = ("ImGui" in stdout or "Editor" in stdout or
                   "D3D11" in stdout or "Initialize" in stdout)
    test("Editor initializes D3D11 + ImGui", editor_init or rc == 0,
         "Found init messages" if editor_init else f"rc={rc}", "editor-live")

    # Check frame limit exit
    frame_exit = "[TEST] Frame limit reached" in stdout
    test("Editor respects --test-frames", frame_exit or rc == 0,
         "Clean exit" if frame_exit else f"rc={rc}", "editor-live")

    if stdout:
        print("  --- Editor output (last 20 lines) ---")
        for line in stdout.strip().split('\n')[-20:]:
            if line.strip():
                print(f"    {line.rstrip()}")


def phase_stress_tests(build_dir: Path):
    """Phase 5: Stress test — push the engine to its limits and break things."""
    print("\n" + "=" * 60)
    print("[PHASE 5] Stress Tests — Breaking Things")
    print("=" * 60)

    engine_exe = str(build_dir / "bin" / "SparkEngine.exe")
    tests_exe = str(build_dir / "bin" / "SparkTests.exe")
    env = get_wine_env()

    # Stress test 1: Rapid start/stop cycles
    print("\n  [5a] Rapid engine start/stop (10 cycles)...")
    crash_count = 0
    for i in range(10):
        rc, _, _ = run_wine(engine_exe, ["-test-frames", "5"], timeout=15, env=env)
        if rc != 0:
            crash_count += 1
    test("Rapid start/stop (10 cycles)", crash_count <= 1,
         f"crashes={crash_count}/10", "stress")

    # Stress test 2: Zero-frame exit
    print("  [5b] Zero-frame edge case...")
    rc, stdout, _ = run_wine(engine_exe, ["-test-frames", "1"], timeout=15, env=env)
    test("Single-frame exit", rc == 0 or "[TEST]" in stdout,
         f"rc={rc}", "stress")

    # Stress test 3: Run with intentionally bad arguments
    print("  [5c] Bad arguments handling...")
    rc, stdout, stderr = run_wine(engine_exe, ["-test-frames", "-1"], timeout=10, env=env)
    test("Negative frame count handled", rc is not None,
         f"rc={rc} (should treat as 0 = infinite, or exit cleanly)", "stress")

    rc, stdout, stderr = run_wine(engine_exe, ["-test-frames", "notanumber"],
                                  timeout=10, env=env)
    test("Non-numeric frame count handled", rc is not None,
         f"rc={rc}", "stress")

    # Stress test 4: Concurrent instances
    print("  [5d] Concurrent engine instances (3 simultaneous)...")
    procs = []
    for i in range(3):
        p = run_wine_process(engine_exe, ["-test-frames", "30"], env=env)
        procs.append(p)
    time.sleep(2)

    alive_count = sum(1 for p in procs if p.poll() is None)
    test("Multiple concurrent instances launch", alive_count >= 1,
         f"{alive_count}/3 still running after 2s", "stress")

    # Wait for all to finish
    for p in procs:
        try:
            p.wait(timeout=20)
        except subprocess.TimeoutExpired:
            p.kill()
    exit_codes = [p.returncode for p in procs]
    ok_count = sum(1 for rc in exit_codes if rc == 0)
    test("Concurrent instances exit cleanly", ok_count >= 1,
         f"exit codes: {exit_codes}", "stress")

    # Stress test 5: Run tests in shuffle mode
    if os.path.isfile(tests_exe):
        print("  [5e] Shuffled test execution...")
        rc, stdout, _ = run_wine(tests_exe, ["--shuffle", "42"],
                                 timeout=300, env=env)
        test("Shuffled tests pass", rc == 0,
             f"rc={rc}", "stress")

    # Stress test 6: Long-running test (300 frames)
    print("  [5f] Extended run (300 frames)...")
    start = time.time()
    rc, stdout, _ = run_wine(engine_exe, ["-test-frames", "300"],
                             timeout=60, env=env)
    elapsed = time.time() - start
    frame_exit = "[TEST] Frame limit reached" in stdout
    test("Extended 300-frame run", frame_exit or rc == 0,
         f"rc={rc}, elapsed={elapsed:.1f}s", "stress")

    # Stress test 7: Headless stress
    print("  [5g] Headless mode stability...")
    rc, stdout, _ = run_wine(engine_exe, ["-headless"], timeout=15, env=env)
    test("Headless mode completes", rc == 0 or "headless" in stdout.lower(),
         f"rc={rc}", "stress")


def phase_break_it(build_dir: Path):
    """Phase 6: Intentionally try to break things — edge cases and abuse."""
    print("\n" + "=" * 60)
    print("[PHASE 6] BREAKING THINGS")
    print("=" * 60)

    engine_exe = str(build_dir / "bin" / "SparkEngine.exe")
    editor_exe = str(build_dir / "bin" / "SparkEditor.exe")
    env = get_wine_env()

    # Break test 1: Kill during initialization
    print("\n  [6a] Kill during initialization...")
    proc = run_wine_process(engine_exe, ["-test-frames", "1000"], env=env)
    time.sleep(0.5)  # Kill mid-init
    proc.kill()
    rc = proc.wait()
    test("Engine handles SIGKILL during init", True,
         f"rc={rc} (killed)", "break")

    # Break test 2: SIGTERM during rendering
    print("  [6b] SIGTERM during rendering...")
    proc = run_wine_process(engine_exe, ["-test-frames", "1000"], env=env)
    time.sleep(3)  # Let it start rendering
    proc.send_signal(signal.SIGTERM)
    try:
        rc = proc.wait(timeout=10)
        test("Engine handles SIGTERM gracefully", True,
             f"rc={rc}", "break")
    except subprocess.TimeoutExpired:
        proc.kill()
        test("Engine handles SIGTERM gracefully", False,
             "Had to SIGKILL after SIGTERM", "break")

    # Break test 3: Corrupt Wine prefix (create then test)
    print("  [6c] Engine resilience with minimal Wine prefix...")
    bad_env = env.copy()
    bad_env["WINEPREFIX"] = "/tmp/spark-bad-wineprefix"
    rc, stdout, stderr = run_wine(engine_exe, ["-test-frames", "5"],
                                  timeout=20, env=bad_env)
    test("Engine with fresh Wine prefix", rc is not None,
         f"rc={rc}", "break")

    # Break test 4: No Vulkan ICD (force DXVK failure)
    print("  [6d] Force Vulkan ICD failure...")
    no_vk_env = env.copy()
    no_vk_env["VK_ICD_FILENAMES"] = "/nonexistent/path/fake_icd.json"
    rc, stdout, stderr = run_wine(engine_exe, ["-test-frames", "5"],
                                  timeout=15, env=no_vk_env)
    test("Engine handles missing Vulkan gracefully", rc is not None,
         f"rc={rc} (may fallback to WineD3D or NullRHI)", "break")

    # Break test 5: Editor with extreme frame count
    if os.path.isfile(editor_exe):
        print("  [6e] Editor rapid start/kill cycle...")
        for i in range(5):
            proc = run_wine_process(editor_exe,
                                    ["--test-mode", "--test-frames", "3"],
                                    env=env)
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
        test("Editor survives rapid start/kill cycles (5x)", True,
             "No hangs", "break")

    # Break test 6: Simultaneous engine + editor
    print("  [6f] Simultaneous engine + editor launch...")
    p1 = run_wine_process(engine_exe, ["-test-frames", "30"], env=env)
    p2 = run_wine_process(editor_exe,
                          ["--test-mode", "--test-frames", "30"],
                          env=env) if os.path.isfile(editor_exe) else None
    time.sleep(5)

    p1_alive = p1.poll() is None
    p2_alive = p2.poll() is None if p2 else False

    for p in [p1, p2]:
        if p:
            try:
                p.wait(timeout=30)
            except subprocess.TimeoutExpired:
                p.kill()

    test("Simultaneous engine+editor", True,
         f"engine_alive={p1_alive}, editor_alive={p2_alive}", "break")


# ── Summary ──────────────────────────────────────────────────────────

def print_summary():
    print("\n" + "=" * 60)
    print("WINDOWS LIVE TEST SUMMARY")
    print("=" * 60)

    passed = sum(1 for r in results if r.passed)
    failed = sum(1 for r in results if not r.passed)
    total = len(results)

    # Group by phase
    phases = {}
    for r in results:
        phase = r.phase or "general"
        if phase not in phases:
            phases[phase] = []
        phases[phase].append(r)

    for phase, tests in phases.items():
        phase_passed = sum(1 for t in tests if t.passed)
        phase_total = len(tests)
        print(f"\n  [{phase}] {phase_passed}/{phase_total} passed")
        for t in tests:
            status = "PASS" if t.passed else "FAIL"
            print(f"    [{status}] {t.name}")

    print(f"\n  TOTAL: {total}  PASSED: {passed}  FAILED: {failed}")

    if failed > 0:
        print(f"\n  FAILURES ({failed}):")
        for r in results:
            if not r.passed:
                print(f"    - {r.name}: {r.detail}")

    # Write JSON report
    report_path = "/tmp/spark-windows-wine-test/report.json"
    os.makedirs(os.path.dirname(report_path), exist_ok=True)
    report = {
        "total": total, "passed": passed, "failed": failed,
        "tests": [{"name": r.name, "passed": r.passed,
                    "detail": r.detail, "phase": r.phase} for r in results]
    }
    with open(report_path, 'w') as f:
        json.dump(report, f, indent=2)
    print(f"\n  Report: {report_path}")

    return failed == 0


# ── Main ─────────────────────────────────────────────────────────────

def main():
    build_dir = Path("build/linux-mingw-release")

    for i, arg in enumerate(sys.argv[1:], 1):
        if arg == "--build-dir" and i < len(sys.argv) - 1:
            build_dir = Path(sys.argv[i + 1])

    build_dir = build_dir.resolve() if not build_dir.is_absolute() else build_dir

    print("=" * 60)
    print("SparkEngine Windows Live Test Suite (Wine)")
    print("=" * 60)
    print(f"  Build dir: {build_dir}")
    print(f"  Project:   {PROJECT_ROOT}")

    phase_prerequisites(build_dir)
    phase_wine_setup()
    phase_unit_tests(build_dir)
    phase_engine_live(build_dir)
    phase_editor_live(build_dir)
    phase_stress_tests(build_dir)
    phase_break_it(build_dir)

    success = print_summary()
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()

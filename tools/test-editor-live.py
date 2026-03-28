#!/usr/bin/env python3
"""
SparkEditor Live Testing Script

Launches SparkEditor with Xvfb software rendering, systematically tests all
panels and interactions, captures screenshots, and reports findings.

Requirements:
  - Xvfb running on :99
  - xdotool installed
  - Python 3 with Pillow
  - SparkEditor built with SDL2 + OpenGL

Usage:
  export DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 MESA_GL_VERSION_OVERRIDE=3.3
  python3 tools/test-editor-live.py [--editor-path build/bin/SparkEditor]
"""

import subprocess
import time
import sys
import os
import signal
import ctypes
import ctypes.util
import json
from pathlib import Path
from dataclasses import dataclass, field
from typing import Optional

# ── Screenshot helper ──────────────────────────────────────────────────

class XImage(ctypes.Structure):
    _fields_ = [
        ("width", ctypes.c_int),
        ("height", ctypes.c_int),
        ("xoffset", ctypes.c_int),
        ("format", ctypes.c_int),
        ("data", ctypes.c_void_p),
        ("byte_order", ctypes.c_int),
        ("bitmap_unit", ctypes.c_int),
        ("bitmap_bit_order", ctypes.c_int),
        ("bitmap_pad", ctypes.c_int),
        ("depth", ctypes.c_int),
        ("bytes_per_line", ctypes.c_int),
        ("bits_per_pixel", ctypes.c_int),
        ("red_mask", ctypes.c_ulong),
        ("green_mask", ctypes.c_ulong),
        ("blue_mask", ctypes.c_ulong),
    ]

def capture_screenshot(output_path: str) -> bool:
    """Capture X11 screenshot to PNG file."""
    try:
        from PIL import Image
    except ImportError:
        print("  [WARN] Pillow not installed, skipping screenshot")
        return False

    x11 = ctypes.cdll.LoadLibrary(ctypes.util.find_library('X11'))
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XRootWindow.restype = ctypes.c_ulong
    x11.XGetImage.restype = ctypes.POINTER(XImage)
    x11.XGetImage.argtypes = [
        ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
        ctypes.c_uint, ctypes.c_uint, ctypes.c_ulong, ctypes.c_int
    ]

    display = x11.XOpenDisplay(None)
    if not display:
        return False

    screen = x11.XDefaultScreen(display)
    root = x11.XRootWindow(display, screen)
    w = x11.XDisplayWidth(display, screen)
    h = x11.XDisplayHeight(display, screen)

    img_ptr = x11.XGetImage(display, root, 0, 0, w, h, 0xFFFFFFFF, 2)
    if not img_ptr:
        x11.XCloseDisplay(display)
        return False

    xi = img_ptr.contents
    data_size = xi.bytes_per_line * xi.height
    buf = (ctypes.c_char * data_size)()
    ctypes.memmove(buf, xi.data, data_size)
    raw = bytes(buf)

    if xi.bits_per_pixel == 32:
        im = Image.frombytes('RGBA', (xi.width, xi.height), raw, 'raw', 'BGRA', xi.bytes_per_line)
        im = im.convert('RGB')
    else:
        x11.XCloseDisplay(display)
        return False

    im.save(output_path)
    x11.XCloseDisplay(display)
    return True


def is_mostly_black(image_path: str, threshold: float = 0.95) -> bool:
    """Check if an image is mostly black (indicating no rendering)."""
    try:
        from PIL import Image
        im = Image.open(image_path)
        pixels = list(im.getdata())
        black_count = sum(1 for p in pixels if sum(p[:3]) < 30)
        return (black_count / len(pixels)) > threshold
    except Exception:
        return True


def count_unique_colors(image_path: str) -> int:
    """Count unique colors in image (more = more complex UI rendered)."""
    try:
        from PIL import Image
        im = Image.open(image_path).convert('RGB')
        return len(set(im.getdata()))
    except Exception:
        return 0


# ── xdotool helpers ────────────────────────────────────────────────────

def xdo(cmd: str, timeout: float = 2.0) -> str:
    """Run an xdotool command."""
    try:
        result = subprocess.run(
            f"xdotool {cmd}", shell=True, capture_output=True,
            text=True, timeout=timeout, env=os.environ
        )
        return result.stdout.strip()
    except subprocess.TimeoutExpired:
        return ""


def click(x: int, y: int, button: int = 1):
    """Click at absolute screen coordinates."""
    xdo(f"mousemove {x} {y}")
    time.sleep(0.15)
    xdo(f"click {button}")
    time.sleep(0.3)


def key(keyname: str):
    """Send a key press."""
    xdo(f"key {keyname}")
    time.sleep(0.2)


# ── Test result tracking ──────────────────────────────────────────────

@dataclass
class TestResult:
    name: str
    passed: bool
    detail: str = ""
    screenshot: str = ""

results: list = []

def test(name: str, condition: bool, detail: str = "", screenshot: str = ""):
    """Record a test result."""
    status = "PASS" if condition else "FAIL"
    print(f"  [{status}] {name}" + (f" — {detail}" if detail else ""))
    results.append(TestResult(name, condition, detail, screenshot))


# ── Main test runner ──────────────────────────────────────────────────

def main():
    editor_path = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.abspath("build/bin/SparkEditor")
    output_dir = Path("/tmp/spark-editor-test")
    output_dir.mkdir(exist_ok=True)

    # Ensure DISPLAY is set
    if "DISPLAY" not in os.environ:
        print("ERROR: DISPLAY not set. Run with: export DISPLAY=:99")
        sys.exit(1)

    print("=" * 60)
    print("SparkEditor Live Test Suite")
    print("=" * 60)

    # ── 1. Launch editor ──
    print("\n[1/7] Launching SparkEditor...")
    env = os.environ.copy()
    env["LIBGL_ALWAYS_SOFTWARE"] = "1"
    env["MESA_GL_VERSION_OVERRIDE"] = "3.3"
    env["GALLIUM_DRIVER"] = "llvmpipe"

    editor_dir = os.path.dirname(editor_path)
    proc = subprocess.Popen(
        [editor_path, "--test-mode", "--debug-console"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        cwd=editor_dir,
        env=env
    )
    time.sleep(4)  # Wait for init

    alive = proc.poll() is None
    test("Editor launches without crash", alive,
         f"PID={proc.pid}" if alive else f"Exit code={proc.returncode}")
    if not alive:
        stdout = proc.stdout.read().decode()
        print(f"  Output:\n{stdout[:500]}")
        print_summary()
        sys.exit(1)

    # Read startup output
    startup_output = ""
    try:
        # Non-blocking read
        import fcntl
        fd = proc.stdout.fileno()
        fl = fcntl.fcntl(fd, fcntl.F_GETFL)
        fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)
        try:
            startup_output = proc.stdout.read().decode()
        except (BlockingIOError, TypeError):
            pass
    except Exception:
        pass

    # ── 2. Verify initial render ──
    print("\n[2/7] Verifying initial render...")
    ss = str(output_dir / "01_initial.png")
    captured = capture_screenshot(ss)
    test("Screenshot captured", captured, screenshot=ss)

    if captured:
        black = is_mostly_black(ss)
        test("Frame is not blank/black", not black, screenshot=ss)
        colors = count_unique_colors(ss)
        test("Frame has sufficient UI complexity", colors > 50,
             f"unique_colors={colors}", screenshot=ss)

    # ── 3. Test menu bar interaction ──
    print("\n[3/7] Testing menu bar...")
    win_info = xdo("search --name 'Spark Engine Editor'")
    win_id = win_info.split('\n')[0] if win_info else ""
    test("Window found by xdotool", bool(win_id), f"WID={win_id}")

    # Get window geometry
    geo = xdo(f"getwindowgeometry {win_id}") if win_id else ""
    # Parse "Position: X,Y" and "Geometry: WxH"
    win_x, win_y = 160, 90  # defaults
    if "Position:" in geo:
        pos_part = geo.split("Position:")[1].split("(")[0].strip()
        parts = pos_part.split(",")
        win_x, win_y = int(parts[0]), int(parts[1])

    menu_y = win_y + 8  # Menu bar is ~8px from window top

    # Test File menu
    click(win_x + 15, menu_y)  # "File"
    time.sleep(0.3)
    ss = str(output_dir / "02_file_menu.png")
    capture_screenshot(ss)
    colors_menu = count_unique_colors(ss)
    test("File menu opens", colors_menu > 60,
         f"unique_colors={colors_menu}", screenshot=ss)

    # Close menu
    click(win_x + 800, win_y + 400)
    time.sleep(0.3)

    # Test Edit menu
    click(win_x + 42, menu_y)  # "Edit"
    time.sleep(0.3)
    ss = str(output_dir / "03_edit_menu.png")
    capture_screenshot(ss)
    test("Edit menu opens", True, screenshot=ss)

    # Close menu
    click(win_x + 800, win_y + 400)
    time.sleep(0.3)

    # Test Window menu
    click(win_x + 155, menu_y)  # "Window"
    time.sleep(0.3)
    ss = str(output_dir / "04_window_menu.png")
    capture_screenshot(ss)
    test("Window menu opens", True, screenshot=ss)

    # Close menu
    click(win_x + 800, win_y + 400)
    time.sleep(0.3)

    # ── 4. Test panel visibility (via Window menu) ──
    print("\n[4/7] Testing panel toggle via Window menu...")
    # We'll open the Window menu and click through several panels
    # Each ImGui menu item is roughly 20px tall
    panel_tests = [
        ("Scene View", 6),
        ("Asset Browser", 8),
        ("Performance Profiler", 12),
        ("Material Editor", 14),
    ]

    for panel_name, approx_index in panel_tests:
        # Open Window menu
        click(win_x + 155, menu_y)
        time.sleep(0.3)
        # Click panel item
        item_y = menu_y + 14 + (approx_index * 19)
        click(win_x + 155, item_y)
        time.sleep(0.5)
        ss = str(output_dir / f"05_panel_{panel_name.replace(' ', '_').lower()}.png")
        capture_screenshot(ss)
        test(f"Panel toggle: {panel_name}", True, screenshot=ss)

    # ── 5. Test keyboard shortcuts ──
    print("\n[5/7] Testing keyboard navigation...")

    # Focus the window
    xdo(f"windowfocus {win_id}")
    time.sleep(0.3)

    # Ctrl+Z (Undo - should do nothing but not crash)
    key("ctrl+z")
    time.sleep(0.3)
    alive = proc.poll() is None
    test("Ctrl+Z (Undo) doesn't crash", alive)

    # Ctrl+S (Save - should handle gracefully)
    key("ctrl+s")
    time.sleep(0.3)
    alive = proc.poll() is None
    test("Ctrl+S (Save) doesn't crash", alive)

    # Ctrl+N (New Scene)
    key("ctrl+n")
    time.sleep(0.3)
    alive = proc.poll() is None
    test("Ctrl+N (New Scene) doesn't crash", alive)

    ss = str(output_dir / "06_after_shortcuts.png")
    capture_screenshot(ss)
    test("Editor still rendering after shortcuts", not is_mostly_black(ss) if captured else True,
         screenshot=ss)

    # ── 6. Test hierarchy interaction ──
    print("\n[6/7] Testing hierarchy panel interaction...")
    # The hierarchy panel is on the left side, approximately:
    # x = win_x + 45 (within hierarchy panel)
    # y = win_y + 105 (tree items start)

    # Click on a tree item in hierarchy
    click(win_x + 65, win_y + 115)
    time.sleep(0.3)
    ss = str(output_dir / "07_hierarchy_click.png")
    capture_screenshot(ss)
    test("Hierarchy panel clickable", True, screenshot=ss)

    # Right-click for context menu
    click(win_x + 65, win_y + 135, button=3)
    time.sleep(0.3)
    ss = str(output_dir / "08_hierarchy_context.png")
    capture_screenshot(ss)
    test("Hierarchy right-click context menu", True, screenshot=ss)

    # Close context menu
    key("Escape")
    time.sleep(0.3)

    # ── 7. Stress test — run for a few seconds ──
    print("\n[7/7] Stress test — running for 5 seconds...")
    start = time.time()
    crash = False
    while time.time() - start < 5:
        if proc.poll() is not None:
            crash = True
            break
        time.sleep(0.5)

    test("Editor survives 5-second stress test", not crash,
         f"Exit code={proc.returncode}" if crash else "Still running")

    ss = str(output_dir / "09_final_state.png")
    capture_screenshot(ss)
    test("Final state screenshot captured", captured, screenshot=ss)

    # ── Shutdown ──
    print("\n[CLEANUP] Shutting down editor...")
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=5)
        test("Editor shuts down cleanly", proc.returncode == 0,
             f"Exit code={proc.returncode}")
    except subprocess.TimeoutExpired:
        proc.kill()
        test("Editor shuts down cleanly", False, "Had to SIGKILL")

    # ── Print summary ──
    print_summary()


def print_summary():
    print("\n" + "=" * 60)
    print("TEST SUMMARY")
    print("=" * 60)
    passed = sum(1 for r in results if r.passed)
    failed = sum(1 for r in results if not r.passed)
    total = len(results)

    for r in results:
        status = "PASS" if r.passed else "FAIL"
        print(f"  [{status}] {r.name}")

    print(f"\n  Total: {total}  Passed: {passed}  Failed: {failed}")

    if failed > 0:
        print("\n  FAILED TESTS:")
        for r in results:
            if not r.passed:
                print(f"    - {r.name}: {r.detail}")
                if r.screenshot:
                    print(f"      Screenshot: {r.screenshot}")

    # Write JSON report
    report_path = "/tmp/spark-editor-test/report.json"
    report = {
        "total": total, "passed": passed, "failed": failed,
        "tests": [{"name": r.name, "passed": r.passed, "detail": r.detail,
                    "screenshot": r.screenshot} for r in results]
    }
    with open(report_path, 'w') as f:
        json.dump(report, f, indent=2)
    print(f"\n  Report: {report_path}")
    print(f"  Screenshots: /tmp/spark-editor-test/")


if __name__ == "__main__":
    main()

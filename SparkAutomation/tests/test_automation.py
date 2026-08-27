"""Black-box contract tests for SparkAutomation itself."""

import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path


AUTOMATION = Path(sys.argv.pop(1)).resolve()


def process_exists(pid):
    if os.name == "nt":
        completed = subprocess.run(
            ["tasklist", "/FI", f"PID eq {pid}", "/FO", "CSV", "/NH"],
            capture_output=True,
            text=True,
            check=False,
        )
        return f'"{pid}"' in completed.stdout
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


class AutomationContractTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_success_reports_and_xml_escaping(self):
        log = self.root / "runtime.log"
        report = self.root / "result.json"
        junit = self.root / "result.xml"
        screenshot = self.root / "frame.png"
        screenshot.write_bytes(b"not-empty")
        child_script = "from pathlib import Path; Path('frame.png').write_bytes(b'fresh'); print('ready')"
        name = 'A&B<"runtime">\nnext\tline'
        completed = subprocess.run(
            [
                str(AUTOMATION), "--name", name, "--executable", sys.executable,
                "--working-dir", str(self.root), "--frames", "2", "--timeout-ms", "5000",
                "--captured-log", str(log), "--log-contains", "ready", "--screenshot", str(screenshot),
                "--json", str(report), "--junit", str(junit), "--", "-c", child_script,
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        self.assertEqual(json.loads(report.read_text(encoding="utf-8"))["frameLimit"], 2)
        case = ET.parse(junit).getroot().find("testcase")
        self.assertIsNotNone(case)
        self.assertEqual(case.attrib["name"], name)
        self.assertEqual(screenshot.read_bytes(), b"fresh")

    def test_stale_screenshot_does_not_satisfy_expectation(self):
        log = self.root / "runtime.log"
        screenshot = self.root / "frame.png"
        screenshot.write_bytes(b"stale")
        completed = subprocess.run(
            [
                str(AUTOMATION), "--executable", sys.executable,
                "--working-dir", str(self.root), "--timeout-ms", "5000",
                "--captured-log", str(log), "--screenshot", str(screenshot),
                "--", "-c", "print('runtime completed')",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 1, completed.stdout + completed.stderr)
        self.assertIn("screenshot is missing or empty", completed.stdout)
        self.assertFalse(screenshot.exists())

    def test_failure_surfaces_captured_child_diagnostics(self):
        log = self.root / "failure.log"
        report = self.root / "failure.json"
        junit = self.root / "failure.xml"
        marker = "SPARK_AUTOMATION_CHILD_DIAGNOSTIC"
        child_script = f"import sys; print('{marker}', file=sys.stderr, flush=True); raise SystemExit(7)"
        completed = subprocess.run(
            [
                str(AUTOMATION), "--name", "diagnostic-failure",
                "--executable", sys.executable, "--working-dir", str(self.root),
                "--timeout-ms", "5000", "--expected-exit", "0",
                "--captured-log", str(log), "--json", str(report),
                "--junit", str(junit), "--", "-c", child_script,
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 1, completed.stdout + completed.stderr)
        self.assertIn(marker, completed.stderr)
        payload = json.loads(report.read_text(encoding="utf-8"))
        self.assertEqual(payload["exitCode"], 7)
        self.assertIn(marker, payload["capturedLogTail"])
        system_out = ET.parse(junit).getroot().find("testcase/system-out")
        self.assertIsNotNone(system_out)
        self.assertIn(marker, system_out.text or "")

    def test_timeout_kills_descendant_process(self):
        log = self.root / "timeout.log"
        child_script = (
            "import subprocess,sys,time; "
            "p=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)']); "
            "print(p.pid,flush=True); time.sleep(60)"
        )
        completed = subprocess.run(
            [
                str(AUTOMATION), "--executable", sys.executable, "--working-dir", str(self.root),
                "--timeout-ms", "750", "--captured-log", str(log), "--", "-c", child_script,
            ],
            capture_output=True,
            text=True,
            check=False,
            timeout=10,
        )
        self.assertEqual(completed.returncode, 1, completed.stdout + completed.stderr)
        self.assertTrue(json.loads(completed.stdout)["timedOut"])
        child_pid = int(log.read_text(encoding="utf-8").strip().splitlines()[0])
        for _ in range(20):
            if not process_exists(child_pid):
                break
            time.sleep(0.05)
        self.assertFalse(process_exists(child_pid), f"descendant process {child_pid} survived timeout")

    @unittest.skipIf(os.name == "nt", "POSIX process-group escape regression")
    def test_timeout_kills_descendant_that_creates_new_session(self):
        log = self.root / "escaped-timeout.log"
        child_script = (
            "import subprocess,sys,time; "
            "p=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)'],start_new_session=True); "
            "print(p.pid,flush=True); time.sleep(60)"
        )
        completed = subprocess.run(
            [
                str(AUTOMATION), "--executable", sys.executable, "--working-dir", str(self.root),
                "--timeout-ms", "750", "--captured-log", str(log), "--", "-c", child_script,
            ],
            capture_output=True,
            text=True,
            check=False,
            timeout=10,
        )
        self.assertEqual(completed.returncode, 1, completed.stdout + completed.stderr)
        self.assertTrue(json.loads(completed.stdout)["timedOut"])
        escaped_pid = int(log.read_text(encoding="utf-8").strip().splitlines()[0])
        for _ in range(40):
            if not process_exists(escaped_pid):
                break
            time.sleep(0.05)
        self.assertFalse(process_exists(escaped_pid), f"escaped descendant {escaped_pid} survived timeout")

    def test_numeric_options_reject_signs_junk_and_overflow(self):
        cases = (
            ("--frames", "-1"),
            ("--frames", "+1"),
            ("--frames", "1junk"),
            ("--frames", "4294967296"),
            ("--timeout-ms", "-1"),
            ("--timeout-ms", "+1"),
            ("--timeout-ms", "10junk"),
            ("--timeout-ms", "4294967296"),
            ("--expected-exit", "+1"),
            ("--expected-exit", "1junk"),
            ("--expected-exit", "2147483648"),
            ("--expected-exit", "-2147483649"),
        )
        for option, value in cases:
            with self.subTest(option=option, value=value):
                completed = subprocess.run(
                    [str(AUTOMATION), "--executable", sys.executable, option, value],
                    capture_output=True,
                    text=True,
                    check=False,
                    timeout=5,
                )
                self.assertEqual(completed.returncode, 2, completed.stdout + completed.stderr)
                self.assertIn(f"Invalid argument: {option} {value}", completed.stderr)


if __name__ == "__main__":
    unittest.main()

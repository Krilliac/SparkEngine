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


if __name__ == "__main__":
    unittest.main()

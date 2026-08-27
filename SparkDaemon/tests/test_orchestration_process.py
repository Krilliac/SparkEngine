#!/usr/bin/env python3
"""Black-box SparkDaemon/SparkOrchestrator process-supervision smoke test."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import uuid


def open_process_log(path: Path):
    """Open an inherited process log without introducing pipe backpressure."""
    return path.open("ab", buffering=0)


def read_process_log(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def record_supervised_pids(output: str, known_pids: set[int]) -> None:
    """Remember supervised descendants so abnormal Windows cleanup is bounded."""
    for line in output.splitlines():
        for field in line.split("\t"):
            if not field.startswith("pid="):
                continue
            try:
                pid = int(field.removeprefix("pid="))
            except ValueError:
                continue
            if pid > 0:
                known_pids.add(pid)


def force_terminate_process_tree(
    daemon_process: subprocess.Popen[str], supervised_pids: set[int]
) -> None:
    """Contain a failed smoke test to the process tree it created."""
    if os.name == "nt":
        targets = dict.fromkeys([daemon_process.pid, *sorted(supervised_pids)])
        for pid in targets:
            try:
                subprocess.run(
                    ["taskkill", "/PID", str(pid), "/T", "/F"],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=5,
                    check=False,
                )
            except (OSError, subprocess.SubprocessError):
                pass
        if daemon_process.poll() is None:
            daemon_process.kill()
            daemon_process.wait(timeout=3)
        return

    process_group = daemon_process.pid
    try:
        os.killpg(process_group, signal.SIGTERM)
    except ProcessLookupError:
        return

    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        try:
            os.killpg(process_group, 0)
        except ProcessLookupError:
            return
        time.sleep(0.05)

    try:
        os.killpg(process_group, signal.SIGKILL)
    except ProcessLookupError:
        pass
    if daemon_process.poll() is None:
        daemon_process.wait(timeout=3)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--daemon", required=True, type=Path)
    parser.add_argument("--orchestrator", required=True, type=Path)
    parser.add_argument("--child", required=True, type=Path)
    return parser.parse_args()


def endpoint(prefix: str, token: str) -> str:
    if os.name == "nt":
        return f"{prefix}-{token}"
    # Unix-domain socket paths are commonly limited to roughly 100 bytes.
    return str(Path(tempfile.gettempdir()) / f"{prefix}-{token}.sock")


def read_exact(stream: object, size: int) -> bytes:
    chunks: list[bytes] = []
    remaining = size
    while remaining:
        if isinstance(stream, socket.socket):
            chunk = stream.recv(remaining)
        else:
            chunk = stream.read(remaining)  # type: ignore[attr-defined]
        if not chunk:
            raise EOFError("endpoint closed before completing a frame")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def control_ping(endpoint_name: str) -> bool:
    """Connect to the child and complete a real Control/Ping frame round-trip."""
    request = struct.pack("<IHH", 0, 0, 1)
    try:
        if os.name == "nt":
            import ctypes

            pipe_prefix = "\\\\.\\pipe\\"
            pipe_name = endpoint_name
            if not pipe_name.startswith(pipe_prefix):
                pipe_name = pipe_prefix + pipe_name
            if not ctypes.windll.kernel32.WaitNamedPipeW(pipe_name, 50):
                return False
            with open(pipe_name, "r+b", buffering=0) as connection:
                connection.write(request)
                response_header = read_exact(connection, 8)
                payload_size, service_id, message_type = struct.unpack("<IHH", response_header)
                if payload_size > 1024:
                    return False
                payload = read_exact(connection, payload_size)
        else:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                connection.settimeout(0.25)
                connection.connect(endpoint_name)
                connection.sendall(request)
                response_header = read_exact(connection, 8)
                payload_size, service_id, message_type = struct.unpack("<IHH", response_header)
                if payload_size > 1024:
                    return False
                payload = read_exact(connection, payload_size)
        return service_id == 0 and message_type == 2 and payload == b"pong"
    except (EOFError, OSError, TimeoutError):
        return False


def main() -> int:
    args = parse_args()
    daemon = args.daemon.resolve(strict=True)
    orchestrator = args.orchestrator.resolve(strict=True)
    child = args.child.resolve(strict=True)
    binary_dir = daemon.parent
    token = uuid.uuid4().hex[:12]
    daemon_endpoint = endpoint("spark-orch-smoke", token)
    child_endpoint = endpoint("spark-collab-smoke", token)

    with tempfile.TemporaryDirectory(prefix="spark-orchestration-smoke-") as temporary:
        scratch = Path(temporary)
        daemon_log_path = scratch / "daemon.log"
        environment = os.environ.copy()
        environment["SPARK_ORCHESTRATOR_IDENTITY"] = str(scratch / "identity.state")
        cli_creation_flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        daemon_creation_flags = 0
        daemon_startup_info = None
        known_supervised_pids: set[int] = set()
        if os.name == "nt":
            # The supervisor sends CTRL_BREAK to graceful-stop Windows process
            # groups. Give it a real, hidden console even when CTest itself is
            # running as a service without one.
            daemon_creation_flags = getattr(subprocess, "CREATE_NEW_CONSOLE", 0)
            daemon_startup_info = subprocess.STARTUPINFO()
            daemon_startup_info.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            daemon_startup_info.wShowWindow = subprocess.SW_HIDE
        # SparkDaemon's supervised children inherit its standard output.  An
        # unread subprocess.PIPE can therefore fill and stall the entire
        # process tree before the harness reaches shutdown.  An append-only
        # file preserves diagnostics while allowing writers to make progress
        # independently of when the parent reads them.
        with open_process_log(daemon_log_path) as daemon_log:
            daemon_process = subprocess.Popen(
                [
                    str(daemon),
                    "--socket",
                    daemon_endpoint,
                    "--orchestrator-allow-root",
                    str(binary_dir),
                    "--orchestrator-max-processes",
                    "2",
                    "--orchestrator-state-file",
                    str(scratch / "orchestration.state"),
                ],
                cwd=binary_dir,
                env=environment,
                stdout=daemon_log,
                stderr=subprocess.STDOUT,
                text=True,
                creationflags=daemon_creation_flags,
                startupinfo=daemon_startup_info,
                start_new_session=os.name != "nt",
            )

        def invoke(
            *command: str, check: bool = True, timeout: float = 3
        ) -> subprocess.CompletedProcess[str]:
            completed = subprocess.run(
                [str(orchestrator), "--socket", daemon_endpoint, *command],
                cwd=binary_dir,
                env=environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=timeout,
                creationflags=cli_creation_flags,
                check=False,
            )
            record_supervised_pids(completed.stdout, known_supervised_pids)
            if check and completed.returncode != 0:
                raise RuntimeError(
                    f"SparkOrchestrator {' '.join(command)} failed with "
                    f"{completed.returncode}:\n{completed.stdout}"
                )
            return completed

        completed_normally = False
        try:
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                if daemon_process.poll() is not None:
                    output = read_process_log(daemon_log_path)
                    raise RuntimeError(f"SparkDaemon exited before readiness:\n{output}")
                if invoke("list", check=False).returncode == 0:
                    break
                time.sleep(0.1)
            else:
                raise RuntimeError("SparkDaemon did not become ready within 8 seconds")

            service_id = "live-collab"
            invoke("define", service_id, str(child), str(binary_dir), "--socket", child_endpoint)
            invoke("start", service_id)

            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                status = invoke("status", service_id)
                if f"{service_id}\trunning\tpid=" in status.stdout and control_ping(child_endpoint):
                    break
                time.sleep(0.1)
            else:
                raise RuntimeError(f"supervised child did not become IPC-ready:\n{status.stdout}")

            invoke("drain", service_id)
            deadline = time.monotonic() + 4
            while time.monotonic() < deadline:
                status = invoke("status", service_id)
                if f"{service_id}\tstopped\tpid=0" in status.stdout:
                    break
                time.sleep(0.1)
            else:
                raise RuntimeError(
                    "drain did not stop the child before the five-second forced-termination deadline:\n"
                    f"{status.stdout}"
                )

            invoke("start", service_id)
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                status = invoke("status", service_id)
                if f"{service_id}\trunning\tpid=" in status.stdout and control_ping(child_endpoint):
                    break
                time.sleep(0.1)
            else:
                raise RuntimeError(f"restarted child did not become IPC-ready:\n{status.stdout}")

            invoke("stop", service_id)
            deadline = time.monotonic() + 4
            while time.monotonic() < deadline:
                status = invoke("status", service_id)
                if f"{service_id}\tstopped\tpid=0" in status.stdout:
                    break
                time.sleep(0.1)
            else:
                raise RuntimeError(
                    "stop did not finish before the five-second forced-termination deadline:\n"
                    f"{status.stdout}"
                )

            invoke("undefine", service_id)
            invoke("daemon-shutdown")
            daemon_exit = daemon_process.wait(timeout=8)
            daemon_output = read_process_log(daemon_log_path)
            if daemon_exit != 0:
                raise RuntimeError(f"SparkDaemon exited with {daemon_exit}:\n{daemon_output}")
            if daemon_output.count("SparkCollabServer: shutdown complete") < 2:
                raise RuntimeError(
                    "supervised child did not report graceful shutdown after drain and stop:\n"
                    f"{daemon_output}"
                )
            completed_normally = True
            print("Spark orchestration process smoke passed")
            return 0
        finally:
            try:
                if not completed_normally:
                    parent_exited_before_cleanup = daemon_process.poll() is not None
                    fallback_shutdown_completed = False
                    if not parent_exited_before_cleanup:
                        try:
                            invoke("daemon-shutdown", check=False, timeout=1)
                        except (OSError, subprocess.SubprocessError):
                            pass
                        try:
                            daemon_process.wait(timeout=3)
                            fallback_shutdown_completed = True
                        except subprocess.TimeoutExpired:
                            pass
                    if parent_exited_before_cleanup or not fallback_shutdown_completed:
                        force_terminate_process_tree(daemon_process, known_supervised_pids)
            finally:
                for socket_path in (daemon_endpoint, child_endpoint):
                    if os.name != "nt":
                        Path(socket_path).unlink(missing_ok=True)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:  # noqa: BLE001 - process-test diagnostics are intentional.
        print(f"orchestration process smoke failed: {error}", file=sys.stderr)
        raise SystemExit(1)

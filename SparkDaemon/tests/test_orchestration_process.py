#!/usr/bin/env python3
"""Black-box SparkDaemon/SparkOrchestrator process-supervision smoke test."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import uuid


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
        environment = os.environ.copy()
        environment["SPARK_ORCHESTRATOR_IDENTITY"] = str(scratch / "identity.state")
        cli_creation_flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        daemon_creation_flags = 0
        daemon_startup_info = None
        if os.name == "nt":
            # The supervisor sends CTRL_BREAK to graceful-stop Windows process
            # groups. Give it a real, hidden console even when CTest itself is
            # running as a service without one.
            daemon_creation_flags = getattr(subprocess, "CREATE_NEW_CONSOLE", 0)
            daemon_startup_info = subprocess.STARTUPINFO()
            daemon_startup_info.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            daemon_startup_info.wShowWindow = subprocess.SW_HIDE
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
            stdout=subprocess.PIPE,
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
            if check and completed.returncode != 0:
                raise RuntimeError(
                    f"SparkOrchestrator {' '.join(command)} failed with "
                    f"{completed.returncode}:\n{completed.stdout}"
                )
            return completed

        try:
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                if daemon_process.poll() is not None:
                    output = daemon_process.stdout.read() if daemon_process.stdout else ""
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
            daemon_output = daemon_process.stdout.read() if daemon_process.stdout else ""
            if daemon_exit != 0:
                raise RuntimeError(f"SparkDaemon exited with {daemon_exit}:\n{daemon_output}")
            if daemon_output.count("SparkCollabServer: shutdown complete") < 2:
                raise RuntimeError(
                    "supervised child did not report graceful shutdown after drain and stop:\n"
                    f"{daemon_output}"
                )
            print("Spark orchestration process smoke passed")
            return 0
        finally:
            try:
                if daemon_process.poll() is None:
                    try:
                        invoke("daemon-shutdown", check=False, timeout=1)
                    except (OSError, subprocess.SubprocessError):
                        pass
                    try:
                        daemon_process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        daemon_process.terminate()
                        try:
                            daemon_process.wait(timeout=3)
                        except subprocess.TimeoutExpired:
                            daemon_process.kill()
                            daemon_process.wait(timeout=3)
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

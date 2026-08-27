#!/usr/bin/env python3
"""Black-box SparkDaemon/SparkOrchestrator process-supervision smoke test."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import json
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
    """Remember supervised descendants so abnormal cleanup stays bounded."""
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

    # SparkDaemon creates every supervised POSIX child as the leader of its
    # own process group.  They therefore do not share the daemon's session
    # group and must be contained explicitly if the daemon dies before its
    # normal StopAll() path can run.
    targets = tuple(dict.fromkeys([daemon_process.pid, *sorted(supervised_pids)]))

    def signal_target(pid: int, signal_number: int) -> None:
        try:
            os.killpg(pid, signal_number)
            return
        except ProcessLookupError:
            pass
        try:
            os.kill(pid, signal_number)
        except ProcessLookupError:
            pass

    def target_exists(pid: int) -> bool:
        try:
            os.killpg(pid, 0)
            return True
        except ProcessLookupError:
            pass
        try:
            os.kill(pid, 0)
            return True
        except ProcessLookupError:
            return False

    for pid in targets:
        signal_target(pid, signal.SIGTERM)

    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        if not any(target_exists(pid) for pid in targets):
            break
        time.sleep(0.05)

    for pid in targets:
        if target_exists(pid):
            signal_target(pid, signal.SIGKILL)
    if daemon_process.poll() is None:
        daemon_process.wait(timeout=3)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--daemon", required=True, type=Path)
    parser.add_argument("--orchestrator", required=True, type=Path)
    parser.add_argument("--child", required=True, type=Path)
    # Optional: when SparkServer and a game module are available, the smoke also
    # proves that an engine host honours the supervisor's graceful-stop signal.
    parser.add_argument("--server", type=Path)
    parser.add_argument("--game-module", type=Path)
    parser.add_argument("--working-dir", type=Path)
    return parser.parse_args()


@contextmanager
def reserve_udp_port():
    """Hold an ephemeral UDP port until immediately before server startup."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
        reservation.bind(("127.0.0.1", 0))
        yield int(reservation.getsockname()[1])


def read_health(path: Path) -> dict | None:
    """Read one atomically published health snapshot, tolerating a rename race."""
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


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


def run_supervised_engine_host(invoke, server: Path, game_module: Path, working_dir: Path, scratch: Path) -> None:
    """Prove a supervised engine host stops gracefully instead of being killed.

    SparkDaemon graceful-stops a supervised process with CTRL_BREAK_EVENT on
    Windows and SIGTERM on POSIX. Only a host that installs the matching handler
    reaches ServerApplication::Stop(), and only Stop() publishes a final
    live=false snapshot. A host that is merely terminated leaves its last
    periodic live=true snapshot behind, so this assertion fails closed rather
    than silently accepting a hard kill as a graceful stop.
    """
    service_id = "live-server"
    health_path = scratch / "supervised-server-health.json"
    # Keep the selected port bound while the definition is transmitted.  The
    # reservation must be released for SparkServer itself to bind, but doing
    # so only after define narrows the unavoidable bind/start handoff race.
    with reserve_udp_port() as server_port:
        invoke(
            "define",
            service_id,
            str(server),
            str(working_dir),
            "--module",
            str(game_module),
            "--port",
            str(server_port),
            "--no-lan-broadcast",
            "--health-file",
            str(health_path),
            "--status-interval-ms",
            "200",
        )
    invoke("start", service_id)

    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        health = read_health(health_path)
        if health is not None and health.get("live") is True and health.get("ready") is True:
            break
        time.sleep(0.1)
    else:
        raise RuntimeError(
            "supervised SparkServer never published ready health; "
            f"last snapshot={read_health(health_path)!r}"
        )

    invoke("stop", service_id)
    status = None
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        status = invoke("status", service_id)
        if f"{service_id}\tstopped\tpid=0" in status.stdout:
            break
        time.sleep(0.1)
    else:
        raise RuntimeError(
            "supervisor could not stop SparkServer before its forced-termination deadline:\n"
            f"{status.stdout if status else '<no status>'}"
        )

    final = read_health(health_path)
    if final is None:
        raise RuntimeError("supervised SparkServer left no readable final health snapshot")
    if final.get("live") is not False or final.get("ready") is not False:
        raise RuntimeError(
            "supervised SparkServer was terminated instead of honouring the graceful-stop "
            f"signal; final health={final!r}"
        )
    invoke("undefine", service_id)


ERROR_ACCESS_DENIED = 5


def attach_console() -> None:
    """Console control events need a console; CTest may be started without one.

    GetConsoleWindow() is not a usable probe here because a ConPTY pseudoconsole
    owns no window and reports NULL, so ask for a console and read the refusal:
    ERROR_ACCESS_DENIED means this process already has one.
    """
    import ctypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    if kernel32.AllocConsole():
        return
    error = ctypes.get_last_error()
    if error == ERROR_ACCESS_DENIED:
        return
    raise RuntimeError(
        "could not obtain a console, so no console control event can be "
        f"delivered (AllocConsole failed with {error})"
    )


def interrupt(process: subprocess.Popen[str]) -> None:
    """Deliver the operator's 'stop this service' signal to a daemon process."""
    if os.name == "nt":
        process.send_signal(signal.CTRL_BREAK_EVENT)
    else:
        os.killpg(process.pid, signal.SIGINT)


def run_console_interrupt_scenario(
    daemon: Path, orchestrator: Path, child: Path, binary_dir: Path, token: str
) -> None:
    """A console interrupt must drain supervised children, not abandon them.

    SparkDaemon owns its supervised processes through job objects that carry
    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, so an abrupt daemon exit takes the
    children down with it -- but by termination, never by asking them to stop.
    Only ~OrchestrationService() -> StopAll() sends each child the graceful
    stop it is written to honour, and that destructor runs only when the
    process leaves main() normally.  On Windows the OS default console handler
    ends the daemon with STATUS_CONTROL_C_EXIT (0xC000013A) instead, which is
    why the daemon installs its own handler.
    """
    if os.name == "nt":
        attach_console()

    daemon_endpoint = endpoint("spark-orch-interrupt", token)
    child_endpoint = endpoint("spark-collab-interrupt", token)

    with tempfile.TemporaryDirectory(prefix="spark-orchestration-interrupt-") as temporary:
        scratch = Path(temporary)
        daemon_log_path = scratch / "daemon.log"
        environment = os.environ.copy()
        environment["SPARK_ORCHESTRATOR_IDENTITY"] = str(scratch / "identity.state")
        known_supervised_pids: set[int] = set()

        # The daemon must share this harness's console and own its process
        # group, so the control event reaches the daemon and nothing else.
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
                creationflags=(
                    subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
                ),
                start_new_session=os.name != "nt",
            )

        def invoke(
            *command: str, check: bool = True, timeout: float = 5
        ) -> subprocess.CompletedProcess[str]:
            completed = subprocess.run(
                [str(orchestrator), "--socket", daemon_endpoint, *command],
                cwd=binary_dir,
                env=environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=timeout,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
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
                    raise RuntimeError(
                        "SparkDaemon exited before readiness:\n"
                        f"{read_process_log(daemon_log_path)}"
                    )
                if invoke("list", check=False).returncode == 0:
                    break
                time.sleep(0.1)
            else:
                raise RuntimeError("SparkDaemon did not become ready within 8 seconds")

            service_id = "interrupt-collab"
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

            interrupt(daemon_process)

            try:
                daemon_exit = daemon_process.wait(timeout=20)
            except subprocess.TimeoutExpired as expired:
                raise RuntimeError(
                    "SparkDaemon ignored the console interrupt and was still running "
                    "after 20 seconds"
                ) from expired

            daemon_output = read_process_log(daemon_log_path)
            if daemon_exit != 0:
                raise RuntimeError(
                    f"console interrupt ended SparkDaemon with {daemon_exit} "
                    f"(0x{daemon_exit & 0xFFFFFFFF:08X}) instead of a clean exit; "
                    "0xC000013A means the OS default handler killed it and no "
                    f"graceful shutdown ran:\n{daemon_output}"
                )
            if "SparkDaemon: shutdown complete" not in daemon_output:
                raise RuntimeError(
                    "SparkDaemon exited 0 without reaching its shutdown message, so "
                    f"the graceful path did not run:\n{daemon_output}"
                )
            if "SparkCollabServer: shutdown complete" not in daemon_output:
                raise RuntimeError(
                    "the supervised child was not drained before the daemon left; it "
                    "was terminated with the job object rather than asked to stop:\n"
                    f"{daemon_output}"
                )
            completed_normally = True
        finally:
            try:
                if not completed_normally and daemon_process.poll() is None:
                    force_terminate_process_tree(daemon_process, known_supervised_pids)
            finally:
                if os.name != "nt":
                    for socket_path in (daemon_endpoint, child_endpoint):
                        Path(socket_path).unlink(missing_ok=True)


def main() -> int:
    args = parse_args()
    daemon = args.daemon.resolve(strict=True)
    orchestrator = args.orchestrator.resolve(strict=True)
    child = args.child.resolve(strict=True)
    binary_dir = daemon.parent
    server = args.server.resolve(strict=True) if args.server else None
    game_module = args.game_module.resolve(strict=True) if args.game_module else None
    host_working_dir = args.working_dir.resolve(strict=True) if args.working_dir else binary_dir
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
        # Both the executable and the working directory of every supervised
        # definition must resolve beneath an allow root.
        allow_roots = ["--orchestrator-allow-root", str(binary_dir)]
        if host_working_dir != binary_dir:
            allow_roots += ["--orchestrator-allow-root", str(host_working_dir)]
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
                    *allow_roots,
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

            if server is not None and game_module is not None:
                run_supervised_engine_host(
                    invoke,
                    server=server,
                    game_module=game_module,
                    working_dir=host_working_dir,
                    scratch=scratch,
                )

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

    run_console_interrupt_scenario(daemon, orchestrator, child, binary_dir, token)
    print("Spark orchestration process smoke passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:  # noqa: BLE001 - process-test diagnostics are intentional.
        print(f"orchestration process smoke failed: {error}", file=sys.stderr)
        raise SystemExit(1)

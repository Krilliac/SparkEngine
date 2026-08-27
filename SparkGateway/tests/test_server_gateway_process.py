#!/usr/bin/env python3
"""Black-box lifecycle test for two SparkServer areas and one SparkGateway."""

from __future__ import annotations

import argparse
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path


MAGIC = 0x5350524B  # "SPRK"
CONNECT = 1
CONNECT_ACCEPTED = 2


class ManagedProcess:
    def __init__(self, name: str, command: list[str], working_dir: Path, log_path: Path):
        self.name = name
        self.log_path = log_path
        self._log = log_path.open("w", encoding="utf-8", errors="replace")
        creation_flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        try:
            self.process = subprocess.Popen(
                command,
                cwd=working_dir,
                stdin=subprocess.DEVNULL,
                stdout=self._log,
                stderr=subprocess.STDOUT,
                creationflags=creation_flags,
                shell=False,
            )
        except Exception:
            self._log.close()
            raise

    def poll(self) -> int | None:
        return self.process.poll()

    def wait(self, timeout: float) -> int:
        return self.process.wait(timeout=timeout)

    def diagnostics(self) -> str:
        self._log.flush()
        try:
            return self.log_path.read_text(encoding="utf-8", errors="replace")[-12000:]
        except OSError as error:
            return f"<could not read {self.log_path}: {error}>"

    def cleanup(self) -> None:
        if self.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=3)
        self._log.close()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def existing_file(value: str) -> Path:
    path = Path(value).resolve()
    if not path.is_file():
        raise argparse.ArgumentTypeError(f"not a file: {path}")
    return path


def reserve_udp_ports(count: int) -> list[int]:
    reservations: list[socket.socket] = []
    ports: list[int] = []
    try:
        for _ in range(count):
            reservation = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            reservation.bind(("127.0.0.1", 0))
            reservations.append(reservation)
            ports.append(int(reservation.getsockname()[1]))
        require(len(set(ports)) == count, "operating system returned duplicate UDP ports")
        return ports
    finally:
        for reservation in reservations:
            reservation.close()


def make_private_key(path: Path) -> None:
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "wb") as output:
        output.write(b"SparkGatewayTopologyKey-32bytes!!")
    if os.name != "nt":
        os.chmod(path, 0o600)
        return

    identity = subprocess.run(
        ["whoami"], capture_output=True, text=True, check=True, timeout=5, shell=False
    ).stdout.strip()
    require(bool(identity), "whoami returned an empty Windows identity")
    secured = subprocess.run(
        ["icacls", str(path), "/inheritance:r", "/grant:r", f"{identity}:(F)"],
        capture_output=True,
        text=True,
        check=False,
        timeout=10,
        shell=False,
    )
    require(
        secured.returncode == 0,
        f"could not make gateway key owner-only: {secured.stdout}{secured.stderr}",
    )
    owned = subprocess.run(
        ["icacls", str(path), "/setowner", identity],
        capture_output=True,
        text=True,
        check=False,
        timeout=10,
        shell=False,
    )
    require(
        owned.returncode == 0,
        f"could not assign gateway key ownership to its only ACL trustee: {owned.stdout}{owned.stderr}",
    )


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def wait_health(
    process: ManagedProcess,
    health_path: Path,
    predicate,
    description: str,
    timeout: float = 10.0,
) -> dict:
    deadline = time.monotonic() + timeout
    last_value: object = "health file did not exist"
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(
                f"{process.name} exited with {process.poll()} while waiting for {description}\n"
                + process.diagnostics()
            )
        if health_path.is_file():
            try:
                last_value = read_json(health_path)
                if predicate(last_value):
                    return last_value
            except (OSError, json.JSONDecodeError) as error:
                last_value = error
        time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for {process.name} {description}; last health={last_value!r}")


def connect_packet(player_name: str) -> bytes:
    name = player_name.encode("utf-8")
    payload = struct.pack("<H", len(name)) + name
    header = struct.pack("<IHBIIfI", MAGIC, CONNECT, 1, 0, 0, 0.0, len(payload))
    return header + payload


def require_udp_admission(port: int, player_name: str) -> None:
    packet = connect_packet(player_name)
    client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        client.bind(("127.0.0.1", 0))
        client.settimeout(0.4)
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            client.sendto(packet, ("127.0.0.1", port))
            attempt_deadline = min(deadline, time.monotonic() + 0.6)
            while time.monotonic() < attempt_deadline:
                try:
                    response, _ = client.recvfrom(4096)
                except socket.timeout:
                    break
                if len(response) >= 6 and struct.unpack_from("<I", response, 0)[0] == MAGIC:
                    if struct.unpack_from("<H", response, 4)[0] == CONNECT_ACCEPTED:
                        return
        raise RuntimeError(f"SparkServer on UDP {port} did not return ConnectAccepted")
    finally:
        client.close()


def parse_probe_json(stdout: str) -> dict:
    for line in reversed(stdout.splitlines()):
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict) and "accepted" in value:
            return value
    raise RuntimeError(f"topology probe emitted no route JSON: {stdout!r}")


def run_gateway_probe(
    executable: Path,
    working_dir: Path,
    endpoint: str,
    key_file: Path,
    client_id: int,
    nonce: int,
) -> dict:
    completed = subprocess.run(
        [
            str(executable),
            "--endpoint",
            endpoint,
            "--key-file",
            str(key_file),
            "--client-id",
            str(client_id),
            "--nonce",
            str(nonce),
            "--session",
            f"topology-session-{client_id}",
            "--player",
            f"Topology Player {client_id}",
        ],
        cwd=working_dir,
        capture_output=True,
        text=True,
        check=False,
        timeout=5,
        shell=False,
    )
    require(
        completed.returncode == 0,
        f"gateway probe exited {completed.returncode}: {completed.stdout}{completed.stderr}",
    )
    return parse_probe_json(completed.stdout)


def write_gateway_config(
    path: Path,
    key_file: Path,
    ingress_endpoint: str,
    gateway_port: int,
    server_ports: list[int],
    control_ports: list[int],
) -> None:
    inter_server_port = gateway_port + 1 if gateway_port < 65535 else gateway_port - 1
    path.write_text(
        "\n".join(
            [
                "[Gateway]",
                "world_name = TopologyWorld",
                f"port = {gateway_port}",
                f"inter_server_port = {inter_server_port}",
                "max_total_clients = 32",
                "tick_rate = 20",
                "load_balancing = false",
                f"ingress_endpoint = {ingress_endpoint}",
                "",
                "[Area.Alpha]",
                "host = 127.0.0.1",
                f"port = {server_ports[0]}",
                f"inter_server_port = {control_ports[0]}",
                "max_clients = 8",
                "tick_rate = 60",
                "scene = topology-alpha",
                "",
                "[Area.Beta]",
                "host = 127.0.0.1",
                f"port = {server_ports[1]}",
                f"inter_server_port = {control_ports[1]}",
                "max_clients = 8",
                "tick_rate = 60",
                "scene = topology-beta",
                "",
                "[Security]",
                f"key_file = {key_file.as_posix()}",
                "",
            ]
        ),
        encoding="utf-8",
    )


def request_graceful_stop(process: ManagedProcess, sentinel: Path, health_path: Path) -> None:
    sentinel.write_text("stop\n", encoding="utf-8")
    try:
        exit_code = process.wait(timeout=8)
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(f"{process.name} did not honor its stop sentinel") from error
    require(exit_code == 0, f"{process.name} exited {exit_code}\n{process.diagnostics()}")
    health = read_json(health_path)
    require(not health.get("live", True), f"{process.name} final health remained live: {health}")
    require(not health.get("ready", True), f"{process.name} final health remained ready: {health}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True, type=existing_file)
    parser.add_argument("--gateway", required=True, type=existing_file)
    parser.add_argument("--module", required=True, type=existing_file)
    parser.add_argument("--probe", required=True, type=existing_file)
    parser.add_argument("--working-dir", type=Path, default=Path.cwd())
    args = parser.parse_args()
    working_dir = args.working_dir.resolve()
    require(working_dir.is_dir(), f"working directory does not exist: {working_dir}")

    processes: list[ManagedProcess] = []
    endpoint_paths: list[Path] = []
    with tempfile.TemporaryDirectory(prefix="spark-server-gateway-") as temporary:
        root = Path(temporary)
        try:
            key_file = root / "gateway.key"
            make_private_key(key_file)
            ports = reserve_udp_ports(5)
            server_ports = ports[:2]
            control_ports = ports[2:4]
            gateway_port = ports[4]
            unique = f"{os.getpid()}-{time.monotonic_ns()}"
            ingress_endpoint = f"spark-gateway-topology-{unique}"
            control_endpoints = [f"spark-area-control-{port}" for port in control_ports]
            if os.name != "nt":
                endpoint_paths = [Path("/tmp") / f"{name}.sock" for name in [ingress_endpoint, *control_endpoints]]

            server_health = [root / "server-alpha-health.json", root / "server-beta-health.json"]
            server_stops = [root / "server-alpha.stop", root / "server-beta.stop"]
            for index, label in enumerate(("Alpha", "Beta")):
                command = [
                    str(args.server),
                    "--module",
                    str(args.module),
                    "--port",
                    str(server_ports[index]),
                    "--map",
                    f"topology-{label.lower()}",
                    "--name",
                    f"Topology {label}",
                    "--max-clients",
                    "8",
                    "--tick-rate",
                    "60",
                    "--no-lan-broadcast",
                    "--health-file",
                    str(server_health[index]),
                    "--stop-file",
                    str(server_stops[index]),
                    "--control-endpoint",
                    control_endpoints[index],
                    "--gateway-key-file",
                    str(key_file),
                    "--control-state-file",
                    str(root / f"server-{label.lower()}-epochs.txt"),
                    "--status-interval-ms",
                    "100",
                ]
                processes.append(
                    ManagedProcess(
                        f"SparkServer-{label}", command, working_dir, root / f"server-{label.lower()}.log"
                    )
                )

            for index, server in enumerate(processes):
                initial = wait_health(
                    server,
                    server_health[index],
                    lambda value: value.get("live") is True
                    and value.get("ready") is True
                    and int(value.get("loadedModules", 0)) >= 1
                    and bool(value.get("gameModule")),
                    "to publish ready module health",
                )
                initial_ticks = int(initial.get("ticks", 0))
                advanced = wait_health(
                    server,
                    server_health[index],
                    lambda value, ticks=initial_ticks: value.get("ready") is True
                    and int(value.get("ticks", 0)) > ticks,
                    "to advance network ticks",
                    timeout=4,
                )
                require(int(advanced["port"]) == server_ports[index], "server health published the wrong UDP port")
                require_udp_admission(server_ports[index], f"Topology{index + 1}")

            gateway_config = root / "gateway.ini"
            gateway_health = root / "gateway-health.json"
            gateway_stop = root / "gateway.stop"
            write_gateway_config(
                gateway_config, key_file, ingress_endpoint, gateway_port, server_ports, control_ports
            )
            gateway = ManagedProcess(
                "SparkGateway",
                [
                    str(args.gateway),
                    "--config",
                    str(gateway_config),
                    "--key-file",
                    str(key_file),
                    "--health-file",
                    str(gateway_health),
                    "--stop-file",
                    str(gateway_stop),
                    "--status-interval-ms",
                    "100",
                ],
                working_dir,
                root / "gateway.log",
            )
            processes.append(gateway)
            gateway_status = wait_health(
                gateway,
                gateway_health,
                lambda value: value.get("live") is True
                and value.get("ready") is True
                and value.get("authenticationReady") is True
                and value.get("controlPlaneReady") is True
                and value.get("ingressReady") is True
                and int(value.get("activeAreas", 0)) == 2,
                "to publish ready two-area health",
            )
            require(int(gateway_status["port"]) == gateway_port, "gateway health published the wrong port")

            route = run_gateway_probe(args.probe, working_dir, ingress_endpoint, key_file, 41001, 81001)
            require(route.get("accepted") is True, f"gateway rejected valid admission: {route}")
            require(route.get("host") == "127.0.0.1", f"gateway returned unexpected host: {route}")
            require(int(route.get("port", 0)) in server_ports, f"gateway routed outside the live areas: {route}")
            require(int(route.get("area", 0)) > 0, f"gateway returned no authoritative area: {route}")

            request_graceful_stop(processes[0], server_stops[0], server_health[0])
            degraded = wait_health(
                gateway,
                gateway_health,
                lambda value: value.get("live") is True
                and value.get("ready") is False
                and value.get("controlPlaneReady") is False
                and int(value.get("activeAreas", -1)) == 1,
                "to report one unavailable area",
            )
            require(int(degraded["activeAreas"]) == 1, f"gateway area count did not degrade: {degraded}")
            rejected = run_gateway_probe(args.probe, working_dir, ingress_endpoint, key_file, 41002, 81002)
            require(rejected.get("accepted") is False, f"degraded gateway admitted a session: {rejected}")
            require(rejected.get("failure") == "NotReady", f"unexpected degraded route failure: {rejected}")

            request_graceful_stop(gateway, gateway_stop, gateway_health)
            request_graceful_stop(processes[1], server_stops[1], server_health[1])

            print(
                json.dumps(
                    {
                        "passed": True,
                        "servers": 2,
                        "gatewayReady": True,
                        "udpAdmissions": 2,
                        "routedPort": route["port"],
                        "degradedAreaCount": 1,
                        "degradedAdmission": rejected["failure"],
                        "gracefulShutdowns": 3,
                    },
                    sort_keys=True,
                )
            )
            return 0
        except Exception as error:
            print(f"SparkServerGatewayProcessSmoke: {error}", file=sys.stderr)
            for process in processes:
                print(f"\n--- {process.name} ({process.poll()}) ---", file=sys.stderr)
                print(process.diagnostics(), file=sys.stderr)
            return 1
        finally:
            for process in reversed(processes):
                process.cleanup()
            for endpoint_path in endpoint_paths:
                try:
                    endpoint_path.unlink(missing_ok=True)
                except OSError:
                    pass


if __name__ == "__main__":
    raise SystemExit(main())

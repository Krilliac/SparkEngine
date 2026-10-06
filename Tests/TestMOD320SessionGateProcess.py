"""MOD-320: one real MMO server and two real clients over secured loopback UDP.

The peer executable owns no test server behavior.  This coordinator only starts
the three production-process peers, exchanges character IDs, and checks the
observable success and refusal events.  Credentials remain constants inside the
C++ fixture and are never passed as arguments or printed.
"""

from __future__ import annotations

import argparse
import math
import queue
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field


@dataclass
class Peer:
    name: str
    process: subprocess.Popen[str]
    lines: list[str] = field(default_factory=list)
    _queue: queue.Queue[str | None] = field(default_factory=queue.Queue)

    def __post_init__(self) -> None:
        assert self.process.stdout is not None

        def reader() -> None:
            for line in self.process.stdout:
                self._queue.put(line.rstrip("\r\n"))
            self._queue.put(None)

        threading.Thread(target=reader, name=f"mod320-{self.name}", daemon=True).start()

    def pump(self) -> None:
        while True:
            try:
                line = self._queue.get_nowait()
            except queue.Empty:
                return
            if line is None:
                return
            self.lines.append(line)

    def send(self, command: str) -> None:
        assert self.process.stdin is not None
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()

    def find(self, prefix: str) -> str | None:
        for line in self.lines:
            if line.startswith("SESSION_GATE " + prefix):
                return line
        return None

    def stop(self) -> None:
        if self.process.poll() is None:
            try:
                self.send("quit")
            except (BrokenPipeError, OSError):
                pass
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                try:
                    self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    # Never let cleanup replace the test's own failure diagnostics.
                    pass


def wait_for(peers: list[Peer], predicate, deadline: float) -> bool:
    while time.monotonic() < deadline:
        for peer in peers:
            peer.pump()
        if predicate():
            return True
        if any(peer.process.poll() not in (None, 0) for peer in peers):
            return False
        time.sleep(0.01)
    return False


def launch(peer_path: str, role: str, *extra: str) -> Peer:
    process = subprocess.Popen(
        [peer_path, "--role", role, *extra],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    return Peer(role, process)


def parse_fields(line: str) -> dict[str, str]:
    return dict(token.split("=", 1) for token in line.split()[2:] if "=" in token)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--peer", required=True)
    parser.add_argument("--timeout", type=float, default=150.0)
    args = parser.parse_args()
    peers: list[Peer] = []
    try:
        server = launch(args.peer, "server")
        peers.append(server)
        deadline = time.monotonic() + args.timeout
        if not wait_for([server], lambda: server.find("ready ") is not None, deadline):
            raise RuntimeError("server did not publish its ephemeral port and public key")
        fields = parse_fields(server.find("ready ") or "")
        port = fields.get("port")
        key = fields.get("key")
        if not port or not key:
            raise RuntimeError("server ready line omitted port or pinned public key")

        alice = launch(args.peer, "alice", "--port", port, "--server-key", key)
        bob = launch(args.peer, "bob", "--port", port, "--server-key", key)
        peers.extend((alice, bob))
        if not wait_for(
            [alice, bob],
            lambda: alice.find("ready role=alice ") is not None and bob.find("ready role=bob ") is not None,
            deadline,
        ):
            raise RuntimeError("one or both clients did not authenticate, create, and enter world")

        alice_fields = parse_fields(alice.find("ready role=alice ") or "")
        bob_fields = parse_fields(bob.find("ready role=bob ") or "")
        alice_character = alice_fields.get("character")
        bob_character = bob_fields.get("character")
        if not alice_character or not bob_character or alice_character == bob_character:
            raise RuntimeError("clients did not receive distinct owned character IDs")
        account_a, account_b = alice_fields.get("account", "0"), bob_fields.get("account", "0")
        if account_a == "0" or account_b == "0" or account_a == account_b:
            raise RuntimeError("clients did not authenticate distinct accounts")

        alice.send(f"act {bob_character}")
        bob.send(f"act {alice_character}")
        if not wait_for(
            [alice, bob],
            lambda: alice.find("done role=alice ") is not None and bob.find("done role=bob ") is not None,
            deadline,
        ):
            raise RuntimeError("authoritative movement and interaction did not converge")

        alice_done = parse_fields(alice.find("done role=alice ") or "")
        bob_done = parse_fields(bob.find("done role=bob ") or "")
        if alice_done.get("target") != bob_character or bob_done.get("target") != alice_character:
            raise RuntimeError("authoritative interaction target was not the other client's character")
        if alice_done.get("observed") != bob_character or bob_done.get("observed") != alice_character:
            raise RuntimeError("clients did not observe the other client's authoritative state")
        if alice_done.get("interactions") != "1" or bob_done.get("interactions") != "1":
            raise RuntimeError("authoritative interaction count did not converge to one")
        for view, own in ((alice_done, alice_character), (bob_done, bob_character)):
            if view.get("observedInteractions") != "1" or view.get("observedTarget") != own:
                raise RuntimeError("peer interaction was not observed authoritatively")
            for field, expected in (("observedX", 1.0), ("ownX", 1.0), ("observedZ", 0.0)):
                value = float(view.get(field, "nan"))
                if not math.isfinite(value) or abs(value - expected) > 0.001:
                    raise RuntimeError("authoritative movement or ownership protection did not converge")

        if not alice.find("negative bad-password=refused") and not bob.find("negative bad-password=refused"):
            raise RuntimeError("bad credentials were not rejected")
        if not bob.find("negative ownership=refused"):
            raise RuntimeError("client B ownership violations were not rejected")
        if not alice.find("negative unauthenticated=refused") or not bob.find("negative unauthenticated=refused"):
            raise RuntimeError("unauthenticated create or enter was not rejected")
        alice.stop()
        bob.stop()
        server.stop()
        if any(peer.process.returncode != 0 for peer in peers):
            raise RuntimeError("a peer exited non-zero: " + ", ".join(f"{p.name}={p.process.returncode}" for p in peers))
        print("MOD-320 three-process session gate: PASS")
        return 0
    except (RuntimeError, OSError, ValueError) as error:
        print(f"MOD-320 three-process session gate: FAIL: {error}", file=sys.stderr)
        for peer in peers:
            peer.pump()
            print(f"--- {peer.name} (exit={peer.process.poll()}) ---", file=sys.stderr)
            for line in peer.lines[-40:]:
                print(line, file=sys.stderr)
        return 1
    finally:
        for peer in reversed(peers):
            peer.stop()


if __name__ == "__main__":
    raise SystemExit(main())

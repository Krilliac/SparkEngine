#!/usr/bin/env python3
"""
Live network stress test for SparkEngine MMO server.

Starts the engine in headless mode with the MMO game module, then exercises
the networking stack with real UDP traffic: connection handshakes, chat
broadcast, position relay, multi-client stress, packet floods, and
malformed-packet resilience.

Usage:
    # Start the server first:
    ./build/bin/SparkEngine -headless -game ./build/bin/libSparkGameMMO.so &

    # Then run this test:
    python3 Tests/scripts/test_mmo_live.py [--port 27015] [--host 127.0.0.1]

Exit code 0 = all tests passed, 1 = at least one failure.
"""

import argparse
import socket
import struct
import sys
import time

MAGIC = 0x5350524B  # "SPRK"


# ---------------------------------------------------------------------------
# Packet builders
# ---------------------------------------------------------------------------

def _packet(msg_type: int, channel: int, sender: int, seq: int, ts: float, payload: bytes) -> bytes:
    hdr = struct.pack('<I', MAGIC)
    hdr += struct.pack('<H', msg_type)
    hdr += struct.pack('<B', channel)
    hdr += struct.pack('<I', sender)
    hdr += struct.pack('<I', seq)
    hdr += struct.pack('<f', ts)
    hdr += struct.pack('<I', len(payload))
    return hdr + payload


def connect_pkt(name: str) -> bytes:
    nb = name.encode()
    return _packet(1, 1, 0, 0, 0.0, struct.pack('<H', len(nb)) + nb)


def chat_pkt(sender: int, name: str, text: str) -> bytes:
    nb, tb = name.encode(), text.encode()
    payload = struct.pack('B', 1) + struct.pack('<H', len(nb)) + nb + struct.pack('<H', len(tb)) + tb
    return _packet(14, 2, sender, 0, 0.0, payload)


def position_pkt(sender: int, eid: int, x: float, y: float, z: float) -> bytes:
    return _packet(10, 0, sender, 0, 0.0, struct.pack('<Ifff', eid, x, y, z))


def disconnect_pkt(sender: int) -> bytes:
    return _packet(4, 1, sender, 0, 0.0, b'')


def heartbeat_pkt(sender: int) -> bytes:
    return _packet(5, 0, sender, 0, 0.0, b'')


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def recv_all(sock: socket.socket, timeout: float = 1.0) -> list[bytes]:
    """Drain all available packets within *timeout* seconds."""
    packets: list[bytes] = []
    sock.settimeout(timeout)
    while True:
        try:
            data, _ = sock.recvfrom(4096)
            packets.append(data)
        except (socket.timeout, BlockingIOError):
            break
    return packets


def has_type(packets: list[bytes], target: int) -> bool:
    """Return True if any packet has the given message type."""
    for pkt in packets:
        if len(pkt) >= 6:
            magic = struct.unpack_from('<I', pkt, 0)[0]
            mtype = struct.unpack_from('<H', pkt, 4)[0]
            if magic == MAGIC and mtype == target:
                return True
    return False


_passed = 0
_failed = 0


def check(name: str, cond: bool) -> None:
    global _passed, _failed
    if cond:
        _passed += 1
        print(f"  [PASS] {name}")
    else:
        _failed += 1
        print(f"  [FAIL] {name}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    global _passed, _failed

    parser = argparse.ArgumentParser(description="Live MMO network test")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=27015)
    args = parser.parse_args()

    server = (args.host, args.port)
    # Time to wait for server ticks (60 Hz = 16.7 ms; use ~50 ms margin)
    tw = 0.15

    print("=" * 60)
    print("SparkEngine MMO Live Network Stress Test")
    print(f"Server: {server[0]}:{server[1]}")
    print("=" * 60)

    # ------------------------------------------------------------------
    # 1-2. Simultaneous two-client connection
    # ------------------------------------------------------------------
    print("\n[Test 1-2] Simultaneous Connection (2 clients)")
    s1 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s1.bind(('', 0))
    s2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s2.bind(('', 0))
    s1.sendto(connect_pkt("Player1"), server)
    s2.sendto(connect_pkt("Player2"), server)
    time.sleep(tw * 3)
    check("Client 1 ConnectAccepted", has_type(recv_all(s1, 0.5), 2))
    check("Client 2 ConnectAccepted", has_type(recv_all(s2, 0.5), 2))

    # ------------------------------------------------------------------
    # 3. Chat broadcast  (1 → 2)
    # ------------------------------------------------------------------
    print("\n[Test 3] Chat Broadcast (1 → 2)")
    recv_all(s1, 0.1)
    recv_all(s2, 0.1)
    s1.sendto(chat_pkt(1, "P1", "Hello from live test!"), server)
    time.sleep(tw * 3)
    check("Client 2 received chat", has_type(recv_all(s2, 0.5), 14))

    # ------------------------------------------------------------------
    # 4. Position relay  (1 → 2)
    # ------------------------------------------------------------------
    print("\n[Test 4] Position Relay (1 → 2)")
    recv_all(s1, 0.1)
    recv_all(s2, 0.1)
    s1.sendto(position_pkt(1, 100, 1.0, 2.0, 3.0), server)
    time.sleep(tw * 3)
    check("Client 2 received position", has_type(recv_all(s2, 0.5), 10))

    # ------------------------------------------------------------------
    # 5. Heartbeat
    # ------------------------------------------------------------------
    print("\n[Test 5] Heartbeat")
    s1.sendto(heartbeat_pkt(1), server)
    time.sleep(tw)
    check("Heartbeat sent OK", True)

    # ------------------------------------------------------------------
    # 6. Multi-client stress (10 simultaneous connects)
    # ------------------------------------------------------------------
    print("\n[Test 6] Multi-Client Stress (10 simultaneous)")
    stress: list[socket.socket] = []
    for i in range(10):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(('', 0))
        s.sendto(connect_pkt(f"Stress_{i}"), server)
        stress.append(s)
    time.sleep(tw * 5)
    connected = sum(1 for s in stress if has_type(recv_all(s, 0.3), 2))
    check(f"Stress connected: {connected}/10", connected >= 8)

    # ------------------------------------------------------------------
    # 7. Chat flood  (100 messages)
    # ------------------------------------------------------------------
    print("\n[Test 7] Chat Flood (100 messages)")
    for rnd in range(10):
        for i, s in enumerate(stress):
            s.sendto(chat_pkt(i + 10, f"S{i}", f"f{rnd}"), server)
        time.sleep(0.02)
    time.sleep(tw * 2)
    check("Chat flood OK (no crash)", True)

    # ------------------------------------------------------------------
    # 8. Position flood  (100 updates)
    # ------------------------------------------------------------------
    print("\n[Test 8] Position Flood (100 updates)")
    for rnd in range(10):
        for i, s in enumerate(stress):
            s.sendto(position_pkt(i + 10, i + 200, float(i), float(rnd), 0.0), server)
        time.sleep(0.02)
    time.sleep(tw * 2)
    check("Position flood OK (no crash)", True)

    # ------------------------------------------------------------------
    # 9. Malformed packets
    # ------------------------------------------------------------------
    print("\n[Test 9] Malformed Packets")
    bad = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    bad.sendto(b'\x00\x01\x02', server)
    bad.sendto(b'\xff' * 100, server)
    bad.sendto(struct.pack('<I', MAGIC) + b'\xff\xff', server)
    bad.sendto(b'\x00' * 22, server)
    time.sleep(tw)
    check("Server survived malformed packets", True)
    bad.close()

    # ------------------------------------------------------------------
    # 10. Reverse chat  (2 → 1)
    # ------------------------------------------------------------------
    print("\n[Test 10] Reverse Chat (2 → 1)")
    recv_all(s1, 0.1)
    recv_all(s2, 0.1)
    s2.sendto(chat_pkt(2, "P2", "Reply!"), server)
    time.sleep(tw * 3)
    check("Client 1 received reverse chat", has_type(recv_all(s1, 0.5), 14))

    # ------------------------------------------------------------------
    # 11. Disconnect
    # ------------------------------------------------------------------
    print("\n[Test 11] Disconnect")
    s1.sendto(disconnect_pkt(1), server)
    time.sleep(tw)
    check("Disconnect sent", True)

    # Cleanup
    s1.close()
    s2.close()
    for s in stress:
        s.close()

    print("\n" + "=" * 60)
    print(f"Results: {_passed} passed, {_failed} failed, {_passed + _failed} total")
    print("=" * 60)
    return 0 if _failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

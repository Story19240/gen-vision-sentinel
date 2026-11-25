#!/usr/bin/env python3
"""Loopback Modbus-RTU-over-TCP relay sink for test_monitor_relay."""

import socket
import sys


def validate_packets(packets):
    if not packets:
        raise RuntimeError("relay client sent no packets")

    states = []
    for packet in packets:
        if len(packet) != 8:
            raise RuntimeError(f"unexpected packet length: {len(packet)}")
        if packet[0] != 1 or packet[1] != 5:
            raise RuntimeError(f"unexpected Modbus PDU: {packet.hex()}")
        channel = int.from_bytes(packet[2:4], "big") + 1
        if channel != 1:
            raise RuntimeError(f"cross-channel write detected: Y{channel}")
        value = packet[4:6]
        if value not in (b"\x00\x00", b"\xff\x00"):
            raise RuntimeError(f"invalid coil value: {value.hex()}")
        states.append(value == b"\xff\x00")

    if states[0] is not False:
        raise RuntimeError("first command did not reset Y1")
    if True not in states:
        raise RuntimeError("confirmed spark did not energize Y1")
    if states[-1] is not False:
        raise RuntimeError("last command did not release Y1")
    return states


def main():
    if len(sys.argv) != 3:
        print("Usage: test_relay_server.py PORT PACKET_LOG", file=sys.stderr)
        return 2
    port = int(sys.argv[1])
    log_path = sys.argv[2]
    packets = []

    with open(log_path, "w", encoding="ascii") as log:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind(("127.0.0.1", port))
            server.listen(1)
            server.settimeout(30)
            connection, _ = server.accept()
            with connection:
                connection.settimeout(30)
                buffered = b""
                while True:
                    data = connection.recv(4096)
                    if not data:
                        break
                    buffered += data
                    while len(buffered) >= 8:
                        packet = buffered[:8]
                        buffered = buffered[8:]
                        packets.append(packet)
                        log.write(packet.hex() + "\n")
                        log.flush()
                        connection.sendall(packet)
                if buffered:
                    raise RuntimeError("truncated relay packet at disconnect")

    states = validate_packets(packets)
    print(
        f"RELAY_PACKETS_OK packets={len(states)} "
        f"on={states.count(True)} off={states.count(False)} channels=Y1"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"RELAY_PACKETS_FAILED: {error}", file=sys.stderr)
        sys.exit(1)

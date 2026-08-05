#!/usr/bin/env python3
"""Read CFG_FLIGHTSIM through the drone's local WebSocket API."""

import base64
import hashlib
import json
import os
import socket
import struct
import sys
import time
import uuid


HOST = "127.0.0.1"
PORT = 8081
PATH = "/ws?topics=telemetry,event"
PARAMETER = "CFG_FLIGHTSIM"
TIMEOUT_SECONDS = 10.0


def read_exact(stream, length):
    chunks = []
    remaining = length
    while remaining:
        chunk = stream.read(remaining)
        if not chunk:
            raise RuntimeError("WebSocket closed before the response completed")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def send_frame(connection, opcode, payload=b""):
    mask = os.urandom(4)
    size = len(payload)
    header = bytearray([0x80 | opcode])
    if size < 126:
        header.append(0x80 | size)
    elif size <= 0xFFFF:
        header.append(0x80 | 126)
        header.extend(struct.pack("!H", size))
    else:
        header.append(0x80 | 127)
        header.extend(struct.pack("!Q", size))
    header.extend(mask)
    masked = bytes(value ^ mask[index % 4] for index, value in enumerate(payload))
    connection.sendall(bytes(header) + masked)


def receive_frame(stream):
    first, second = read_exact(stream, 2)
    opcode = first & 0x0F
    masked = bool(second & 0x80)
    size = second & 0x7F
    if size == 126:
        size = struct.unpack("!H", read_exact(stream, 2))[0]
    elif size == 127:
        size = struct.unpack("!Q", read_exact(stream, 8))[0]
    mask = read_exact(stream, 4) if masked else b""
    payload = read_exact(stream, size)
    if masked:
        payload = bytes(
            value ^ mask[index % 4] for index, value in enumerate(payload)
        )
    return opcode, payload


def main():
    request_id = "flow-test-" + str(uuid.uuid4())
    websocket_key = base64.b64encode(os.urandom(16)).decode("ascii")
    expected_accept = base64.b64encode(
        hashlib.sha1(
            (websocket_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode(
                "ascii"
            )
        ).digest()
    ).decode("ascii")

    with socket.create_connection((HOST, PORT), timeout=TIMEOUT_SECONDS) as connection:
        connection.settimeout(TIMEOUT_SECONDS)
        request = (
            f"GET {PATH} HTTP/1.1\r\n"
            f"Host: {HOST}:{PORT}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {websocket_key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        connection.sendall(request.encode("ascii"))
        stream = connection.makefile("rb")
        status_line = stream.readline().decode("latin-1").strip()
        headers = {}
        while True:
            line = stream.readline().decode("latin-1")
            if line in ("\r\n", "\n", ""):
                break
            name, value = line.split(":", 1)
            headers[name.strip().lower()] = value.strip()
        if " 101 " not in f" {status_line} ":
            raise RuntimeError(f"WebSocket upgrade failed: {status_line}")
        if headers.get("sec-websocket-accept") != expected_accept:
            raise RuntimeError("WebSocket accept key mismatch")

        envelope = {
            "version": "2.0",
            "type": "command",
            "needResponse": True,
            "session": "match_agent_flow_test_preflight",
            "requestId": request_id,
            "command": {
                "version": "1.0",
                "name": "fc_get_param",
                "data": {"param_id": PARAMETER},
            },
        }
        send_frame(connection, 0x1, json.dumps(envelope).encode("utf-8"))

        deadline = time.monotonic() + TIMEOUT_SECONDS
        while time.monotonic() < deadline:
            opcode, payload = receive_frame(stream)
            if opcode == 0x8:
                raise RuntimeError("WebSocket closed before CFG_FLIGHTSIM arrived")
            if opcode == 0x9:
                send_frame(connection, 0xA, payload)
                continue
            if opcode != 0x1:
                continue
            message = json.loads(payload.decode("utf-8"))
            if message.get("type") != "response":
                continue
            if message.get("requestAck") != request_id:
                continue
            response = message.get("response") or {}
            data = response.get("data") or {}
            integer = data.get("integer")
            success = data.get("success") is True
            result = {
                "parameter": PARAMETER,
                "integer": integer,
                "simulation": integer == 1,
                "success": success,
            }
            print(json.dumps(result, ensure_ascii=True, separators=(",", ":")))
            return 0 if success and integer == 1 else 1

    raise RuntimeError("CFG_FLIGHTSIM response timed out")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"simulation preflight failed: {error}", file=sys.stderr)
        raise SystemExit(1)

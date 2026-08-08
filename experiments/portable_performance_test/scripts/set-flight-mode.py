#!/usr/bin/env python3
"""Set CFG_FLIGHTSIM through the drone's local WebSocket API and verify it."""

import base64
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import time
import uuid


HOST = "127.0.0.1"
PORT = 8081
PATH = "/ws?topics=telemetry,event"
TIMEOUT_SECONDS = 10.0
PARAMETER = "CFG_FLIGHTSIM"
READER = Path(__file__).with_name("read-parameter.py")


def read_exact(stream, length):
    chunks = []
    while length:
        chunk = stream.read(length)
        if not chunk:
            raise RuntimeError("WebSocket closed before the response completed")
        chunks.append(chunk)
        length -= len(chunk)
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


def open_websocket():
    connection = socket.create_connection((HOST, PORT), timeout=TIMEOUT_SECONDS)
    connection.settimeout(TIMEOUT_SECONDS)
    websocket_key = base64.b64encode(os.urandom(16)).decode("ascii")
    expected_accept = base64.b64encode(
        hashlib.sha1(
            (websocket_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode(
                "ascii"
            )
        ).digest()
    ).decode("ascii")
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
        connection.close()
        raise RuntimeError(f"WebSocket upgrade failed: {status_line}")
    if headers.get("sec-websocket-accept") != expected_accept:
        connection.close()
        raise RuntimeError("WebSocket accept key mismatch")
    return connection, stream


def read_mode():
    result = subprocess.run(
        [sys.executable, str(READER), PARAMETER],
        check=False,
        text=True,
        capture_output=True,
    )
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "CFG_FLIGHTSIM read failed")
    data = json.loads(result.stdout)
    if data.get("success") is not True or data.get("integer") not in (0, 1):
        raise RuntimeError("CFG_FLIGHTSIM returned an invalid value")
    return int(data["integer"])


def send_mode(target):
    connection, stream = open_websocket()
    request_id = "set-flight-mode-" + str(uuid.uuid4())
    envelope = {
        "version": "2.0",
        "type": "command",
        "needResponse": True,
        "session": "portable_performance_test_mode_switch",
        "requestId": request_id,
        "command": {
            "version": "1.0",
            "name": "fc_set_param",
            "data": {
                "param_id": PARAMETER,
                "integer": target,
                "real": 0,
            },
        },
    }
    try:
        send_frame(connection, 0x1, json.dumps(envelope).encode("utf-8"))
        deadline = time.monotonic() + TIMEOUT_SECONDS
        while time.monotonic() < deadline:
            opcode, payload = receive_frame(stream)
            if opcode == 0x8:
                raise RuntimeError("WebSocket closed before the write response")
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
            if response.get("status", 0) != 0 or data.get("success") is False:
                detail = response.get("message") or "fc_set_param rejected the write"
                raise RuntimeError(detail)
            return
    finally:
        stream.close()
        connection.close()
    raise RuntimeError("CFG_FLIGHTSIM write response timed out")


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ("sim", "real"):
        print(f"usage: {sys.argv[0]} sim|real", file=sys.stderr)
        return 2
    target_name = sys.argv[1]
    target = 1 if target_name == "sim" else 0
    before = read_mode()
    if before == target:
        print(
            json.dumps(
                {
                    "changed": False,
                    "before": before,
                    "after": before,
                    "mode": target_name,
                    "verified": True,
                },
                separators=(",", ":"),
            )
        )
        return 0

    send_mode(target)
    last_error = None
    for _ in range(5):
        time.sleep(1)
        try:
            after = read_mode()
        except Exception as error:
            last_error = error
            continue
        if after == target:
            print(
                json.dumps(
                    {
                        "changed": True,
                        "before": before,
                        "after": after,
                        "mode": target_name,
                        "verified": True,
                    },
                    separators=(",", ":"),
                )
            )
            return 0
        last_error = RuntimeError(f"read-back returned {after}")
    raise RuntimeError(f"write was not verified: {last_error}")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"flight mode switch failed: {error}", file=sys.stderr)
        raise SystemExit(1)

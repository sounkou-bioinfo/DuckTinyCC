#!/usr/bin/env python3
"""Minimal jgd JSONL capture server for DuckTinyCC demos.

It implements just enough of the jgd protocol for an R jgd device to connect,
request font metrics, send frames, and close. Captured messages are written to a
JSON file for inspection after the DuckDB query returns.
"""

import json
import socket
import sys


if len(sys.argv) != 3:
    print("usage: jgd_capture_server.py <port> <capture.json>", file=sys.stderr)
    sys.exit(2)

HOST = "127.0.0.1"
PORT = int(sys.argv[1])
OUT = sys.argv[2]

messages = []
frames = []

server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind((HOST, PORT))
server.listen(1)
print(f"ready tcp://{HOST}:{PORT}", flush=True)

conn, _addr = server.accept()
stream = conn.makefile("rwb", buffering=0)
welcomed = False


def send(obj):
    stream.write((json.dumps(obj, separators=(",", ":")) + "\n").encode("utf-8"))


while True:
    line = stream.readline()
    if not line:
        break
    text = line.decode("utf-8", "replace").rstrip("\n")
    if not text:
        continue
    try:
        msg = json.loads(text)
    except json.JSONDecodeError:
        continue

    messages.append(msg)

    if not welcomed:
        send({
            "type": "server_info",
            "serverName": "ducktinycc-jgd-capture",
            "protocolVersion": 1,
            "transport": "tcp",
            "serverInfo": {"mode": "capture", "frontend": "DuckTinyCC demo"},
        })
        welcomed = True

    typ = msg.get("type")
    if typ == "metrics_request":
        # A deliberately simple metrics backend. It is enough for base plots and
        # keeps the capture server dependency-free.
        if msg.get("kind") == "strWidth":
            width = 7.2 * len(str(msg.get("str", "")))
            send({"type": "metrics_response", "id": msg.get("id"), "width": width, "ascent": 10.0, "descent": 3.0})
        else:
            send({"type": "metrics_response", "id": msg.get("id"), "width": 8.0, "ascent": 10.0, "descent": 3.0})
    elif typ == "resize":
        # Not expected from this capture server, but tolerated by the protocol.
        pass
    elif typ == "frame":
        frames.append(msg)
    elif typ == "close":
        break

try:
    stream.close()
    conn.close()
    server.close()
finally:
    with open(OUT, "w", encoding="utf-8") as fp:
        json.dump({"messages": messages, "frames": frames}, fp)

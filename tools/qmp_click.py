#!/usr/bin/env python3
"""Send a relative mouse move and left click through QMP."""
import json
import socket
import sys
import time

SOCKET = sys.argv[1] if len(sys.argv) > 1 else "/tmp/cos_gui_qmp.sock"
DX = int(sys.argv[2]) if len(sys.argv) > 2 else 0
DY = int(sys.argv[3]) if len(sys.argv) > 3 else 0

def send(sock, payload):
    sock.sendall((json.dumps(payload) + "\n").encode())
    while True:
        line = b""
        while not line.endswith(b"\n"):
            chunk = sock.recv(4096)
            if not chunk:
                raise RuntimeError("QMP disconnected")
            line += chunk
        response = json.loads(line.decode())
        if "return" in response:
            return response
        if "error" in response:
            raise RuntimeError(response["error"])

def rel_event(axis, value):
    return {"type": "rel", "data": {"axis": axis, "value": value}}

def button_event(down):
    return {"type": "btn", "data": {"button": "left", "down": down}}

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
    sock.connect(SOCKET)
    sock.recv(4096)
    send(sock, {"execute": "qmp_capabilities"})
    send(sock, {"execute": "input-send-event", "arguments": {"events": [rel_event("x", DX), rel_event("y", DY)]}})
    time.sleep(0.15)
    send(sock, {"execute": "input-send-event", "arguments": {"events": [button_event(True), button_event(False)]}})
print(f"clicked after relative move ({DX}, {DY})")

#!/usr/bin/env python3
"""Request a QEMU HMP screendump through QMP."""
import json
import socket
import sys

sock_path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/cos_qemu_strict.qmp"
out_path = sys.argv[2] if len(sys.argv) > 2 else "/tmp/cos_screen.ppm"

def send(sock, payload):
    sock.sendall((json.dumps(payload) + "\n").encode())
    while True:
        line = b""
        while not line.endswith(b"\n"):
            chunk = sock.recv(4096)
            if not chunk:
                raise RuntimeError("QMP disconnected")
            line += chunk
        obj = json.loads(line.decode())
        if "return" in obj:
            return obj
        if "error" in obj:
            raise RuntimeError(obj["error"])

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
    sock.connect(sock_path)
    sock.recv(4096)
    send(sock, {"execute": "qmp_capabilities"})
    result = send(sock, {"execute": "human-monitor-command", "arguments": {"command-line": f"screendump {out_path}"}})
    print(result)

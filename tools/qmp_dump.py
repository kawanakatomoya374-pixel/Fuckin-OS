#!/usr/bin/env python3
"""qmp_dump.py QMP_SOCKET - prints the VM run state and, when the VM is not running (a guest triple fault stops it: the launcher uses -no-reboot -no-shutdown),
the registers of every CPU as they were at the fault (HMP "info registers -a"). Exit status 0: running, 1: stopped."""
import json, socket, sys
s = socket.socket(socket.AF_UNIX); s.settimeout(10); s.connect(sys.argv[1]); s.recv(4096)
def cmd(o):
    s.sendall((json.dumps(o) + "\n").encode()); buf = b""
    while True:
        buf += s.recv(65536)
        for line in buf.decode(errors="replace").split("\n"):
            if line.strip():
                try: j = json.loads(line)
                except Exception: continue
                if "return" in j or "error" in j: return j
cmd({"execute": "qmp_capabilities"})
st = cmd({"execute": "query-status"})["return"]
print("status:", st["status"], "running:", st["running"])
if st["running"] and "--force" not in sys.argv: sys.exit(0)
print(cmd({"execute": "human-monitor-command", "arguments": {"command-line": "info registers -a"}}).get("return", ""))
sys.exit(1)

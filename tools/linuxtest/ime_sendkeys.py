import socket, json, time, sys
Q = "/tmp/cos-linux.qmp"
def cmd(obj):
    s = socket.socket(socket.AF_UNIX); s.connect(Q); s.recv(4096)
    s.sendall(b'{"execute":"qmp_capabilities"}\n'); time.sleep(0.15); s.recv(4096)
    s.sendall(json.dumps(obj).encode() + b"\n"); time.sleep(0.4); r = s.recv(8192); s.close(); return r
def keys(*names):
    cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": n} for n in names], "hold-time": 80}})
def shot(name): cmd({"execute": "screendump", "arguments": {"filename": name}})
time.sleep(float(sys.argv[1]))
shot("/tmp/ime_0.ppm")
keys("ctrl", "spc"); time.sleep(3); shot("/tmp/ime_1.ppm")
for ch in "konnichiha": keys(ch); time.sleep(0.6)
time.sleep(5); shot("/tmp/ime_2.ppm")
keys("spc"); time.sleep(4); shot("/tmp/ime_3.ppm")
keys("ret"); time.sleep(4); shot("/tmp/ime_4.ppm")
print("keys sent")

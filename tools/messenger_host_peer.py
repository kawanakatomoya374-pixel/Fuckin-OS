#!/usr/bin/env python3
"""Host-side peer for the C-OS Messenger LAN test: speaks the CMSG UDP protocol through QEMU's hostfwd, with end-to-end encryption.
usage: messenger_host_peer.py <host_udp_port forwarded to guest 5150>; binds host 5150 because replies/ACKs always go to port 5150 (the guest reaches the host as 192.168.70.2).
Identities on the LAN are the IP addresses: the guest is 192.168.70.10, this host peer is 192.168.70.2 (as the guest sees it)."""
import os, socket, struct, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from cosmsg_e2e import E2E, b64e, event
GUEST, ME = "ip:c0a8460a", "ip:c0a84602"
port = int(sys.argv[1])
e = E2E(ME)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("127.0.0.1", 5150)); s.settimeout(1.0)
def pkt(t, i, text=b""):
    return b"CMSG" + struct.pack("<B3xII", t, i, int(time.time())) + b"hostpeer".ljust(24, b"\0") + text
def send(t, i, text=b""): s.sendto(pkt(t, i, text), ("127.0.0.1", port))
state = {"keyed": False, "key_asked": 0}
def serve(d):
    """answers key requests, learns the guest's key, returns (type, id, text) for everything else"""
    if d[:4] != b"CMSG": return None
    t, i = d[4], struct.unpack("<I", d[8:12])[0]; text = d[40:]
    if t == 8: send(9, 0, b64e(e.bundle()).encode()); return None                  # the guest asks for our bundle
    if t == 9 and state["key_asked"]:
        try: state["keyed"] = e.learn_key(GUEST, text.decode())
        except Exception as ex: print("bad key from the guest:", ex)
        return None
    return t, i, text
def pump(t=1.0):
    s.settimeout(t)
    try: d, _ = s.recvfrom(4096)
    except socket.timeout: return None
    return serve(d)
# 1. learn the guest's key (it publishes it for everyone who asks)
for _ in range(20):
    state["key_asked"] = 1; send(8, 0)
    end = time.time() + 2
    while time.time() < end and not state["keyed"]: pump(0.5)
    if state["keyed"]: break
print("the guest's key learnt:", state["keyed"])
acked = False
for _ in range(40):
    send(1, 4242, e.encrypt(GUEST, 4242, "Hello from the host peer").encode())
    end = time.time() + 1.5
    while time.time() < end and not acked:
        r = pump(0.5)
        if r and r[0] == 2 and r[1] == 4242: acked = True
    if acked: break
print("ACK for host->guest message:", acked)
got = None; t0 = time.time()
while time.time() - t0 < 120 and got is None:
    r = pump(1.0)
    if r and r[0] == 1:
        text = r[2].decode(errors="replace")
        if text.startswith("\x01E") and "Reply" not in text:
            try: got = e.decrypt(GUEST, r[1], text); send(2, r[1])
            except Exception as ex: print("cannot decrypt:", ex)
print("guest->host message:", repr(got), "(encrypted on the wire)")
def evack(kind, text=b""):
    for _ in range(10):
        send(kind, 4242, text)
        end = time.time() + 1.0
        while time.time() < end:
            r = pump(0.3)
            if r and r[0] == 7 and r[1] == 4242: return True
    return False
ed = evack(5, e.encrypt(GUEST, 4242, event("E", "edited by the host"), kind=1).encode()); print("EVACK for edit:", ed)
dl = evack(5, e.encrypt(GUEST, 4242, event("D", ts=int(time.time()) + 5), kind=1).encode()); print("EVACK for delete (an encrypted tombstone):", dl)
# a plain delete must be ignored
plain_del = False
while pump(0.5) is not None: pass                                  # (late answers to the earlier sends)
send(6, 4242)
end = time.time() + 2
while time.time() < end:
    r = pump(0.3)
    if r and r[0] == 7: plain_del = True
print("a plain DEL is ignored:", not plain_del)
# a plain-text message must be refused (no ACK) while encryption is required
plain_acked = False
for _ in range(3):
    send(1, 4343, b"plain text should not be accepted")
    end = time.time() + 1.5
    while time.time() < end:
        r = pump(0.3)
        if r and r[0] == 2 and r[1] == 4343: plain_acked = True
print("plain text refused:", not plain_acked)
sys.exit(0 if state["keyed"] and acked and got == "Reply from C-OS" and ed and dl and not plain_acked and not plain_del else 1)

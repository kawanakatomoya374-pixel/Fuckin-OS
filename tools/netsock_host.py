#!/usr/bin/env python3
"""Host side of the ring-3 socket test: once the guest prints '[NETSOCK] listening', connect to its forwarded port,
send a pattern, read the echo back, close, then open a second connection and read the greeting."""
import socket, sys, time
port = int(sys.argv[1]); N = 150000
def pat(i): return (i * 131 + (i >> 8) * 7) & 0xFF
data = bytes(pat(i) for i in range(N))
for attempt in range(30):
    try:
        c = socket.create_connection(('127.0.0.1', port), timeout=20); break
    except OSError: time.sleep(1)
else:
    print('host: could not connect'); sys.exit(1)
c.settimeout(60)
got = b''
sent = 0
c.setblocking(False)
import select
while len(got) < N:
    r, w, _ = select.select([c], [c] if sent < N else [], [], 30)
    if w and sent < N:
        try: sent += c.send(data[sent:sent + 8192])
        except BlockingIOError: pass
    if r:
        d = c.recv(65536)
        if not d: break
        got += d
c.close()
print('host: echoed', len(got), 'bytes', 'OK' if got == data else 'MISMATCH', flush=True)
c2 = socket.create_connection(('127.0.0.1', port), timeout=30)
msg = c2.recv(100)
print('host: second connection got', msg, flush=True)
c2.close()
sys.exit(0 if got == data and msg.startswith(b'hello') else 2)

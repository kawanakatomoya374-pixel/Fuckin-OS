#!/usr/bin/env python3
"""Server for the in-kernel network selftest (make iso VALIDATION_NET=1). Listens on 0.0.0.0:PORT.
Guest commands (one line): "DOWN <n>" -> n pattern bytes; "UP <n>" then n pattern bytes -> "OK\\n" or "BAD\\n"."""
import socket, sys, threading
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18080
def pat(i): return (i * 131 + (i >> 8) * 7) & 0xFF
def block(start, n):
    return bytes(pat(start + k) for k in range(n))
def handle(c, addr):
    try:
        line = b''
        while not line.endswith(b'\n'): 
            d = c.recv(1)
            if not d: return
            line += d
        verb, n = line.split(); n = int(n)
        if verb == b'DOWN':
            sent = 0
            while sent < n:
                m = min(65536, n - sent); c.sendall(block(sent, m)); sent += m
            print('DOWN', n, 'sent', flush=True)
        elif verb == b'UP':
            got = 0; ok = True
            while got < n:
                d = c.recv(65536)
                if not d: break
                if d != block(got, len(d)): ok = False
                got += len(d)
            ok = ok and got == n
            c.sendall(b'OK\n' if ok else b'BAD\n')
            print('UP', n, 'OK' if ok else 'BAD', flush=True)
    finally:
        c.close()
import http.server
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def do_GET(self):
        body = block(0, 300000) if self.path.startswith('/big') else b'hello from the host\n'
        self.send_response(200); self.send_header('Content-Length', str(len(body))); self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Connection', 'close'); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): print('HTTP', self.path, flush=True)
threading.Thread(target=lambda: http.server.ThreadingHTTPServer(('0.0.0.0', PORT + 1), H).serve_forever(), daemon=True).start()
def udp_echo():
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u.bind(('0.0.0.0', PORT + 2))
    print('udp echo', PORT + 2, flush=True)
    while True:
        d, a = u.recvfrom(2048)
        u.sendto(d, a)
threading.Thread(target=udp_echo, daemon=True).start()
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('0.0.0.0', PORT)); s.listen(8)
print('listening', PORT, flush=True)
while True:
    c, a = s.accept(); threading.Thread(target=handle, args=(c, a), daemon=True).start()

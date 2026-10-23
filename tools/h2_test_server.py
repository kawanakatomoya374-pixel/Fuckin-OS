#!/usr/bin/env python3
"""HTTP/2 (and HTTP/1.1 fallback) test server for the kernel's h2 client: tools/run_h2_test.sh.

  h2_test_server.py PORT CERT KEY

TLS with ALPN h2 / http/1.1. One scenario per path, each probing something an h2 client can get wrong:
  /small            200, short body                  /big/N        200, N pattern bytes (flow control)
  /slowbody         12 s of 1 KB chunks, 1 s apart   /stall        headers, then silence (client must give up)
  /early            103 Early Hints, then 200        /redirect     302 -> /small
  /rst              HEADERS then RST_STREAM          /goaway       partial body, GOAWAY, close
  /gzip             Content-Encoding: gzip           /multi        many Set-Cookie + a big header
  /trailers         body then trailers               /empty        204
  /missing          404 with a body                  /huge         12 MiB (over the client's 10 MiB cap)
  /settings         waits for the client's SETTINGS ACK before answering (preface/ack ordering)
Pattern bytes: pat(i) = (i*131 + (i>>8)*7) & 0xFF, the same as tools/net_test_server.py."""
import gzip, socket, ssl, sys, threading, time
import h2.config, h2.connection, h2.events, h2.exceptions

PORT = int(sys.argv[1]); CERT = sys.argv[2]; KEY = sys.argv[3]

def pat(i): return (i * 131 + (i >> 8) * 7) & 0xFF
def block(start, n): return bytes(pat(start + k) for k in range(n))

class Conn:
    def __init__(self, sock):
        self.sock = sock
        self.c = h2.connection.H2Connection(config=h2.config.H2Configuration(client_side=False, header_encoding='utf-8'))
        self.reset = set(); self.closed = False; self.pending = []; self.ack_seen = False

    def flush(self):
        d = self.c.data_to_send()
        if d: self.sock.sendall(d)

    def pump(self, timeout=5.0):
        """Read once, handle events; request events are queued in self.pending."""
        self.sock.settimeout(timeout)
        try:
            data = self.sock.recv(65536)
        except socket.timeout:
            return False
        except OSError:
            self.closed = True; return False
        if not data:
            self.closed = True; return False
        try:
            events = self.c.receive_data(data)
        except h2.exceptions.ProtocolError as e:
            print('protocol error from client:', e, flush=True); self.closed = True; return False
        for ev in events:
            if isinstance(ev, h2.events.RequestReceived):
                self.pending.append((ev.stream_id, dict(ev.headers)))
            elif isinstance(ev, h2.events.StreamReset):
                self.reset.add(ev.stream_id)
            elif isinstance(ev, h2.events.SettingsAcknowledged):
                self.ack_seen = True
            elif isinstance(ev, h2.events.ConnectionTerminated):
                self.closed = True
        self.flush()
        return True

    def send_body(self, sid, data, end=True, stream_chunk=None):
        off = 0
        while off < len(data):
            if self.closed or sid in self.reset: return False
            win = min(self.c.local_flow_control_window(sid), self.c.max_outbound_frame_size, len(data) - off)
            if win <= 0:
                self.pump(10.0); continue
            self.c.send_data(sid, data[off:off + win]); off += win
            self.flush()
        if end: self.c.end_stream(sid); self.flush()
        return True

def respond(cn, sid, status, headers, body=b'', end=True):
    h = [(':status', str(status))] + headers
    cn.c.send_headers(sid, h, end_stream=(end and not body)); cn.flush()
    if body: cn.send_body(sid, body, end)

def serve_request(cn, sid, hd):
    path = hd.get(':path', '/')
    print('REQ', path, flush=True)
    if path.startswith('/big/'):
        n = int(path.split('/')[2]); respond(cn, sid, 200, [('content-type', 'application/octet-stream'), ('content-length', str(n))], block(0, n))
    elif path == '/small':
        respond(cn, sid, 200, [('content-type', 'text/plain'), ('content-length', '9')], b'hello h2\n')
    elif path == '/slowbody':
        cn.c.send_headers(sid, [(':status', '200'), ('content-type', 'application/octet-stream'), ('content-length', '12000')]); cn.flush()
        for i in range(12):
            if not cn.send_body(sid, block(i * 1000, 1000), end=(i == 11)): return
            end = time.time() + 1.0
            while time.time() < end and not cn.closed: cn.pump(max(0.05, end - time.time()))
    elif path == '/stall':
        cn.c.send_headers(sid, [(':status', '200'), ('content-length', '100')]); cn.flush()
        end = time.time() + 45
        while time.time() < end and not cn.closed: cn.pump(1.0)
    elif path == '/early':
        cn.c.send_headers(sid, [(':status', '103'), ('link', '</style.css>; rel=preload')]); cn.flush()
        respond(cn, sid, 200, [('content-type', 'text/plain'), ('content-length', '10')], b'after 103\n')
    elif path == '/redirect':
        respond(cn, sid, 302, [('location', '/small'), ('content-length', '0')])
    elif path == '/rst':
        cn.c.send_headers(sid, [(':status', '200'), ('content-length', '1000')]); cn.flush()
        cn.c.reset_stream(sid, error_code=2); cn.flush()
    elif path == '/goaway':
        cn.c.send_headers(sid, [(':status', '200'), ('content-length', '100000')]); cn.flush()
        cn.send_body(sid, block(0, 30000), end=False)
        cn.c.close_connection(error_code=0, last_stream_id=sid); cn.flush()
        cn.sock.shutdown(socket.SHUT_RDWR); cn.closed = True
    elif path == '/gzip':
        raw = gzip.compress(block(0, 200000))
        respond(cn, sid, 200, [('content-encoding', 'gzip'), ('content-type', 'application/octet-stream'), ('content-length', str(len(raw)))], raw)
    elif path == '/multi':
        hs = [('content-type', 'text/plain'), ('x-big', 'b' * 6000)] + [('set-cookie', 'c%d=v%d; Path=/' % (i, i)) for i in range(12)]
        respond(cn, sid, 200, hs + [('content-length', '6')], b'multi\n')
    elif path == '/trailers':
        cn.c.send_headers(sid, [(':status', '200'), ('content-type', 'text/plain')]); cn.flush()
        cn.send_body(sid, block(0, 5000), end=False)
        cn.c.send_headers(sid, [('x-trailer', 'yes')], end_stream=True); cn.flush()
    elif path == '/empty':
        respond(cn, sid, 204, [])
    elif path == '/missing':
        respond(cn, sid, 404, [('content-type', 'text/plain'), ('content-length', '10')], b'not found\n')
    elif path == '/huge':
        n = 12 << 20
        cn.c.send_headers(sid, [(':status', '200'), ('content-length', str(n))]); cn.flush()
        sent = 0
        while sent < n and cn.send_body(sid, block(sent, 65536), end=False): sent += 65536
        if sent >= n: cn.c.end_stream(sid); cn.flush()
    elif path == '/settings':
        end = time.time() + 5
        while not cn.ack_seen and time.time() < end and not cn.closed: cn.pump(0.2)
        respond(cn, sid, 200, [('content-length', '3')], b'ok\n' if cn.ack_seen else b'no\n')
    else:
        respond(cn, sid, 404, [('content-length', '0')])

def handle(sock, addr):
    try:
        proto = sock.selected_alpn_protocol()
        if proto != 'h2':
            sock.settimeout(10)
            req = b''
            while b'\r\n\r\n' not in req:
                d = sock.recv(4096)
                if not d: return
                req += d
            print('H1 fallback', req.split(b'\r\n')[0], flush=True)
            if b'/stall' in req.split(b'\r\n')[0] or b'/rst' in req.split(b'\r\n')[0] or b'/goaway' in req.split(b'\r\n')[0]:
                body = b'h1 fallback\n'
            else:
                body = b'h1 fallback\n'
            sock.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n' % len(body) + body)
            return
        cn = Conn(sock)
        cn.c.initiate_connection(); cn.flush()
        while not cn.closed:
            if cn.pending:
                sid, hd = cn.pending.pop(0)
                serve_request(cn, sid, hd)
            else:
                cn.pump(30.0)
    except (OSError, ssl.SSLError, h2.exceptions.H2Error) as e:
        print('conn error:', type(e).__name__, e, flush=True)
    finally:
        try: sock.close()
        except OSError: pass

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(CERT, KEY)
ctx.set_alpn_protocols(['h2', 'http/1.1'])
ctx.minimum_version = ssl.TLSVersion.TLSv1_2
ctx.maximum_version = ssl.TLSVersion.TLSv1_2          # the kernel's BearSSL build speaks TLS 1.0-1.2
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('0.0.0.0', PORT)); srv.listen(16)
print('h2 server listening', PORT, flush=True)
while True:
    c, a = srv.accept()
    try:
        s = ctx.wrap_socket(c, server_side=True)
    except (ssl.SSLError, OSError) as e:
        print('tls handshake failed:', e, flush=True); c.close(); continue
    threading.Thread(target=handle, args=(s, a), daemon=True).start()

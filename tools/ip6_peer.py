#!/usr/bin/env python3
"""A tiny IPv6 neighbour for the C-OS network selftest (VALIDATION_NET=2), sitting on the other end of a QEMU socket
netdev (frames length-prefixed, big endian). It is deliberately independent of the kernel's code, so it also checks the
checksums and header formats:

  - answers Router Solicitation with a Router Advertisement (prefix fec0::/64, SLAAC), Neighbour Solicitation with a
    Neighbour Advertisement, ICMPv6 echo requests with echo replies
  - TCP on port 18082: "DOWN <n>" sends n pattern bytes, "UP <n>" receives n bytes and answers "OK"/"BAD"
    (go-back-N sender with a retransmit timer, so frames the guest drops are recovered)

usage: ip6_peer.py [listen_port]        QEMU:  -netdev socket,id=n1,connect=127.0.0.1:<listen_port>
"""
import socket, struct, sys, time, select, random

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 55556
DUP = len(sys.argv) > 2 and sys.argv[2] == 'dup'      # answer duplicate-address-detection probes as if the guest's address were taken
MAC = bytes.fromhex('5254 00aa bb01'.replace(' ', ''))
LL = bytes.fromhex('fe80000000000000') + bytes([0, 0, 0, 0, 0, 0, 0, 2])
GA = bytes.fromhex('fec0000000000000') + bytes([0, 0, 0, 0, 0, 0, 0, 2])
SVC_PORT = 18082
MSS = 1440

def pat(i): return (i * 131 + (i >> 8) * 7) & 0xFF
def block(start, n): return bytes(pat(start + k) for k in range(n))

def csum(src, dst, nh, payload):
    data = src + dst + struct.pack('>I', len(payload)) + b'\0\0\0' + bytes([nh]) + payload
    if len(data) & 1: data += b'\0'
    s = sum(struct.unpack('>%dH' % (len(data) // 2), data))
    while s >> 16: s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF

def ip6pkt(src, dst, nh, payload, hop=64):
    return b'\x60\0\0\0' + struct.pack('>HBB', len(payload), nh, hop) + src + dst + payload

def eth(dst_mac, payload): return dst_mac + MAC + b'\x86\xdd' + payload

def icmp6(src, dst, typ, body, hop=255):
    m = bytes([typ, 0, 0, 0]) + body
    c = csum(src, dst, 58, m)
    return ip6pkt(src, dst, 58, m[:2] + struct.pack('>H', c) + m[4:], hop)

class Peer:
    def __init__(self, sock):
        self.s = sock; self.buf = b''; self.macs = {}; self.conns = {}; self.lossy = 0
    def send(self, frame):
        self.s.sendall(struct.pack('>I', len(frame)) + frame)
    def mac_of(self, ip, fallback): return self.macs.get(ip, fallback)

    # ---- frames
    def on_frame(self, f):
        if len(f) < 54 or f[12:14] != b'\x86\xdd': return
        smac = f[6:12]; p = f[14:]
        if p[0] >> 4 != 6: return
        nh = p[6]; src = p[8:24]; dst = p[24:40]; pl = p[40:40 + struct.unpack('>H', p[4:6])[0]]
        self.macs[src] = smac
        if nh == 58: self.on_icmp6(src, dst, pl, smac, p[7])
        elif nh == 6: self.on_tcp(src, dst, pl, smac)
        elif nh == 17: self.on_udp(src, dst, pl, smac)

    def on_icmp6(self, src, dst, m, smac, hop):
        if csum(src, dst, 58, m) != 0: print('bad icmp6 checksum', flush=True); return
        t = m[0]
        if t == 133:      # RS -> RA with a /64 prefix (L, A) and our link-layer address
            body = bytes([64, 0]) + struct.pack('>HII', 1800, 0, 0)
            body += bytes([1, 1]) + MAC
            body += bytes([3, 4, 64, 0xC0]) + struct.pack('>II', 86400, 14400) + b'\0\0\0\0' + GA[:8] + b'\0' * 8
            body += bytes([25, 3, 0, 0]) + struct.pack('>I', 1800) + GA          # RDNSS: we are the DNS server too
            self.send(eth(smac, icmp6(LL, src, 134, body)))
        elif t == 135:    # NS -> NA
            target = m[8:24]
            if src == bytes(16):
                print('DAD NS seen', flush=True)
                if DUP:
                    body = struct.pack('>I', 0x20000000) + target + bytes([2, 1]) + MAC
                    allnodes = b'\xff\x02' + bytes(13) + b'\x01'
                    self.send(b'\x33\x33\x00\x00\x00\x01' + MAC + b'\x86\xdd' + icmp6(target, allnodes, 136, body))
                return
            if target in (LL, GA):
                body = struct.pack('>I', 0x60000000) + target + bytes([2, 1]) + MAC
                self.send(eth(smac, icmp6(target, src, 136, body)))
        elif t == 128:    # echo
            self.send(eth(smac, icmp6(dst if dst in (LL, GA) else GA, src, 129, m[4:], 64)))

    # ---- UDP: a DNS server that knows one name, ipv6test.cos (AAAA -> fec0::2, A -> nothing)
    def on_udp(self, src, dst, u, smac):
        sport, dport, ln = struct.unpack('>HHH', u[:6])
        if csum(src, dst, 17, u[:ln]) != 0: print('bad udp checksum', flush=True); return
        if dport == 7:                                  # UDP echo
            pkt = struct.pack('>HHHH', 7, sport, ln, 0) + u[8:ln]
            cs = csum(GA, src, 17, pkt) or 0xFFFF
            self.send(eth(smac, ip6pkt(GA, src, 17, pkt[:6] + struct.pack('>H', cs) + pkt[8:])))
            return
        if dport != 53: return
        q = u[8:ln]
        tid, flags, qd = struct.unpack('>HHH', q[:6]); i = 12; labels = []
        while q[i]: labels.append(q[i + 1:i + 1 + q[i]].decode()); i += 1 + q[i]
        qtype = struct.unpack('>H', q[i + 1:i + 3])[0]; question = q[12:i + 5]
        name = '.'.join(labels); print('DNS', name, qtype, flush=True)
        if name == 'ipv6test.cos' and qtype == 28:
            ans = b'\xc0\x0c' + struct.pack('>HHIH', 28, 1, 300, 16) + GA
            resp = struct.pack('>HHHHHH', tid, 0x8180, 1, 1, 0, 0) + question + ans
        else:
            resp = struct.pack('>HHHHHH', tid, 0x8183 if name != 'ipv6test.cos' else 0x8180, 1, 0, 0, 0) + question
        pkt = struct.pack('>HHHH', 53, sport, 8 + len(resp), 0) + resp
        cs = csum(GA, src, 17, pkt) or 0xFFFF
        pkt = pkt[:6] + struct.pack('>H', cs) + pkt[8:]
        self.send(eth(smac, ip6pkt(GA, src, 17, pkt)))

    # ---- TCP
    def seg(self, c, flags, seq, data=b'', opts=b''):
        hdr = struct.pack('>HHIIBBHHH', SVC_PORT, c['gport'], seq & 0xFFFFFFFF, c['rcv_nxt'] & 0xFFFFFFFF, ((20 + len(opts)) // 4) << 4, flags, 65535, 0, 0) + opts
        seg = hdr + data
        cs = csum(GA, c['src'], 6, seg)
        seg = seg[:16] + struct.pack('>H', cs) + seg[18:]
        self.send(eth(self.mac_of(c['src'], c['smac']), ip6pkt(GA, c['src'], 6, seg)))

    def on_tcp(self, src, dst, t, smac):
        if csum(src, dst, 6, t) != 0: print('bad tcp checksum', flush=True); return
        gport, dport, seq, ack, off, flags, win = struct.unpack('>HHIIBBH', t[:16])
        off = (off >> 4) * 4; data = t[off:]
        if dport != SVC_PORT: return
        key = (src, gport)
        if flags & 0x02:                                    # SYN
            c = self.conns.get(key)
            if not c or c['state'] == 'closed':
                c = dict(src=src, smac=smac, gport=gport, rcv_nxt=seq + 1, iss=random.getrandbits(32), state='syn', inbuf=b'', cmd=None,
                         got=0, ok=True, snd_una=0, snd_nxt=0, total=0, last_tx=0, fin_sent=False, want=0, mss=MSS)
                c['snd_una'] = c['snd_nxt'] = c['iss'] + 1
                self.conns[key] = c
            self.seg(c, 0x12, c['iss'], opts=bytes([2, 4]) + struct.pack('>H', MSS))
            return
        c = self.conns.get(key)
        if not c: return
        if flags & 0x10:                                    # ACK
            if c['state'] == 'syn': c['state'] = 'est'
            if ((ack - c['snd_una']) & 0xFFFFFFFF) < 0x80000000 and ack != c['snd_una'] and ((c['snd_nxt'] - ack) & 0xFFFFFFFF) < 0x80000000:
                c['snd_una'] = ack
                self.pump(c)
        if data:
            if seq == c['rcv_nxt']:
                c['rcv_nxt'] += len(data); self.on_data(c, data)
            self.seg(c, 0x10, c['snd_nxt'])                 # ACK (a duplicate one when out of order)
        if flags & 0x01 and seq + len(data) == c['rcv_nxt']:
            c['rcv_nxt'] += 1
            self.seg(c, 0x10, c['snd_nxt'])
            c['state'] = 'closed' if c['fin_sent'] else 'closing'

    def on_data(self, c, data):
        if c['cmd'] is None and c['inbuf'] == b'' and data.startswith(b'GET '):
            c['cmd'] = 'HTTP'; c['inbuf'] = data
            body = block(0, 20000)
            hdr = b'HTTP/1.1 200 OK\r\nContent-Length: %d\r\nContent-Type: application/octet-stream\r\nConnection: close\r\n\r\n' % len(body)
            c['http'] = hdr + body; c['total'] = len(c['http']); c['base'] = c['snd_una']
            self.pump(c)
            return
        if c['cmd'] is None:
            c['inbuf'] += data
            if b'\n' in c['inbuf']:
                line, rest = c['inbuf'].split(b'\n', 1)
                verb, n = line.split(); c['cmd'] = verb.decode(); c['want'] = int(n)
                if c['cmd'] == 'DOWN':
                    c['total'] = c['want']; c['base'] = c['snd_una']; self.pump(c)
                else:
                    self.on_data(c, rest) if rest else None
            return
        if c['cmd'] == 'UP':
            n = min(len(data), c['want'] - c['got'])
            if data[:n] != block(c['got'], n): c['ok'] = False
            c['got'] += n
            if c['got'] == c['want'] and not c['fin_sent']:
                self.seg(c, 0x18, c['snd_nxt'], b'OK\n' if c['ok'] else b'BAD\n')
                c['snd_nxt'] += 3; c['fin_sent'] = True
                self.seg(c, 0x11, c['snd_nxt']); c['snd_nxt'] += 1

    def pump(self, c):
        if c['cmd'] not in ('DOWN', 'HTTP'): return
        sent_end = c['snd_nxt'] - c['base']
        while sent_end < c['total'] and (c['snd_nxt'] - c['snd_una']) < 8 * MSS:
            n = min(MSS, c['total'] - sent_end)
            self.seg(c, 0x18, c['snd_nxt'], c['http'][sent_end:sent_end + n] if c['cmd'] == 'HTTP' else block(sent_end, n))
            c['snd_nxt'] += n; sent_end += n; c['last_tx'] = time.time()
        if sent_end >= c['total'] and not c['fin_sent'] and c['snd_una'] == c['snd_nxt']:
            self.seg(c, 0x11, c['snd_nxt']); c['snd_nxt'] += 1; c['fin_sent'] = True
            print('DOWN', c['total'], 'sent', flush=True)

    def tick(self):
        now = time.time()
        for c in list(self.conns.values()):
            if c['cmd'] in ('DOWN', 'HTTP') and c['snd_una'] != c['snd_nxt'] and now - c['last_tx'] > 0.4 and not c['fin_sent']:
                off = c['snd_una'] - c['base']                   # go back to the oldest unacknowledged byte
                c['snd_nxt'] = c['snd_una']; c['last_tx'] = now; self.pump(c)

    def run(self):
        while True:
            r, _, _ = select.select([self.s], [], [], 0.05)
            if r:
                d = self.s.recv(65536)
                if not d: return
                self.buf += d
                while len(self.buf) >= 4:
                    n = struct.unpack('>I', self.buf[:4])[0]
                    if len(self.buf) < 4 + n: break
                    self.on_frame(self.buf[4:4 + n]); self.buf = self.buf[4 + n:]
            self.tick()

srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('127.0.0.1', PORT)); srv.listen(1)
print('listening', PORT, flush=True)
while True:
    conn, _ = srv.accept()
    print('qemu connected', flush=True)
    try: Peer(conn).run()
    except Exception as e: print('peer error', repr(e), flush=True)

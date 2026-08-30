import json
import socket
import sys
import time

QMP = "/tmp/cos_gui_qmp.sock"
URL = sys.argv[1] if len(sys.argv) > 1 else "http://example.com/"

PUNCT = {
    ':': ('semicolon', True),
    '%': ('5', True),
    ',': ('comma', False),
    '.': ('dot', False),
    '/': ('slash', False),
    '_': ('minus', True),
    '-': ('minus', False),
    '=': ('equal', False),
    '?': ('slash', True),
    '&': ('7', True),
    ';': ('semicolon', False),
}


def event(qcode, down):
    return {'type': 'key', 'data': {'key': {'type': 'qcode', 'data': qcode}, 'down': down}}


def recv_response(sock):
    while True:
        line = b''
        while not line.endswith(b'\n'):
            chunk = sock.recv(4096)
            if not chunk:
                raise RuntimeError('QMP disconnected')
            line += chunk
        response = json.loads(line.decode())
        if 'return' in response:
            return response
        if 'error' in response:
            raise RuntimeError(response['error'])


def send(sock, payload):
    sock.sendall((json.dumps(payload) + '\n').encode())
    return recv_response(sock)


def send_key(sock, qcode, shift=False, ctrl=False):
    events = []
    if ctrl:
        events.append(event('ctrl', True))
    if shift:
        events.append(event('shift', True))
    events.extend([event(qcode, True), event(qcode, False)])
    if shift:
        events.append(event('shift', False))
    if ctrl:
        events.append(event('ctrl', False))
    send(sock, {'execute': 'input-send-event', 'arguments': {'events': events}})
    time.sleep(0.025)


def encode_char(ch):
    if 'a' <= ch <= 'z' or '0' <= ch <= '9':
        return ch, False
    if 'A' <= ch <= 'Z':
        return ch.lower(), True
    if ch in PUNCT:
        return PUNCT[ch]
    raise ValueError(f'Unsupported QMP character: {ch!r}')


with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
    sock.connect(QMP)
    recv_response(sock)
    send(sock, {'execute': 'qmp_capabilities'})
    send_key(sock, 'b', ctrl=True)
    time.sleep(1.0)
    send_key(sock, 'l', ctrl=True)
    for ch in URL:
        qcode, shift = encode_char(ch)
        send_key(sock, qcode, shift=shift)
    send_key(sock, 'ret')

print(URL)

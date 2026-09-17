import json
import socket
import sys
import time

QMP = "/tmp/cos_gui_qmp.sock"
TEXT = sys.argv[1] if len(sys.argv) > 1 else "data:text/html,%3Cscript%3Edocument.body.innerHTML%3D%27JS_OK%27%3C%2Fscript%3E"

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
    '(': ('9', True),
    ')': ('0', True),
    "'": ('apostrophe', False),
    '"': ('apostrophe', True),
    '+': ('equal', True),
    '*': ('8', True),
    '!': ('1', True),
    '<': ('comma', True),
    '>': ('dot', True),
}

def event(qcode, down):
    return {'type': 'key', 'data': {'key': {'type': 'qcode', 'data': qcode}, 'down': down}}

def send(sock, payload):
    sock.sendall((json.dumps(payload) + '\n').encode())
    while True:
        line = b''
        while not line.endswith(b'\n'):
            chunk = sock.recv(4096)
            if not chunk:
                raise RuntimeError('QMP disconnected')
            line += chunk
        response = json.loads(line.decode())
        if 'return' in response or 'error' in response:
            if 'error' in response:
                raise RuntimeError(response['error'])
            return response

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
    # The C-OS GUI consumes injected key events on its normal frame cadence.
    # A 45 ms interval prevents long data: URLs from losing punctuation while
    # retaining a practical control-page turnaround time.
    time.sleep(0.045)

def encode_char(ch):
    if 'a' <= ch <= 'z' or '0' <= ch <= '9':
        return ch, False
    if 'A' <= ch <= 'Z':
        return ch.lower(), True
    if ch in PUNCT:
        return PUNCT[ch]
    raise ValueError(f'Unsupported character for QMP keyboard injection: {ch!r}')

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
    s.connect(QMP)
    s.recv(4096)
    send(s, {'execute': 'qmp_capabilities'})
    send_key(s, 'l', ctrl=True)
    for character in TEXT:
        qcode, shift = encode_char(character)
        send_key(s, qcode, shift=shift)
    send_key(s, 'ret')

print(TEXT)

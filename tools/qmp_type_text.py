#!/usr/bin/env python3
"""Send text and an optional Enter key to the focused C-OS GUI control via QMP."""
import json
import socket
import sys
import time

import os
QMP = os.environ.get("COS_QMP", "/tmp/cos_gui_qmp.sock")
TEXT = sys.argv[1] if len(sys.argv) > 1 else "C-OS NetSurf"
PRESS_ENTER = "--no-enter" not in sys.argv[2:]
PUNCT = {
    ':': ('semicolon', True), '%': ('5', True), ',': ('comma', False),
    '.': ('dot', False), '/': ('slash', False), '_': ('minus', True),
    '-': ('minus', False), '=': ('equal', False), '?': ('slash', True),
    '&': ('7', True), ';': ('semicolon', False), '(': ('9', True),
    ')': ('0', True), "'": ('apostrophe', False), '"': ('apostrophe', True),
    '+': ('equal', True), '*': ('8', True), '!': ('1', True),
    '<': ('comma', True), '>': ('dot', True), ' ': ('spc', False),
    '{': ('bracket_left', True), '}': ('bracket_right', True), '[': ('bracket_left', False), ']': ('bracket_right', False),
    '|': ('backslash', True), '\\': ('backslash', False), '#': ('3', True), '$': ('4', True), '@': ('2', True), '^': ('6', True),
    '~': ('grave_accent', True), '`': ('grave_accent', False),
}

def event(qcode, down):
    return {'type': 'key', 'data': {'key': {'type': 'qcode', 'data': qcode}, 'down': down}}

def send(sock, payload):
    sock.sendall((json.dumps(payload) + '\n').encode())
    buf = b''
    while True:
        while b'\n' not in buf:                       # QMP also sends asynchronous events: take the answer line by line
            chunk = sock.recv(4096)
            if not chunk:
                raise RuntimeError('QMP disconnected')
            buf += chunk
        line, buf = buf.split(b'\n', 1)
        if not line.strip():
            continue
        response = json.loads(line.decode())
        if 'return' in response:
            return response
        if 'error' in response:
            raise RuntimeError(response['error'])

def send_key(sock, qcode, shift=False):
    events = []
    if shift:
        events.append(event('shift', True))
    events.extend([event(qcode, True), event(qcode, False)])
    if shift:
        events.append(event('shift', False))
    send(sock, {'execute': 'input-send-event', 'arguments': {'events': events}})
    time.sleep(0.045)

def encode_char(ch):
    if 'a' <= ch <= 'z' or '0' <= ch <= '9':
        return ch, False
    if 'A' <= ch <= 'Z':
        return ch.lower(), True
    if ch in PUNCT:
        return PUNCT[ch]
    raise ValueError(f'Unsupported character: {ch!r}')

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
    sock.connect(QMP)
    sock.recv(4096)
    send(sock, {'execute': 'qmp_capabilities'})
    for character in TEXT:
        qcode, shift = encode_char(character)
        send_key(sock, qcode, shift)
    if PRESS_ENTER:
        send_key(sock, 'ret')

print(TEXT)

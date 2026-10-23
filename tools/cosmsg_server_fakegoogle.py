#!/usr/bin/env python3
"""TEST ONLY: cosmsg_server.py with a fake Google that approves every sign-in 12 seconds after it started.
It exists so the C-OS Message sign-in screens can be driven in QEMU without a real Google client id. Never use it for real."""
import asyncio, os, sys, time
sys.path.insert(0, os.path.dirname(__file__))
import cosmsg_server as cs

class FakeGoogle:
    def __init__(self): self.n, self.t0 = 0, {}
    def start(self):
        self.n += 1; dev = "fake%d" % self.n; self.t0[dev] = time.time()
        return {"device_code": dev, "user_code": "WXYZ-1234", "url": "https://www.google.com/device", "expires_in": 600, "interval": 3}
    def poll(self, dev):
        if time.time() - self.t0[dev] < 12: return "pending"
        n = dev[4:]
        return {"sub": "fake-sub-" + n, "email": "person%s@gmail.com" % n}

_orig = cs.Server.__init__
def init(self, dbpath, google=None, require_google=False): _orig(self, dbpath, FakeGoogle(), True)
cs.Server.__init__ = init
asyncio.run(cs.main())

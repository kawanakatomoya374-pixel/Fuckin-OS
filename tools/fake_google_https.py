#!/usr/bin/env python3
"""TEST ONLY: a fake Google over HTTPS for cosmsgd (the C-OS relay server). Speaks the three endpoints the device flow uses:
  POST /device/code   POST /token   GET /oauth2/v3/certs
and signs ID tokens with a real RSA key, so the server's RS256 verification is exercised for real. Test control (never in Google):
  POST /_script  dev=<device_code>&state=pending|denied|expired|slow_down|ok|badsig|badaud|expired_token&sub=..&email=..[&verified=0]
Usage: fake_google_https.py PORT CERT_OUT CLIENT_ID CLIENT_SECRET   (writes a self-signed RSA certificate for localhost to CERT_OUT as DER)"""
import base64, datetime, json, ssl, sys, tempfile, threading, time, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from cryptography import x509
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa, padding

port, cert_out, CID, SECRET = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4]
b64 = lambda b: base64.urlsafe_b64encode(b).rstrip(b"=").decode()
tls_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "localhost")])
now = datetime.datetime.now(datetime.timezone.utc)
cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(tls_key.public_key()).serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1)).not_valid_after(now + datetime.timedelta(days=30))
        .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
        .add_extension(x509.SubjectAlternativeName([x509.DNSName("localhost"), x509.DNSName("192.168.70.2")]), critical=False).sign(tls_key, hashes.SHA256()))
open(cert_out, "wb").write(cert.public_bytes(serialization.Encoding.DER))
d = tempfile.mkdtemp(); open(d + "/c.pem", "wb").write(cert.public_bytes(serialization.Encoding.PEM))
open(d + "/k.pem", "wb").write(tls_key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL, serialization.NoEncryption()))
SIGN = rsa.generate_private_key(public_exponent=65537, key_size=2048); OTHER = rsa.generate_private_key(public_exponent=65537, key_size=2048)
NUM = SIGN.public_key().public_numbers()
JWKS = {"keys": [{"kty": "RSA", "kid": "k1", "alg": "RS256", "use": "sig", "n": b64(NUM.n.to_bytes(256, "big")), "e": b64(NUM.e.to_bytes(3, "big"))}]}
devices = {}; counter = [0]; lock = threading.Lock()

def token(claims, key=SIGN, kid="k1"):
    h = b64(json.dumps({"alg": "RS256", "kid": kid, "typ": "JWT"}).encode()); p = b64(json.dumps(claims).encode())
    return h + "." + p + "." + b64(key.sign((h + "." + p).encode(), padding.PKCS1v15(), hashes.SHA256()))

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    def log_message(self, *a): pass
    def send(self, code, obj):
        body = json.dumps(obj).encode(); self.send_response(code); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def form(self):
        n = int(self.headers.get("Content-Length", 0)); return {k: v[0] for k, v in urllib.parse.parse_qs(self.rfile.read(n).decode()).items()}
    def do_GET(self):
        if self.path == "/oauth2/v3/certs": self.send(200, JWKS)
        else: self.send(404, {})
    def do_POST(self):
        f = self.form()
        with lock:
            if self.path == "/device/code":
                if f.get("client_id") != CID or "openid" not in f.get("scope", ""): return self.send(400, {"error": "invalid_client"})
                counter[0] += 1; dev = "dev%d" % counter[0]; devices[dev] = {"state": "pending"}
                return self.send(200, {"device_code": dev, "user_code": "ABCD-EFGH", "verification_url": "https://www.google.com/device", "expires_in": 600, "interval": 3})
            if self.path == "/_script":
                devices.setdefault(f["dev"], {}).update(f); return self.send(200, {})
            if self.path == "/token":
                if f.get("client_id") != CID or f.get("client_secret") != SECRET: return self.send(401, {"error": "invalid_client"})
                if f.get("grant_type") != "urn:ietf:params:oauth:grant-type:device_code": return self.send(400, {"error": "unsupported_grant_type"})
                dv = devices.get(f.get("device_code")); 
                if not dv: return self.send(400, {"error": "invalid_grant"})
                st = dv.get("state", "pending")
                if st in ("pending", "slow_down"): return self.send(428 if st == "pending" else 400, {"error": "authorization_pending" if st == "pending" else "slow_down"})
                if st == "denied": return self.send(403, {"error": "access_denied"})
                if st == "expired_token": return self.send(400, {"error": "expired_token"})
                t = int(time.time()); claims = {"iss": "https://accounts.google.com", "aud": CID, "sub": dv.get("sub", "1100"), "email": dv.get("email", "taro@gmail.com"),
                                                "email_verified": dv.get("verified", "1") != "0", "iat": t, "exp": t + 3600}
                if st == "badaud": claims["aud"] = "evil.apps.googleusercontent.com"
                if st == "expired": claims["exp"] = t - 5
                return self.send(200, {"access_token": "x", "token_type": "Bearer", "id_token": token(claims, OTHER if st == "badsig" else SIGN)})
        self.send(404, {})

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(d + "/c.pem", d + "/k.pem")
srv = ThreadingHTTPServer(("0.0.0.0", port), H); srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
print("fake google ready", flush=True); srv.serve_forever()

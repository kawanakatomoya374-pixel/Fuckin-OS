"""Google sign-in for the C-OS Message relay server (OAuth 2.0 device flow + OpenID Connect ID token checks). Stdlib only.

Why the device flow: the C-OS client has no browser the user can trust with a Google password, and it must never see that
password. The user opens https://www.google.com/device on their phone or PC, types a short code, and signs in at Google.
The relay server asks Google for the result, so neither the password nor any Google token ever touches the C-OS client.

What is verified (OpenID Connect Core 3.1.3.7):
  * the ID token is an RS256 JWT whose signature checks out against Google's published keys (JWKS), matched by `kid`
    ("alg": "none" and every other algorithm are refused);
  * iss is Google, aud is OUR client id, exp is in the future (iat is not from the future), email_verified is true;
  * the e-mail address ends in one of the allowed domains (default gmail.com / googlemail.com).
The stable `sub` claim, not the e-mail address, identifies the person (an e-mail address can be reassigned).
"""
import base64, hashlib, json, time, urllib.error, urllib.parse, urllib.request

GOOGLE_DEVICE_URL = "https://oauth2.googleapis.com/device/code"
GOOGLE_TOKEN_URL = "https://oauth2.googleapis.com/token"
GOOGLE_JWKS_URL = "https://www.googleapis.com/oauth2/v3/certs"
ISSUERS = ("https://accounts.google.com", "accounts.google.com")
# DigestInfo prefix of a SHA-256 PKCS#1 v1.5 signature (RFC 8017, 9.2)
SHA256_PREFIX = bytes.fromhex("3031300d060960864801650304020105000420")


class GoogleError(Exception):
    pass


def b64url(s):
    if isinstance(s, str): s = s.encode()
    return base64.urlsafe_b64decode(s + b"=" * (-len(s) % 4))


def _int(s):
    return int.from_bytes(b64url(s), "big")


def rs256_verify(signing_input, signature, n, e):
    """RSASSA-PKCS1-v1_5 with SHA-256, checked by hand (no third-party crypto needed)."""
    k = (n.bit_length() + 7) // 8
    if len(signature) != k: return False
    m = pow(int.from_bytes(signature, "big"), e, n)
    em = m.to_bytes(k, "big")
    t = SHA256_PREFIX + hashlib.sha256(signing_input).digest()
    expected = b"\x00\x01" + b"\xff" * (k - len(t) - 3) + b"\x00" + t
    return len(em) == len(expected) and _consteq(em, expected)


def _consteq(a, b):
    r = 0
    for x, y in zip(a, b): r |= x ^ y
    return r == 0


def verify_id_token(token, jwks, client_id, now=None, domains=("gmail.com", "googlemail.com")):
    """Returns the claims of a valid Google ID token or raises GoogleError."""
    now = int(now if now is not None else time.time())
    try:
        h64, p64, s64 = token.split(".")
        header, claims, sig = json.loads(b64url(h64)), json.loads(b64url(p64)), b64url(s64)
    except Exception:
        raise GoogleError("malformed token")
    if header.get("alg") != "RS256":
        raise GoogleError("unsupported signature algorithm")
    key = next((k for k in jwks.get("keys", []) if k.get("kid") == header.get("kid") and k.get("kty") == "RSA"), None)
    if not key: raise GoogleError("unknown signing key")
    if not rs256_verify((h64 + "." + p64).encode(), sig, _int(key["n"]), _int(key["e"])):
        raise GoogleError("bad signature")
    if claims.get("iss") not in ISSUERS: raise GoogleError("wrong issuer")
    aud = claims.get("aud")
    if aud != client_id and not (isinstance(aud, list) and client_id in aud): raise GoogleError("wrong audience")
    if int(claims.get("exp", 0)) <= now: raise GoogleError("token expired")
    if int(claims.get("iat", 0)) > now + 300: raise GoogleError("token issued in the future")
    if not claims.get("sub"): raise GoogleError("no subject")
    email = str(claims.get("email", "")).lower()
    if claims.get("email_verified") not in (True, "true"): raise GoogleError("e-mail address not verified")
    if "@" not in email or email.rsplit("@", 1)[1] not in domains:
        raise GoogleError("only %s addresses can sign in" % " / ".join(domains))
    return {"sub": str(claims["sub"]), "email": email}


def mask_email(email):
    local, _, domain = email.partition("@")
    return (local[:1] + "***@" + domain) if local else "***@" + domain


class GoogleBackend:
    """Talks to Google. Blocking calls: run them in a thread."""

    def __init__(self, client_id, client_secret, domains=("gmail.com", "googlemail.com")):
        self.client_id, self.client_secret, self.domains = client_id, client_secret, tuple(domains)
        self._jwks, self._jwks_at = None, 0

    def _post(self, url, data):
        req = urllib.request.Request(url, urllib.parse.urlencode(data).encode(), {"Content-Type": "application/x-www-form-urlencoded"})
        try:
            with urllib.request.urlopen(req, timeout=10) as r: return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            try: return e.code, json.loads(e.read())
            except Exception: return e.code, {}
        except Exception as e:
            raise GoogleError("cannot reach Google (%s)" % e)

    def keys(self, force=False):
        if force or not self._jwks or time.time() - self._jwks_at > 3600:
            try:
                with urllib.request.urlopen(GOOGLE_JWKS_URL, timeout=10) as r: self._jwks = json.loads(r.read())
            except Exception as e:
                if not self._jwks: raise GoogleError("cannot fetch Google's keys (%s)" % e)
            self._jwks_at = time.time()
        return self._jwks

    def start(self):
        st, d = self._post(GOOGLE_DEVICE_URL, {"client_id": self.client_id, "scope": "openid email"})
        if st != 200 or "device_code" not in d: raise GoogleError("Google refused to start a sign-in (%s)" % d.get("error", st))
        return {"device_code": d["device_code"], "user_code": d["user_code"], "url": d.get("verification_url", "https://www.google.com/device"),
                "expires_in": int(d.get("expires_in", 600)), "interval": int(d.get("interval", 5))}

    def poll(self, device_code):
        """'pending', 'slow_down', 'denied', 'expired' or the verified claims {'sub','email'}."""
        st, d = self._post(GOOGLE_TOKEN_URL, {"client_id": self.client_id, "client_secret": self.client_secret, "device_code": device_code,
                                              "grant_type": "urn:ietf:params:oauth:grant-type:device_code"})
        err = d.get("error")
        if err == "authorization_pending": return "pending"
        if err == "slow_down": return "slow_down"
        if err == "access_denied": return "denied"
        if err in ("expired_token", "invalid_grant"): return "expired"
        if st != 200 or "id_token" not in d: raise GoogleError("Google sign-in failed (%s)" % (err or st))
        try:
            return verify_id_token(d["id_token"], self.keys(), self.client_id, domains=self.domains)
        except GoogleError as e:
            if str(e) == "unknown signing key": return verify_id_token(d["id_token"], self.keys(True), self.client_id, domains=self.domains)
            raise

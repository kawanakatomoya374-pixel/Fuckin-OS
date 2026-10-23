#!/usr/bin/env python3
"""C-OS Message relay server (stdlib only).

Lets C-OS Message users who do not know each other's IP address talk: accounts ("C-OS ID" + secret token), friend
requests that must be accepted before anyone can send a message, blocking, offline delivery and delivery receipts.

Usage:  cosmsg_server.py [--port 5151] [--db cosmsg.db] [--bind 0.0.0.0] [--cert cosmsg.crt --key cosmsg.key]

The connection is always TLS. If the certificate files do not exist a self-signed one is generated (needs the openssl
command). C-OS Message pins the certificate on first use, so a self-signed certificate is enough; to move the server
to another machine, copy the .crt/.key pair (otherwise every client must run /repin).

Wire format: one line per frame, fields separated by TAB; inside a field "\\" -> "\\\\", TAB -> "\\t", LF -> "\\n".
Client -> server                          Server -> client
  REG name                                  OK_REG id token name
  LOGIN id token                            OK_LOGIN id name | ERR text
  NAME newname                              OK_NAME name
  FIND id                                   FOUND id name | ERR text
  FREQ id intro                             OK_FREQ id | ERR text           (target gets FREQ_IN)
  FACC id / FDEC id / FDEL id               FRIEND id name / FRIEND_DEL id
  BLOCK id                                  OK_BLOCK id
  SEND msgid toid text                      SENT msgid | NOSEND msgid reason
  EDIT msgid toid text                      OK_EDIT msgid   (receiver gets EDIT_IN seq from ts msgid text)
  DEL msgid toid                            OK_DEL msgid    (receiver gets DEL_IN seq from ts msgid)
  GOT seq                                   DELIV msgid toid   (to the sender once the receiver stored it)
  FUP size name                             FUP_OK fid | FERR fid reason       start an upload
  FCH fid idx base64                        (no reply)                          chunk idx = 0,1,2,... of CHUNK bytes
  FEND fid                                  FEND_OK fid | FERR fid reason       finish it
  FGET fid                                  FDATA fid idx last base64 ... | FERR fid reason
Profiles (Instagram style): HANDLE h / BIO text / DISC 0|1 / AVATAR fid / ME / INVITE_NEW / JOIN code / SEARCH q / PROFILE id
  HANDLE h          OK_HANDLE h | ERR ...     unique lowercase @handle (a-z 0-9 _ . , 3-20 chars); empty clears it
  BIO text          OK_BIO | DISC 0|1  OK_DISC v   searchable only when DISC is 1 (default 0)
  AVATAR fid        OK_AVATAR fid | ERR      a file you uploaded (<= 256 KB) becomes your public avatar; empty removes it
  ME                ME id name handle bio disc avatar invite     your own profile and invite code
  INVITE_NEW        INVITE code               a new invite code (the old one stops working)
  JOIN code         OK_FREQ id | ERR          sends a friend request to the owner of the code
  SEARCH q          RESULT id name handle bio avatar ... RESULT_END     discoverable users by @handle prefix or name
  PROFILE id        PROFILE id name handle bio avatar   (handle/bio/avatar only for friends, discoverable users and pending requests)
  FRIEND id name handle avatar / FREQ_IN id name intro handle avatar    carry the profile fields too
Groups (joining needs a 6-digit password, stored only as a salted scrypt hash; wrong guesses lock the user and the group out for a while):
  GNEW name [pw]            OK_GNEW gid name pw | ERR      pw: 6 digits chosen by the user, or generated when empty (shown only here)
  GJOIN gid pw              OK_GJOIN gid name | ERR        (the error never says whether the group exists)
  GLEAVE gid                OK_GLEAVE gid                  the owner leaving hands the group to its oldest member
  GPASS gid [pw]            OK_GPASS gid pw | ERR          owner only; a new random password when empty
  GKICK gid uid / GRENAME gid name                         owner only
  GINFO gid                 GROUP + GMEMBER frames
  GSEND msgid gid text      GSENT msgid | NOSEND msgid reason     members receive GMSG seq gid from ts text msgid
  GEDIT msgid gid text      OK_GEDIT msgid                 (members get GEDIT_IN seq gid from ts msgid text)
  GDEL msgid gid            OK_GDEL msgid                  (members get GDEL_IN seq gid from ts msgid)
  GROLE gid uid 0|1         OK_GROLE gid uid role          owner only: make a member a sub-admin (1) or a plain member (0)
  GOWNER gid uid            OK_GOWNER gid uid              owner only: hand the group over (the old owner becomes a sub-admin)
  GANN gid [text]           OK_GANN gid                    owner/sub-admin: pin an announcement (empty clears it)
  GNOTE gid text            OK_GNOTE gid id                any member: post a note (<= 600 chars, 100 per group); everyone gets GNOTE
  GNOTES gid                GNOTE gid id author ts text ... GNOTES_END gid
  GNOTEDEL gid id           OK_GNOTEDEL gid id             the author, or an owner/sub-admin
  GKICK: the owner can remove anybody, a sub-admin only plain members. GRENAME: owner or sub-admin. GPASS: owner.
  GROUP gid name owner / GMEMBER gid uid name role (0 member, 1 sub-admin, 2 owner) / GMEMBER_DEL gid uid / GROUP_DEL gid /
  GANN gid text by ts   are pushed after login and whenever they change
Google sign-in (OAuth device flow; the user types the code at google.com/device, the password never reaches C-OS or this server):
  GREG name                 GOOGLE_CODE user_code url expires interval     starts a sign-in for a NEW device (no login needed); when the
                            Google account already has a C-OS account that account comes back with a fresh token (old tokens stop working)
  GPOLL                     GOOGLE_PENDING | OK_REG id token name + GOOGLE_OK masked-email | ERR       (ask again every `interval` seconds)
  GLINK                     links the Google account to the logged-in C-OS account (same GPOLL / GOOGLE_OK handshake)
  GSTATUS                   GOOGLE_STATUS linked masked-email required configured     (also sent after every login)
  LOGOUT_ALL                OK_LOGOUT_ALL token     new token, every other device is logged out
  With --require-google a plain REG answers NEED_GOOGLE and an account that is not linked yet can do nothing but link.
  Tokens are stored only as SHA-256 hashes; one Google account = one C-OS account.
Blocking and reporting:
  BLOCKS                    BLOCKED id name ... BLOCKS_END n          UNBLOCK id    OK_UNBLOCK id
  REPORT id reason [evidence]   OK_REPORT | ERR     stored for the operator (cosmsg_server.py --db X --reports); five different reporters
                                within a week restrict the account (no friend requests, hidden from search)
Typing and presence:
  TYPING id / GTYPING gid   (no reply)  the friend (or the group's other members) get TYPING_IN id / GTYPING_IN gid id - nothing is stored
  SETPRES 0|1               OK_SETPRES v     show (1, default) or hide (0) when you are online and your last seen time
  PRES id state ts          state 1 online, 0 offline since ts, 2 hidden; sent for every friend at login and whenever it changes
Read receipts:
  READ id upto              (no reply)  the friend's messages up to msgid `upto` have been read; the writer gets READ_IN reader upto
  GREAD gid author upto     (no reply)  the same for a group; the author gets GREAD_IN gid reader upto.  Last positions are kept and re-sent at login.
End-to-end encryption (the server only stores and hands out public keys and relays opaque text; see docs/messenger_security.md):
  KEYPUT bundle             OK_KEYPUT | ERR     your public key bundle (base64url, 197 raw bytes); replaces the old one
  KEYGET id                 KEY id bundle       a friend's, a fellow group member's or your own bundle; bundle is empty when none was published
  GKEY gid uid epoch wrapped    OK_GKEY gid uid epoch | ERR    hands one member the group key of an epoch, wrapped for that member (end-to-end);
                            the member gets GKEY_IN seq gid from ts epoch wrapped (acknowledge with GOT like a message)
  Group announcements (GANN) and notes (GNOTE) may be "\x01G..." envelopes too (opaque to the server).
  Group texts "\x01G<base64url>" are end-to-end encrypted (sender-signed) envelopes: never cut, up to MAX_E2E characters; GSEND may carry a
  5th field fid for an encrypted attachment. --require-e2e also refuses plain group texts.
  SEND/EDIT with text "\x01E<base64url>" are end-to-end encrypted envelopes: up to MAX_E2E characters, never cut (too long = NOSEND),
  and SEND takes an optional 5th field fid that grants the receiver the right to FGET that file (the announcement itself is encrypted).
  --require-e2e refuses every 1:1 text that is not an envelope (NOSEND "encryption required").
An attachment is announced to a friend by a normal message whose text is "\x01F|<fid>|<size>|<name>"; sending it
grants that friend (and only that friend) the right to FGET the file. Files live in <db>.files/.
  PING                                      PONG
Pushed after login: FRIEND.., FREQ_IN.. (pending requests), MSG seq fromid ts text msgid (undelivered events, oldest first), SYNCED.
msgid is chosen by the sender and only meaningful together with the sender's id, so a user can only edit or delete their own messages.
"""
import argparse, asyncio, base64, binascii, hashlib, hmac, os, re, secrets, sqlite3, ssl, subprocess, time
from cosmsg_google import GoogleBackend, GoogleError, mask_email

MAX_TEXT = 600
MAX_BACKLOG = 2000                  # undelivered events one user can have waiting (everything together)
MAX_BACKLOG_PAIR = 400              # ... from one single sender
MAX_GKEY_WAITING = 64               # undelivered group keys per user
KEYPUT_PER_HOUR = 12                # new key bundles one account may publish per hour (a key change is rare; this stops churn)
MAX_E2E = 3200                      # characters of one end-to-end envelope (base64url of up to 2400 bytes)
MAX_NAME = 23
MAX_BIO = 120
MAX_AVATAR = 256 * 1024             # avatar images are small (the client sends 96x96)
HANDLE_RE = re.compile(r"^[a-z0-9_.]{3,20}$")
INVITE_ALPHABET = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"      # no 0/O/1/I/L
MAX_LINE = 16384
MAX_FILE = 10 * 1024 * 1024         # one attachment
QUOTA = 300 * 1024 * 1024           # live attachment bytes per user
FILE_TTL = 14 * 86400               # attachments are deleted after two weeks
REPORT_LIMIT = 5                           # distinct reporters within a week that restrict an account
MAX_GNAME = 30
GROUP_MEMBERS = 200
GROUPS_PER_USER = 30
PW_RE = re.compile(r"^[0-9]{6}$")
USER_TRIES, USER_LOCK = 5, 600               # wrong passwords per user and group before that user is locked out for 10 minutes
GROUP_TRIES, GROUP_WINDOW, GROUP_LOCK = 40, 3600, 1800   # wrong passwords per group per hour before the group refuses everybody for 30 minutes
ANY_TRIES, ANY_LOCK = 25, 3600               # wrong passwords per user over all groups per hour
CHUNK = 6144                        # raw bytes per FCH/FDATA frame (8192 base64 characters)


def esc(s):
    return str(s).replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n")


def unesc(s):
    out, i = [], 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            out.append({"t": "\t", "n": "\n"}.get(s[i + 1], s[i + 1])); i += 2
        else:
            out.append(s[i]); i += 1
    return "".join(out)


def frame(*fields):
    return ("\t".join(esc(f) for f in fields) + "\n").encode()


VERBOSE = False


def weak_pw(pw):
    d = [int(c) for c in pw]
    steps = {d[i + 1] - d[i] for i in range(5)}
    return len(set(pw)) == 1 or steps == {1} or steps == {-1} or pw in ("123123", "121212", "112233", "012345", "654321")


def gen_pw():
    while True:
        pw = "%06d" % secrets.randbelow(1_000_000)
        if not weak_pw(pw):
            return pw


def hash_pw(pw, salt=None):
    salt = salt or secrets.token_bytes(16)
    return salt.hex() + ":" + hashlib.scrypt(pw.encode(), salt=salt, n=2 ** 14, r=8, p=1, dklen=32).hex()


def check_pw(pw, stored):
    salt_hex, h = stored.split(":")
    got = hashlib.scrypt(pw.encode(), salt=bytes.fromhex(salt_hex), n=2 ** 14, r=8, p=1, dklen=32).hex()
    return hmac.compare_digest(got, h)


class Server:
    def __init__(self, dbpath, google=None, require_google=False, require_e2e=False):
        self.require_e2e = require_e2e
        self.keyput_log = {}
        self.google, self.require_google = google, require_google and google is not None
        self.login_fail = {}
        self.db = sqlite3.connect(dbpath)
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS users(id TEXT PRIMARY KEY, token TEXT, name TEXT, created INTEGER);
            CREATE TABLE IF NOT EXISTS friends(a TEXT, b TEXT, PRIMARY KEY(a,b));
            CREATE TABLE IF NOT EXISTS reqs(frm TEXT, to_ TEXT, intro TEXT, PRIMARY KEY(frm,to_));
            CREATE TABLE IF NOT EXISTS blocks(a TEXT, b TEXT, PRIMARY KEY(a,b));
            CREATE TABLE IF NOT EXISTS msgs(seq INTEGER PRIMARY KEY AUTOINCREMENT, frm TEXT, to_ TEXT, ts INTEGER, text TEXT, msgid TEXT);
        """)
        try:
            self.db.execute("ALTER TABLE msgs ADD COLUMN kind INTEGER DEFAULT 0")     # 0 message, 1 edit, 2 delete
        except sqlite3.OperationalError:
            pass
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS groups(gid TEXT PRIMARY KEY, name TEXT, owner TEXT, pwhash TEXT, created INTEGER,
                                              gfails INTEGER DEFAULT 0, gwin INTEGER DEFAULT 0, glocked INTEGER DEFAULT 0);
            CREATE TABLE IF NOT EXISTS gmembers(gid TEXT, uid TEXT, joined INTEGER, PRIMARY KEY(gid,uid));
        """)
        try:
            self.db.execute("ALTER TABLE msgs ADD COLUMN gid TEXT DEFAULT ''")
        except sqlite3.OperationalError:
            pass
        for sql in ("ALTER TABLE gmembers ADD COLUMN role INTEGER DEFAULT 0", "ALTER TABLE groups ADD COLUMN ann TEXT DEFAULT ''",
                    "ALTER TABLE groups ADD COLUMN ann_by TEXT DEFAULT ''", "ALTER TABLE groups ADD COLUMN ann_ts INTEGER DEFAULT 0",
                    "ALTER TABLE users ADD COLUMN restricted INTEGER DEFAULT 0", "ALTER TABLE users ADD COLUMN last_seen INTEGER DEFAULT 0",
                    "ALTER TABLE users ADD COLUMN hide_pres INTEGER DEFAULT 0"):
            try: self.db.execute(sql)
            except sqlite3.OperationalError: pass
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS gnotes(id INTEGER PRIMARY KEY AUTOINCREMENT, gid TEXT, author TEXT, ts INTEGER, text TEXT);
            CREATE TABLE IF NOT EXISTS reports(id INTEGER PRIMARY KEY AUTOINCREMENT, reporter TEXT, target TEXT, ts INTEGER, reason TEXT, evidence TEXT);
            CREATE TABLE IF NOT EXISTS reads(reader TEXT, writer TEXT, gid TEXT, upto INTEGER, PRIMARY KEY(reader,writer,gid));
        """)
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS google(sub TEXT PRIMARY KEY, uid TEXT UNIQUE, email TEXT, ts INTEGER);
        """)
        self.tries = {}           # (uid, gid) -> [wrong guesses, window start, locked until]
        self.any_tries = {}       # uid -> [wrong guesses, window start, locked until]
        self.online = {}          # user id -> set of writers
        self.fdir = dbpath + ".files"
        os.makedirs(self.fdir, exist_ok=True)
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS files(fid TEXT PRIMARY KEY, owner TEXT, name TEXT, size INTEGER, created INTEGER, done INTEGER DEFAULT 0);
            CREATE TABLE IF NOT EXISTS e2e_keys(uid TEXT PRIMARY KEY, bundle TEXT, ts INTEGER);
            CREATE TABLE IF NOT EXISTS file_acl(fid TEXT, uid TEXT, PRIMARY KEY(fid,uid));
        """)
        for sql in ("ALTER TABLE users ADD COLUMN handle TEXT", "ALTER TABLE users ADD COLUMN bio TEXT DEFAULT ''",
                    "ALTER TABLE users ADD COLUMN disc INTEGER DEFAULT 0", "ALTER TABLE users ADD COLUMN avatar TEXT DEFAULT ''",
                    "ALTER TABLE users ADD COLUMN invite TEXT", "ALTER TABLE files ADD COLUMN public INTEGER DEFAULT 0"):
            try: self.db.execute(sql)
            except sqlite3.OperationalError: pass
        self.db.execute("CREATE UNIQUE INDEX IF NOT EXISTS users_handle ON users(handle)")
        self.db.execute("CREATE UNIQUE INDEX IF NOT EXISTS users_invite ON users(invite)")
        self.db.commit()
        self.purge()

    def q(self, sql, *a):
        return self.db.execute(sql, a)

    def name_of(self, uid):
        r = self.q("SELECT name FROM users WHERE id=?", uid).fetchone()
        return r[0] if r else None

    def profile(self, uid):
        r = self.q("SELECT name, COALESCE(handle,''), COALESCE(bio,''), COALESCE(disc,0), COALESCE(avatar,''), COALESCE(invite,'') FROM users WHERE id=?", uid).fetchone()
        return r

    def friend_frame(self, of):
        r = self.profile(of)
        return frame("FRIEND", of, r[0] if r else "?", r[1] if r else "", r[4] if r else "")

    def push_friend_info(self, uid):
        for (b,) in self.q("SELECT b FROM friends WHERE a=?", uid).fetchall():
            r = self.profile(uid)
            self.push(b, "FRIEND", uid, r[0], r[1], r[4])

    def new_invite(self, uid):
        while True:
            code = "".join(secrets.choice(INVITE_ALPHABET) for _ in range(8))
            try:
                self.q("UPDATE users SET invite=? WHERE id=?", code, uid); self.db.commit(); return code
            except sqlite3.IntegrityError:
                continue

    def are_friends(self, a, b):
        return self.q("SELECT 1 FROM friends WHERE a=? AND b=?", a, b).fetchone() is not None

    def share_group(self, a, b):
        return self.q("SELECT 1 FROM gmembers x JOIN gmembers y ON x.gid=y.gid WHERE x.uid=? AND y.uid=?", a, b).fetchone() is not None

    def blocked(self, by, who):
        return self.q("SELECT 1 FROM blocks WHERE a=? AND b=?", by, who).fetchone() is not None

    def push(self, uid, *fields):
        for w in list(self.online.get(uid, ())):
            try:
                w.write(frame(*fields))
            except Exception:
                pass

    def new_id(self):
        while True:
            uid = str(secrets.randbelow(90_000_000) + 10_000_000)
            if not self.q("SELECT 1 FROM users WHERE id=?", uid).fetchone():
                return uid

    @staticmethod
    def tok_hash(token):
        return "h:" + hashlib.sha256(token.encode()).hexdigest()

    def token_ok(self, uid, token, stored):
        if stored.startswith("h:"):
            return secrets.compare_digest(stored, self.tok_hash(token))
        if secrets.compare_digest(stored, token):                       # an account from before tokens were hashed: upgrade it
            self.q("UPDATE users SET token=? WHERE id=?", self.tok_hash(token), uid); self.db.commit()
            return True
        return False

    def new_token(self, uid):
        token = secrets.token_hex(12)
        self.q("UPDATE users SET token=? WHERE id=?", self.tok_hash(token), uid); self.db.commit()
        return token

    def linked(self, uid):
        r = self.q("SELECT email FROM google WHERE uid=?", uid).fetchone()
        return r[0] if r else None

    def google_status(self, uid):
        em = self.linked(uid)
        return frame("GOOGLE_STATUS", "1" if em else "0", mask_email(em) if em else "", "1" if self.require_google else "0", "1" if self.google else "0")

    async def google_start(self, writer, gflow, mode, name=""):
        if not self.google:
            writer.write(frame("ERR", "this server has no Google sign-in")); return
        try:
            d = await asyncio.to_thread(self.google.start)
        except GoogleError as e:
            writer.write(frame("ERR", str(e))); return
        now = time.time()
        gflow.clear(); gflow.update(mode=mode, name=name, dev=d["device_code"], interval=max(3, d["interval"]), exp=now + d["expires_in"], next=now + max(3, d["interval"]))
        writer.write(frame("GOOGLE_CODE", d["user_code"], d["url"], d["expires_in"], max(3, d["interval"])))

    async def google_poll(self, uid, writer, gflow):
        """One poll of the sign-in in progress. Returns the (possibly new) user id."""
        now = time.time()
        if not gflow: writer.write(frame("ERR", "no Google sign-in in progress")); return uid
        if now > gflow["exp"]: gflow.clear(); writer.write(frame("ERR", "the code expired - start again")); return uid
        if now < gflow["next"]: writer.write(frame("GOOGLE_PENDING")); return uid           # asked too early: do not bother Google
        gflow["next"] = now + gflow["interval"]
        try:
            res = await asyncio.to_thread(self.google.poll, gflow["dev"])
        except GoogleError as e:
            gflow.clear(); writer.write(frame("ERR", str(e))); return uid
        if res in ("pending", "slow_down"):
            if res == "slow_down": gflow["interval"] += 5
            writer.write(frame("GOOGLE_PENDING")); return uid
        if res in ("denied", "expired"):
            gflow.clear(); writer.write(frame("ERR", "the Google sign-in was cancelled" if res == "denied" else "the code expired - start again")); return uid
        sub, email, mode, name = res["sub"], res["email"], gflow["mode"], gflow["name"]
        gflow.clear()
        row = self.q("SELECT uid FROM google WHERE sub=?", sub).fetchone()
        if mode == "link":
            if row and row[0] != uid:
                writer.write(frame("ERR", "that Google account is already linked to another C-OS account")); return uid
            if self.linked(uid) and not row:
                writer.write(frame("ERR", "this account is already linked to a different Google account")); return uid
            self.q("INSERT OR REPLACE INTO google(sub,uid,email,ts) VALUES(?,?,?,?)", sub, uid, email, int(now)); self.db.commit()
            writer.write(frame("GOOGLE_OK", mask_email(email))); return uid
        if row:                                               # the same person on a new device: same account, new token, old tokens die
            nid = row[0]
            token = self.new_token(nid)
            self.q("UPDATE google SET email=?, ts=? WHERE sub=?", email, int(now), sub); self.db.commit()
            nm = self.name_of(nid)
        else:
            nid, token, nm = self.new_id(), secrets.token_hex(12), (name.strip()[:MAX_NAME] or "user")
            self.q("INSERT INTO users(id,token,name,created) VALUES(?,?,?,?)", nid, self.tok_hash(token), nm, int(now))
            self.q("INSERT INTO google(sub,uid,email,ts) VALUES(?,?,?,?)", sub, nid, email, int(now)); self.db.commit()
        writer.write(frame("OK_REG", nid, token, nm)); writer.write(frame("GOOGLE_OK", mask_email(email)))
        await self.on_login(nid, writer)
        return nid

    async def handle(self, reader, writer):
        uid = None
        gflow = {}                # the Google sign-in in progress on this connection
        uploads = {}              # fid -> [file object, next chunk index, bytes so far, declared size]
        window, count = time.time(), 0
        try:
            while True:
                try:
                    line = await asyncio.wait_for(reader.readline(), 120)
                except asyncio.TimeoutError:
                    break
                if not line or len(line) > MAX_LINE:
                    break
                now = time.time()
                if now - window > 5:
                    window, count = now, 0
                if not line.startswith(b"FCH\t"):             # upload chunks are bounded by the file size limit instead
                    count += 1
                if count > 40:                               # flood: drop the connection
                    break
                f = [unesc(x) for x in line.decode("utf-8", "replace").rstrip("\r\n").split("\t")]
                cmd = f[0]
                if VERBOSE and cmd != "FCH": print("rx", uid, repr(line[:120]), flush=True)
                if cmd == "PING":
                    writer.write(frame("PONG")); await writer.drain(); continue
                if cmd == "REG" and uid is None and len(f) >= 2 and self.require_google:
                    writer.write(frame("NEED_GOOGLE"))
                elif cmd == "GREG" and uid is None:
                    await self.google_start(writer, gflow, "reg", f[1] if len(f) > 1 else "")
                elif cmd == "GPOLL" and uid is None:
                    uid = await self.google_poll(uid, writer, gflow)
                elif cmd == "REG" and uid is None and len(f) >= 2:
                    name = f[1].strip()[:MAX_NAME] or "user"
                    nid, token = self.new_id(), secrets.token_hex(12)
                    self.q("INSERT INTO users(id,token,name,created) VALUES(?,?,?,?)", nid, self.tok_hash(token), name, int(now)); self.db.commit()
                    writer.write(frame("OK_REG", nid, token, name))
                    uid = nid
                    await self.on_login(uid, writer)
                elif cmd == "LOGIN" and uid is None and len(f) >= 3:
                    r = self.q("SELECT token,name FROM users WHERE id=?", f[1]).fetchone()
                    fl = self.login_fail.get(f[1], [0, now])
                    if fl[0] >= 5 and now - fl[1] < 600:
                        writer.write(frame("ERR", "too many failed logins - wait a few minutes")); await writer.drain(); break
                    if not r or not self.token_ok(f[1], f[2], r[0]):
                        self.login_fail[f[1]] = [fl[0] + 1, fl[1] if now - fl[1] < 600 else now]
                        writer.write(frame("ERR", "bad id or token")); await writer.drain(); await asyncio.sleep(1); break
                    self.login_fail.pop(f[1], None)
                    uid = f[1]
                    writer.write(frame("OK_LOGIN", uid, r[1]))
                    await self.on_login(uid, writer)
                elif uid is None:
                    writer.write(frame("ERR", "login first"))
                elif cmd in ("GLINK", "GPOLL", "GSTATUS", "LOGOUT_ALL"):
                    if cmd == "GLINK": await self.google_start(writer, gflow, "link")
                    elif cmd == "GPOLL": await self.google_poll(uid, writer, gflow)
                    elif cmd == "GSTATUS": writer.write(self.google_status(uid))
                    else: writer.write(frame("OK_LOGOUT_ALL", self.new_token(uid)))
                elif self.require_google and not self.linked(uid) and cmd not in ("NAME", "ME", "PING"):
                    writer.write(frame("ERR", "sign in with Google first (/google)"))
                else:
                    await self.command(uid, writer, f, uploads)
                await writer.drain()
        except (ConnectionError, asyncio.IncompleteReadError):
            pass
        finally:
            for fid, st in uploads.items():                      # unfinished uploads are discarded
                try: st[0].close(); os.remove(os.path.join(self.fdir, fid + ".part"))
                except OSError: pass
                self.q("DELETE FROM files WHERE fid=? AND done=0", fid)
            self.db.commit()
            if uid and uid in self.online:
                self.online[uid].discard(writer)
                if not self.online[uid]:
                    del self.online[uid]
                    self.q("UPDATE users SET last_seen=? WHERE id=?", int(time.time()), uid); self.db.commit()
                    self.broadcast_presence(uid)
            try:
                writer.close()
            except Exception:
                pass

    def pres_state(self, uid):
        r = self.q("SELECT COALESCE(hide_pres,0), COALESCE(last_seen,0) FROM users WHERE id=?", uid).fetchone()
        if not r: return 0, 0
        if r[0]: return 2, 0
        return (1, int(time.time())) if self.online.get(uid) else (0, r[1])

    def broadcast_presence(self, uid):
        st, ts = self.pres_state(uid)
        for (b,) in self.q("SELECT b FROM friends WHERE a=?", uid).fetchall(): self.push(b, "PRES", uid, st, ts)

    async def on_login(self, uid, writer):
        first = not self.online.get(uid)
        self.online.setdefault(uid, set()).add(writer)
        if first: self.broadcast_presence(uid)
        for (b,) in self.q("SELECT b FROM friends WHERE a=?", uid).fetchall():
            writer.write(self.friend_frame(b))
            st, ts = self.pres_state(b); writer.write(frame("PRES", b, st, ts))
        for frm, intro in self.q("SELECT frm,intro FROM reqs WHERE to_=?", uid).fetchall():
            pr = self.profile(frm)
            writer.write(frame("FREQ_IN", frm, pr[0] if pr else "?", intro, pr[1] if pr else "", pr[4] if pr else ""))
        for (gid,) in self.q("SELECT gid FROM gmembers WHERE uid=?", uid).fetchall():
            for fr in self.group_state(gid): writer.write(fr)
        for reader, gid, upto in self.q("SELECT reader,gid,upto FROM reads WHERE writer=?", uid).fetchall():
            writer.write(frame("GREAD_IN", gid, reader, upto) if gid else frame("READ_IN", reader, upto))
        for row in self.q("SELECT seq,frm,ts,text,msgid,kind,COALESCE(gid,'') FROM msgs WHERE to_=? ORDER BY seq", uid).fetchall():
            writer.write(self.event_frame(*row))
        writer.write(self.google_status(uid))
        writer.write(frame("SYNCED"))

    def purge(self):
        old = int(time.time()) - FILE_TTL
        for (fid,) in self.q("SELECT fid FROM files WHERE created<? AND public=0", old).fetchall():
            try: os.remove(os.path.join(self.fdir, fid))
            except OSError: pass
        self.q("DELETE FROM file_acl WHERE fid IN (SELECT fid FROM files WHERE created<? AND public=0)", old)
        self.q("DELETE FROM files WHERE created<? AND public=0", old); self.db.commit()

    def used(self, uid):
        return self.q("SELECT COALESCE(SUM(size),0) FROM files WHERE owner=?", uid).fetchone()[0]

    async def send_file(self, w, fid):
        try:
            with open(os.path.join(self.fdir, fid), "rb") as fp:
                data, idx = fp.read(CHUNK), 0
                while True:
                    nxt = fp.read(CHUNK)
                    w.write(frame("FDATA", fid, idx, 0 if nxt else 1, base64.b64encode(data).decode()))
                    await w.drain()
                    idx += 1
                    if not nxt: break
                    data = nxt
        except (OSError, ConnectionError):
            try: w.write(frame("FERR", fid, "read failed"))
            except Exception: pass

    @staticmethod
    def event_frame(seq, frm, ts, text, msgid, kind, gid=""):
        if gid:
            if kind == 3: return frame("GKEY_IN", seq, gid, frm, ts, msgid, text)
            if kind == 1: return frame("GEDIT_IN", seq, gid, frm, ts, msgid, text)
            if kind == 2: return frame("GDEL_IN", seq, gid, frm, ts, msgid)
            return frame("GMSG", seq, gid, frm, ts, text, msgid)
        if kind == 1:
            return frame("EDIT_IN", seq, frm, ts, msgid, text)
        if kind == 2:
            return frame("DEL_IN", seq, frm, ts, msgid)
        return frame("MSG", seq, frm, ts, text, msgid)

    def backlog_full(self, frm, to):
        n = self.q("SELECT COUNT(*) FROM msgs WHERE to_=?", to).fetchone()[0]
        if n >= MAX_BACKLOG: return True
        return frm is not None and self.q("SELECT COUNT(*) FROM msgs WHERE to_=? AND frm=?", to, frm).fetchone()[0] >= MAX_BACKLOG_PAIR

    def queue(self, frm, to, msgid, text, kind):
        ts = int(time.time())
        cur = self.q("INSERT INTO msgs(frm,to_,ts,text,msgid,kind) VALUES(?,?,?,?,?,?)", frm, to, ts, text, msgid, kind)
        self.db.commit()
        for w in list(self.online.get(to, ())):
            try:
                w.write(self.event_frame(cur.lastrowid, frm, ts, text, msgid, kind))
            except Exception:
                pass

    # ------------------------------------------------------------------ groups
    def members(self, gid):
        return [r[0] for r in self.q("SELECT uid FROM gmembers WHERE gid=? ORDER BY joined, rowid", gid).fetchall()]

    def group_state(self, gid):
        g = self.q("SELECT name,owner FROM groups WHERE gid=?", gid).fetchone()
        if not g: return []
        out = [frame("GROUP", gid, g[0], g[1])]
        out += [self.member_frame(gid, m) for m in self.members(gid)]
        a = self.q("SELECT ann,ann_by,ann_ts FROM groups WHERE gid=?", gid).fetchone()
        if a and a[0]: out.append(frame("GANN", gid, a[0], a[1], a[2]))
        return out

    def role_of(self, gid, uid):
        g = self.q("SELECT owner FROM groups WHERE gid=?", gid).fetchone()
        if g and g[0] == uid: return 2
        r = self.q("SELECT role FROM gmembers WHERE gid=? AND uid=?", gid, uid).fetchone()
        return (r[0] or 0) if r else -1

    def member_frame(self, gid, uid):
        return frame("GMEMBER", gid, uid, self.name_of(uid) or "?", self.role_of(gid, uid))

    def push_members(self, gid, uid):
        for m in self.members(gid): self.push(m, "GMEMBER", gid, uid, self.name_of(uid) or "?", self.role_of(gid, uid))

    def push_group_state(self, gid, to=None):
        for t in ([to] if to else self.members(gid)):
            for fr in self.group_state(gid):
                for w in list(self.online.get(t, ())):
                    try: w.write(fr)
                    except Exception: pass

    def queue_group(self, frm, gid, msgid, text, kind):
        ts = int(time.time())
        for m in self.members(gid):
            if m == frm or self.backlog_full(frm, m): continue                    # a member whose mailbox is full simply misses it (the server stays bounded)
            cur = self.q("INSERT INTO msgs(frm,to_,ts,text,msgid,kind,gid) VALUES(?,?,?,?,?,?,?)", frm, m, ts, text, msgid, kind, gid)
            for w in list(self.online.get(m, ())):
                try: w.write(self.event_frame(cur.lastrowid, frm, ts, text, msgid, kind, gid))
                except Exception: pass
        self.db.commit()

    def drop_member(self, gid, uid):
        self.q("DELETE FROM gmembers WHERE gid=? AND uid=?", gid, uid)
        self.q("DELETE FROM msgs WHERE to_=? AND gid=?", uid, gid)
        g = self.q("SELECT owner FROM groups WHERE gid=?", gid).fetchone()
        rest = self.members(gid)
        if not rest:
            self.q("DELETE FROM groups WHERE gid=?", gid); self.q("DELETE FROM msgs WHERE gid=?", gid)
            self.q("DELETE FROM gnotes WHERE gid=?", gid); self.q("DELETE FROM reads WHERE gid=?", gid)
        elif g and g[0] == uid:
            adm = self.q("SELECT uid FROM gmembers WHERE gid=? AND role=1 ORDER BY joined, rowid", gid).fetchone()
            self.q("UPDATE groups SET owner=? WHERE gid=?", adm[0] if adm else rest[0], gid)
            self.q("UPDATE gmembers SET role=0 WHERE gid=? AND uid=?", gid, adm[0] if adm else rest[0])
        self.db.commit()
        self.push(uid, "GROUP_DEL", gid)
        for m in rest: self.push(m, "GMEMBER_DEL", gid, uid)
        if rest and g and g[0] == uid: self.push_group_state(gid)

    def lock_left(self, uid, gid):
        now = time.time()
        left = max(self.tries.get((uid, gid), [0, 0, 0])[2], self.any_tries.get(uid, [0, 0, 0])[2])
        g = self.q("SELECT glocked FROM groups WHERE gid=?", gid).fetchone()
        if g: left = max(left, g[0])
        return int(left - now) if left > now else 0

    async def verify_group_pw(self, uid, gid, pw):
        """None when the password is right, otherwise the text of the error. A wrong id and a wrong password look the same."""
        left = self.lock_left(uid, gid)
        if left > 0:
            return "too many wrong passwords - try again in %d minutes" % (left // 60 + 1)
        g = self.q("SELECT pwhash FROM groups WHERE gid=?", gid).fetchone()
        ok = False
        if PW_RE.match(pw):
            ok = await asyncio.to_thread(check_pw, pw, g[0] if g else hash_pw("000000"))
            ok = ok and g is not None
        else:
            await asyncio.to_thread(hash_pw, "000000")
        if ok:
            self.tries.pop((uid, gid), None)
            return None
        now = time.time()
        t = self.tries.setdefault((uid, gid), [0, now, 0])
        t[0] += 1
        if t[0] >= USER_TRIES: t[0], t[2] = 0, now + USER_LOCK
        a = self.any_tries.setdefault(uid, [0, now, 0])
        if now - a[1] > 3600: a[0], a[1] = 0, now
        a[0] += 1
        if a[0] >= ANY_TRIES: a[0], a[2] = 0, now + ANY_LOCK
        if g:
            row = self.q("SELECT gfails,gwin FROM groups WHERE gid=?", gid).fetchone()
            fails, win = (row[0] + 1, row[1]) if now - row[1] <= GROUP_WINDOW else (1, int(now))
            locked = int(now + GROUP_LOCK) if fails >= GROUP_TRIES else 0
            self.q("UPDATE groups SET gfails=?, gwin=?, glocked=? WHERE gid=?", 0 if locked else fails, win, locked, gid); self.db.commit()
        return "wrong group ID or password"

    async def group_command(self, uid, w, f):
        cmd, n = f[0], len(f)
        gid = f[2] if cmd in ("GSEND", "GEDIT", "GDEL") and n > 2 else (f[1] if n > 1 else "")
        is_member = self.q("SELECT 1 FROM gmembers WHERE gid=? AND uid=?", gid, uid).fetchone() is not None
        owner = self.q("SELECT owner FROM groups WHERE gid=?", gid).fetchone()
        is_owner = owner is not None and owner[0] == uid
        if cmd == "GNEW" and n >= 2:
            name = f[1].strip()[:MAX_GNAME]
            pw = f[2].strip() if n > 2 else ""
            if not name: w.write(frame("ERR", "give the group a name")); return
            if self.q("SELECT COUNT(*) FROM gmembers WHERE uid=?", uid).fetchone()[0] >= GROUPS_PER_USER:
                w.write(frame("ERR", "you are in too many groups")); return
            if pw and (not PW_RE.match(pw) or weak_pw(pw)):
                w.write(frame("ERR", "the password must be 6 digits and not too simple (not 123456, 111111, ...)")); return
            pw = pw or gen_pw()
            while True:
                gid = "g" + str(secrets.randbelow(90_000_000) + 10_000_000)
                if not self.q("SELECT 1 FROM groups WHERE gid=?", gid).fetchone(): break
            h = await asyncio.to_thread(hash_pw, pw)
            self.q("INSERT INTO groups(gid,name,owner,pwhash,created) VALUES(?,?,?,?,?)", gid, name, uid, h, int(time.time()))
            self.q("INSERT INTO gmembers(gid,uid,joined) VALUES(?,?,?)", gid, uid, int(time.time())); self.db.commit()
            w.write(frame("OK_GNEW", gid, name, pw))
            self.push_group_state(gid)
        elif cmd == "GJOIN" and n >= 3:
            err = await self.verify_group_pw(uid, gid, f[2].strip())
            if err: w.write(frame("ERR", err)); return
            if is_member: w.write(frame("ERR", "you are already in this group")); return
            if len(self.members(gid)) >= GROUP_MEMBERS: w.write(frame("ERR", "this group is full")); return
            if self.q("SELECT COUNT(*) FROM gmembers WHERE uid=?", uid).fetchone()[0] >= GROUPS_PER_USER:
                w.write(frame("ERR", "you are in too many groups")); return
            self.q("INSERT INTO gmembers(gid,uid,joined) VALUES(?,?,?)", gid, uid, int(time.time())); self.db.commit()
            w.write(frame("OK_GJOIN", gid, self.q("SELECT name FROM groups WHERE gid=?", gid).fetchone()[0]))
            self.push_group_state(gid, uid)
            for m in self.members(gid):
                if m != uid: self.push(m, "GMEMBER", gid, uid, self.name_of(uid), 0)
        elif not is_member:
            w.write(frame("ERR", "you are not in that group"))
        elif cmd == "GLEAVE":
            self.drop_member(gid, uid); w.write(frame("OK_GLEAVE", gid))
        elif cmd == "GINFO":
            for fr in self.group_state(gid): w.write(fr)
        elif cmd == "GKEY" and n >= 5:
            to, epoch, wrapped = f[2], f[3], f[4]
            if to == uid or not self.q("SELECT 1 FROM gmembers WHERE gid=? AND uid=?", gid, to).fetchone():
                w.write(frame("ERR", "not a member of that group")); return
            if not wrapped.startswith("\x01E") or len(wrapped) > MAX_E2E or len(epoch) > 12:
                w.write(frame("ERR", "bad group key")); return
            self.q("DELETE FROM msgs WHERE frm=? AND to_=? AND gid=? AND kind=3 AND msgid=?", uid, to, gid, epoch)
            if self.q("SELECT COUNT(*) FROM msgs WHERE to_=? AND kind=3", to).fetchone()[0] >= MAX_GKEY_WAITING or self.backlog_full(None, to):
                w.write(frame("ERR", "that member has too many keys waiting")); return
            ts = int(time.time()); cur = self.q("INSERT INTO msgs(frm,to_,ts,text,msgid,kind,gid) VALUES(?,?,?,?,?,?,?)", uid, to, ts, wrapped, epoch, 3, gid); self.db.commit()
            for ow in list(self.online.get(to, ())):
                try: ow.write(self.event_frame(cur.lastrowid, uid, ts, wrapped, epoch, 3, gid))
                except Exception: pass
            w.write(frame("OK_GKEY", gid, to, epoch))
        elif cmd == "GSEND" and n >= 4:
            msgid, text = f[1], f[3]
            genc = text.startswith("\x01G")
            if genc and len(text) > MAX_E2E:
                w.write(frame("NOSEND", msgid, "too long")); return
            if self.require_e2e and not genc:
                w.write(frame("NOSEND", msgid, "encryption required")); return
            if not genc: text = text[:MAX_TEXT]
            if genc and n >= 5 and f[4]:
                if not self.q("SELECT 1 FROM files WHERE fid=? AND owner=? AND done=1", f[4], uid).fetchone():
                    w.write(frame("NOSEND", msgid, "file not uploaded")); return
                for m in self.members(gid): self.q("INSERT OR IGNORE INTO file_acl VALUES(?,?)", f[4], m)
            if text.startswith("\x01F|"):
                fid = text.split("|", 3)[1] if text.count("|") >= 3 else ""
                if not self.q("SELECT 1 FROM files WHERE fid=? AND owner=? AND done=1", fid, uid).fetchone():
                    w.write(frame("NOSEND", msgid, "file not uploaded")); return
                for m in self.members(gid): self.q("INSERT OR IGNORE INTO file_acl VALUES(?,?)", fid, m)
            self.queue_group(uid, gid, msgid, text, 0); w.write(frame("GSENT", msgid))
        elif cmd == "GEDIT" and n >= 4:
            text = f[3]; genc = text.startswith("\x01G")
            if genc and len(text) > MAX_E2E:
                w.write(frame("NOSEND", f[1], "too long")); return
            if self.require_e2e and not genc:
                w.write(frame("NOSEND", f[1], "encryption required")); return
            self.queue_group(uid, gid, f[1], text if genc else text[:MAX_TEXT], 1); w.write(frame("OK_GEDIT", f[1]))
        elif cmd == "GDEL" and n >= 3:
            self.queue_group(uid, gid, f[1], "", 2); w.write(frame("OK_GDEL", f[1]))
        elif cmd == "GNOTE" and n >= 3:
            text = f[2]
            if text.startswith("\x01G"):
                if len(text) > MAX_E2E: w.write(frame("ERR", "note too long")); return
            else:
                if self.require_e2e: w.write(frame("ERR", "encryption required")); return
                text = text.strip()[:MAX_TEXT]
            if not text: return
            if self.q("SELECT COUNT(*) FROM gnotes WHERE gid=?", gid).fetchone()[0] >= 100:
                w.write(frame("ERR", "this group already has 100 notes - delete one first")); return
            ts = int(time.time())
            nid = self.q("INSERT INTO gnotes(gid,author,ts,text) VALUES(?,?,?,?)", gid, uid, ts, text).lastrowid; self.db.commit()
            w.write(frame("OK_GNOTE", gid, nid))
            for m in self.members(gid): self.push(m, "GNOTE", gid, nid, uid, ts, text)
        elif cmd == "GNOTES":
            for nid, au, ts, text in self.q("SELECT id,author,ts,text FROM gnotes WHERE gid=? ORDER BY id", gid).fetchall():
                w.write(frame("GNOTE", gid, nid, au, ts, text))
            w.write(frame("GNOTES_END", gid))
        elif cmd == "GNOTEDEL" and n >= 3:
            r = self.q("SELECT author FROM gnotes WHERE gid=? AND id=?", gid, f[2]).fetchone()
            if not r: w.write(frame("ERR", "no such note")); return
            if r[0] != uid and self.role_of(gid, uid) < 1: w.write(frame("ERR", "only the author or an admin can delete a note")); return
            self.q("DELETE FROM gnotes WHERE gid=? AND id=?", gid, f[2]); self.db.commit()
            w.write(frame("OK_GNOTEDEL", gid, f[2]))
            for m in self.members(gid): self.push(m, "GNOTE_DEL", gid, f[2])
        elif cmd == "GTYPING":
            for m in self.members(gid):
                if m != uid: self.push(m, "GTYPING_IN", gid, uid)
        elif cmd == "GREAD" and n >= 4:
            if f[2] in self.members(gid) and f[3].isdigit():
                self.q("INSERT OR REPLACE INTO reads VALUES(?,?,?,?)", uid, f[2], gid, int(f[3])); self.db.commit()
                self.push(f[2], "GREAD_IN", gid, uid, f[3])
        elif cmd in ("GANN", "GKICK", "GRENAME") and self.role_of(gid, uid) < 1:
            w.write(frame("ERR", "only the group owner or a sub-admin can do that"))
        elif cmd == "GANN":
            text = (f[2] if n > 2 else "")
            if text.startswith("\x01G"):
                if len(text) > MAX_E2E: w.write(frame("ERR", "announcement too long")); return
            else:
                if self.require_e2e and text.strip(): w.write(frame("ERR", "encryption required")); return
                text = text.strip()[:300]
            self.q("UPDATE groups SET ann=?, ann_by=?, ann_ts=? WHERE gid=?", text, uid, int(time.time()), gid); self.db.commit()
            w.write(frame("OK_GANN", gid))
            for m in self.members(gid): self.push(m, "GANN", gid, text, uid, int(time.time()))
        elif cmd == "GKICK" and n >= 3:
            if f[2] == uid or f[2] not in self.members(gid): w.write(frame("ERR", "no such member")); return
            if not is_owner and self.role_of(gid, f[2]) >= 1: w.write(frame("ERR", "only the owner can remove an admin")); return
            self.drop_member(gid, f[2]); w.write(frame("OK_GKICK", gid, f[2]))
        elif cmd == "GRENAME" and n >= 3:
            name = f[2].strip()[:MAX_GNAME]
            if not name: w.write(frame("ERR", "give the group a name")); return
            self.q("UPDATE groups SET name=? WHERE gid=?", name, gid); self.db.commit(); self.push_group_state(gid)
        elif not is_owner:
            w.write(frame("ERR", "only the group owner can do that"))
        elif cmd == "GPASS":
            pw = f[2].strip() if n > 2 else ""
            if pw and (not PW_RE.match(pw) or weak_pw(pw)):
                w.write(frame("ERR", "the password must be 6 digits and not too simple (not 123456, 111111, ...)")); return
            pw = pw or gen_pw()
            h = await asyncio.to_thread(hash_pw, pw)
            self.q("UPDATE groups SET pwhash=?, gfails=0, glocked=0 WHERE gid=?", h, gid); self.db.commit()
            w.write(frame("OK_GPASS", gid, pw))
        elif cmd == "GROLE" and n >= 4:
            if f[2] == uid or f[2] not in self.members(gid) or f[3] not in ("0", "1"):
                w.write(frame("ERR", "no such member")); return
            if f[3] == "1" and self.q("SELECT COUNT(*) FROM gmembers WHERE gid=? AND role=1", gid).fetchone()[0] >= 10:
                w.write(frame("ERR", "a group can have at most 10 sub-admins")); return
            self.q("UPDATE gmembers SET role=? WHERE gid=? AND uid=?", int(f[3]), gid, f[2]); self.db.commit()
            w.write(frame("OK_GROLE", gid, f[2], f[3])); self.push_members(gid, f[2])
        elif cmd == "GOWNER" and n >= 3:
            if f[2] == uid or f[2] not in self.members(gid): w.write(frame("ERR", "no such member")); return
            self.q("UPDATE groups SET owner=? WHERE gid=?", f[2], gid)
            self.q("UPDATE gmembers SET role=0 WHERE gid=? AND uid=?", gid, f[2])
            self.q("UPDATE gmembers SET role=1 WHERE gid=? AND uid=?", gid, uid); self.db.commit()
            w.write(frame("OK_GOWNER", gid, f[2])); self.push_group_state(gid)
        else:
            w.write(frame("ERR", "unknown command"))

    async def command(self, uid, w, f, uploads):
        cmd, n = f[0], len(f)
        if cmd in ("GNEW", "GJOIN", "GLEAVE", "GINFO", "GSEND", "GEDIT", "GDEL", "GPASS", "GKICK", "GRENAME", "GROLE", "GOWNER", "GANN",
                   "GNOTE", "GNOTES", "GNOTEDEL", "GREAD", "GTYPING", "GKEY"):
            await self.group_command(uid, w, f)
        elif cmd == "NAME" and n >= 2:
            name = f[1].strip()[:MAX_NAME]
            if name:
                self.q("UPDATE users SET name=? WHERE id=?", name, uid); self.db.commit()
                w.write(frame("OK_NAME", name))
                self.push_friend_info(uid)
                for (gid,) in self.q("SELECT gid FROM gmembers WHERE uid=?", uid).fetchall():
                    self.push_members(gid, uid)
        elif cmd == "HANDLE":
            h = (f[1] if n > 1 else "").strip().lower().lstrip("@")
            if h and not HANDLE_RE.match(h):
                w.write(frame("ERR", "handle: 3-20 characters, a-z 0-9 _ .")); return
            try:
                self.q("UPDATE users SET handle=? WHERE id=?", h or None, uid); self.db.commit()
            except sqlite3.IntegrityError:
                w.write(frame("ERR", "that handle is taken")); return
            w.write(frame("OK_HANDLE", h)); self.push_friend_info(uid)
        elif cmd == "BIO":
            self.q("UPDATE users SET bio=? WHERE id=?", (f[1] if n > 1 else "").strip()[:MAX_BIO], uid); self.db.commit()
            w.write(frame("OK_BIO"))
        elif cmd == "DISC" and n >= 2:
            self.q("UPDATE users SET disc=? WHERE id=?", 1 if f[1] == "1" else 0, uid); self.db.commit()
            w.write(frame("OK_DISC", "1" if f[1] == "1" else "0"))
        elif cmd == "AVATAR":
            fid = f[1] if n > 1 else ""
            if fid:
                r = self.q("SELECT size FROM files WHERE fid=? AND owner=? AND done=1", fid, uid).fetchone()
                if not r or r[0] > MAX_AVATAR:
                    w.write(frame("ERR", "avatar must be an uploaded image up to 256 KB")); return
                self.q("UPDATE files SET public=1 WHERE fid=?", fid)
            self.q("UPDATE users SET avatar=? WHERE id=?", fid, uid); self.db.commit()
            w.write(frame("OK_AVATAR", fid)); self.push_friend_info(uid)
        elif cmd == "ME":
            pr = self.profile(uid)
            if not pr[5]: self.new_invite(uid); pr = self.profile(uid)
            w.write(frame("ME", uid, pr[0], pr[1], pr[2], pr[3], pr[4], pr[5]))
        elif cmd == "INVITE_NEW":
            w.write(frame("INVITE", self.new_invite(uid)))
        elif cmd == "JOIN" and n >= 2:
            code = f[1].strip().upper().replace("-", "").replace(" ", "")
            r = self.q("SELECT id FROM users WHERE invite=?", code).fetchone()
            if not r: w.write(frame("ERR", "invalid invite code")); return
            await self.command(uid, w, ["FREQ", r[0], "Joined with your invite code"], uploads)
        elif cmd == "SEARCH" and n >= 2:
            q = f[1].strip().lower().lstrip("@")[:30]
            rows = []
            if len(q) >= 2:
                like = q.replace("%", "").replace("_", "\\_")
                rows = self.q("""SELECT id,name,COALESCE(handle,''),COALESCE(bio,''),COALESCE(avatar,'') FROM users
                                 WHERE disc=1 AND COALESCE(restricted,0)=0 AND id!=? AND (handle LIKE ? ESCAPE '\\' OR lower(name) LIKE ? ESCAPE '\\')
                                 AND NOT EXISTS(SELECT 1 FROM blocks WHERE a=users.id AND b=?)
                                 ORDER BY (handle=?) DESC, handle LIKE ? ESCAPE '\\' DESC, name LIMIT 20""",
                              uid, like + "%", "%" + like + "%", uid, q, like + "%").fetchall()
            for r in rows: w.write(frame("RESULT", *r))
            w.write(frame("RESULT_END", str(len(rows))))
        elif cmd == "PROFILE" and n >= 2:
            pr = self.profile(f[1])
            if not pr or self.blocked(f[1], uid): w.write(frame("ERR", "no such user")); return
            visible = (f[1] == uid or pr[3] == 1 or self.are_friends(uid, f[1])
                       or self.q("SELECT 1 FROM reqs WHERE frm=? AND to_=?", f[1], uid).fetchone() is not None)
            w.write(frame("PROFILE", f[1], pr[0], pr[1] if visible else "", pr[2] if visible else "", pr[4] if visible else ""))
        elif cmd == "FIND" and n >= 2:
            nm = self.name_of(f[1])
            w.write(frame("FOUND", f[1], nm) if nm and not self.blocked(f[1], uid) else frame("ERR", "no such user"))
        elif cmd == "FREQ" and n >= 2:
            to, intro = f[1], (f[2] if n > 2 else "")[:120]
            if to == uid or not self.name_of(to) or self.blocked(to, uid):
                w.write(frame("ERR", "cannot send a request to that user")); return
            if self.q("SELECT restricted FROM users WHERE id=?", uid).fetchone()[0]:
                w.write(frame("ERR", "your account is restricted after reports from other users")); return
            if self.are_friends(uid, to):
                w.write(frame("ERR", "already friends")); return
            if self.q("SELECT 1 FROM reqs WHERE frm=? AND to_=?", to, uid).fetchone():   # they already asked us: accept
                await self.command(uid, w, ["FACC", to]); return
            if self.q("SELECT COUNT(*) FROM reqs WHERE frm=?", uid).fetchone()[0] >= 50:
                w.write(frame("ERR", "too many pending requests")); return
            self.q("INSERT OR REPLACE INTO reqs VALUES(?,?,?)", uid, to, intro); self.db.commit()
            w.write(frame("OK_FREQ", to))
            pr = self.profile(uid)
            self.push(to, "FREQ_IN", uid, pr[0], intro, pr[1], pr[4])
        elif cmd == "FACC" and n >= 2:
            other = f[1]
            if not self.q("SELECT 1 FROM reqs WHERE frm=? AND to_=?", other, uid).fetchone():
                w.write(frame("ERR", "no such request")); return
            self.q("DELETE FROM reqs WHERE frm=? AND to_=?", other, uid)
            self.q("INSERT OR IGNORE INTO friends VALUES(?,?)", uid, other)
            self.q("INSERT OR IGNORE INTO friends VALUES(?,?)", other, uid); self.db.commit()
            for target, of in ((uid, other), (other, uid)):
                pr = self.profile(of)
                self.push(target, "FRIEND", of, pr[0], pr[1], pr[4])
                st, ts = self.pres_state(of); self.push(target, "PRES", of, st, ts)        # the new friend's online state, right away
        elif cmd == "FDEC" and n >= 2:
            self.q("DELETE FROM reqs WHERE frm=? AND to_=?", f[1], uid); self.db.commit()
        elif cmd == "FDEL" and n >= 2:
            self.q("DELETE FROM friends WHERE (a=? AND b=?) OR (a=? AND b=?)", uid, f[1], f[1], uid); self.db.commit()
            self.push(uid, "FRIEND_DEL", f[1]); self.push(f[1], "FRIEND_DEL", uid)
        elif cmd == "TYPING" and n >= 2:
            if self.are_friends(uid, f[1]): self.push(f[1], "TYPING_IN", uid)
        elif cmd == "SETPRES" and n >= 2:
            v = 1 if f[1] == "1" else 0
            self.q("UPDATE users SET hide_pres=? WHERE id=?", 0 if v else 1, uid); self.db.commit()
            w.write(frame("OK_SETPRES", v)); self.broadcast_presence(uid)
        elif cmd == "BLOCKS":
            rows = self.q("SELECT b FROM blocks WHERE a=?", uid).fetchall()
            for (b,) in rows: w.write(frame("BLOCKED", b, self.name_of(b) or "?"))
            w.write(frame("BLOCKS_END", str(len(rows))))
        elif cmd == "UNBLOCK" and n >= 2:
            self.q("DELETE FROM blocks WHERE a=? AND b=?", uid, f[1]); self.db.commit(); w.write(frame("OK_UNBLOCK", f[1]))
        elif cmd == "REPORT" and n >= 3:
            target, reason, evidence = f[1], f[2].strip()[:200], (f[3] if n > 3 else "")[:1000]
            if not self.name_of(target) or target == uid: w.write(frame("ERR", "no such user")); return
            now = int(time.time())
            if self.q("SELECT COUNT(*) FROM reports WHERE reporter=? AND ts>?", uid, now - 3600).fetchone()[0] >= 5:
                w.write(frame("ERR", "too many reports - try again later")); return
            if self.q("SELECT 1 FROM reports WHERE reporter=? AND target=? AND ts>?", uid, target, now - 86400).fetchone():
                w.write(frame("ERR", "you already reported that user today")); return
            self.q("INSERT INTO reports(reporter,target,ts,reason,evidence) VALUES(?,?,?,?,?)", uid, target, now, reason, evidence)
            if self.q("SELECT COUNT(DISTINCT reporter) FROM reports WHERE target=? AND ts>?", target, now - 7 * 86400).fetchone()[0] >= REPORT_LIMIT:
                self.q("UPDATE users SET restricted=1 WHERE id=?", target)
            self.q("INSERT OR IGNORE INTO blocks VALUES(?,?)", uid, target)      # whoever reports somebody does not hear from them again
            self.q("DELETE FROM friends WHERE (a=? AND b=?) OR (a=? AND b=?)", uid, target, target, uid); self.db.commit()
            w.write(frame("OK_REPORT", target)); self.push(uid, "FRIEND_DEL", target)
        elif cmd == "READ" and n >= 3:
            if self.are_friends(uid, f[1]) and f[2].isdigit():
                self.q("INSERT OR REPLACE INTO reads VALUES(?,?,?,?)", uid, f[1], "", int(f[2])); self.db.commit()
                self.push(f[1], "READ_IN", uid, f[2])
        elif cmd == "BLOCK" and n >= 2:
            self.q("INSERT OR IGNORE INTO blocks VALUES(?,?)", uid, f[1])
            self.q("DELETE FROM friends WHERE (a=? AND b=?) OR (a=? AND b=?)", uid, f[1], f[1], uid)
            self.q("DELETE FROM reqs WHERE frm=? AND to_=?", f[1], uid); self.db.commit()
            w.write(frame("OK_BLOCK", f[1])); self.push(uid, "FRIEND_DEL", f[1])
        elif cmd == "KEYPUT" and n >= 2:
            if not re.fullmatch(r"[A-Za-z0-9_-]{263}", f[1]):
                w.write(frame("ERR", "bad key bundle")); return
            now = time.time(); log = [t for t in self.keyput_log.get(uid, []) if now - t < 3600]
            old = self.q("SELECT bundle FROM e2e_keys WHERE uid=?", uid).fetchone()
            if old and old[0] == f[1]:
                w.write(frame("OK_KEYPUT")); return                                # the same bundle again: nothing to do
            if len(log) >= KEYPUT_PER_HOUR:
                w.write(frame("ERR", "too many key changes - try again later")); return
            log.append(now); self.keyput_log[uid] = log
            if len(self.keyput_log) > 1024: self.keyput_log = {u: l for u, l in self.keyput_log.items() if l and now - l[-1] < 3600}      # (the table stays bounded)
            self.q("INSERT OR REPLACE INTO e2e_keys VALUES(?,?,?)", uid, f[1], int(time.time())); self.db.commit()
            w.write(frame("OK_KEYPUT"))
        elif cmd == "KEYGET" and n >= 2:
            if f[1] != uid and not self.are_friends(uid, f[1]) and not self.share_group(uid, f[1]):
                w.write(frame("ERR", "not friends")); return
            r = self.q("SELECT bundle FROM e2e_keys WHERE uid=?", f[1]).fetchone()
            w.write(frame("KEY", f[1], r[0] if r else ""))
        elif cmd == "SEND" and n >= 4:
            msgid, to, text = f[1], f[2], f[3]
            enc = text.startswith("\x01E")
            if not self.are_friends(uid, to):
                w.write(frame("NOSEND", msgid, "not friends")); return
            if enc and len(text) > MAX_E2E:
                w.write(frame("NOSEND", msgid, "too long")); return
            if self.require_e2e and not enc:
                w.write(frame("NOSEND", msgid, "encryption required")); return
            if not enc: text = text[:MAX_TEXT]
            if self.backlog_full(uid, to):
                w.write(frame("NOSEND", msgid, "the recipient's mailbox is full")); return
            fid = f[4] if n >= 5 and enc else (text.split("|", 3)[1] if text.startswith("\x01F|") and text.count("|") >= 3 else "")
            if fid:
                r = self.q("SELECT 1 FROM files WHERE fid=? AND owner=? AND done=1", fid, uid).fetchone()
                if not r:
                    w.write(frame("NOSEND", msgid, "file not uploaded")); return
                self.q("INSERT OR IGNORE INTO file_acl VALUES(?,?)", fid, to)
            self.queue(uid, to, msgid, text, 0)
            w.write(frame("SENT", msgid))
        elif cmd in ("EDIT", "DEL") and n >= (4 if cmd == "EDIT" else 3):
            msgid, to = f[1], f[2]
            if not self.are_friends(uid, to):
                w.write(frame("NOSEND", msgid, "not friends")); return
            if self.backlog_full(uid, to):
                w.write(frame("NOSEND", msgid, "the recipient's mailbox is full")); return
            if cmd == "EDIT":
                text = f[3]; enc = text.startswith("\x01E")
                if enc and len(text) > MAX_E2E:
                    w.write(frame("NOSEND", msgid, "too long")); return
                if self.require_e2e and not enc:
                    w.write(frame("NOSEND", msgid, "encryption required")); return
                self.queue(uid, to, msgid, text if enc else text[:MAX_TEXT], 1); w.write(frame("OK_EDIT", msgid))
            else:
                self.queue(uid, to, msgid, "", 2); w.write(frame("OK_DEL", msgid))
        elif cmd == "GOT" and n >= 2:
            try:
                seq = int(f[1])
            except ValueError:
                return
            r = self.q("SELECT frm,msgid,kind,COALESCE(gid,'') FROM msgs WHERE seq=? AND to_=?", seq, uid).fetchone()
            if r:
                self.q("DELETE FROM msgs WHERE seq=?", seq); self.db.commit()
                if r[2] == 0 and not r[3]:
                    self.push(r[0], "DELIV", r[1], uid)
        elif cmd == "FUP" and n >= 3:
            try: size = int(f[1])
            except ValueError: size = 0
            if size < 1 or size > MAX_FILE:
                w.write(frame("FERR", "", "file too large (max %d MB)" % (MAX_FILE >> 20))); return
            if len(uploads) >= 2 or self.used(uid) + size > QUOTA:
                w.write(frame("FERR", "", "upload quota exceeded")); return
            fid = secrets.token_hex(8)
            name = os.path.basename(f[2].replace("\\", "/"))[:80] or "file"
            self.q("INSERT INTO files(fid,owner,name,size,created,done) VALUES(?,?,?,?,?,0)", fid, uid, name, size, int(time.time())); self.db.commit()
            uploads[fid] = [open(os.path.join(self.fdir, fid + ".part"), "wb"), 0, 0, size]
            w.write(frame("FUP_OK", fid))
        elif cmd == "FCH" and n >= 4:
            st = uploads.get(f[1])
            if not st: return
            try:
                data = base64.b64decode(f[3], validate=True); idx = int(f[2])
            except (binascii.Error, ValueError):
                data, idx = b"", -1
            if idx != st[1] or not data or len(data) > CHUNK or st[2] + len(data) > st[3]:
                st[0].close(); os.remove(os.path.join(self.fdir, f[1] + ".part")); del uploads[f[1]]
                self.q("DELETE FROM files WHERE fid=?", f[1]); self.db.commit()
                w.write(frame("FERR", f[1], "bad chunk")); return
            st[0].write(data); st[1] += 1; st[2] += len(data)
        elif cmd == "FEND" and n >= 2:
            st = uploads.pop(f[1], None)
            if not st: w.write(frame("FERR", f[1], "no such upload")); return
            st[0].close()
            part = os.path.join(self.fdir, f[1] + ".part")
            if st[2] != st[3]:
                os.remove(part); self.q("DELETE FROM files WHERE fid=?", f[1]); self.db.commit()
                w.write(frame("FERR", f[1], "size mismatch")); return
            os.replace(part, os.path.join(self.fdir, f[1]))
            self.q("UPDATE files SET done=1 WHERE fid=?", f[1]); self.db.commit()
            w.write(frame("FEND_OK", f[1]))
        elif cmd == "FGET" and n >= 2:
            fid = f[1]
            ok = self.q("SELECT 1 FROM files WHERE fid=? AND done=1 AND (public=1 OR owner=? OR EXISTS(SELECT 1 FROM file_acl WHERE fid=files.fid AND uid=?))", fid, uid, uid).fetchone()
            if not ok:
                w.write(frame("FERR", fid, "no such file")); return
            asyncio.ensure_future(self.send_file(w, fid))
        else:
            w.write(frame("ERR", "unknown command"))


def make_context(cert, key):
    if not (os.path.exists(cert) and os.path.exists(key)):
        subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes",
                        "-days", "3650", "-subj", "/CN=cosmsg", "-keyout", key, "-out", cert],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.chmod(key, 0o600)
        print(f"generated a self-signed certificate: {cert}", flush=True)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(cert, key)
    return ctx


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cert", default="cosmsg.crt")
    ap.add_argument("--key", default="cosmsg.key")
    ap.add_argument("--port", type=int, default=5151)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--db", default="cosmsg.db")
    ap.add_argument("--verbose", action="store_true", help="log every received command (not chunk payloads)")
    ap.add_argument("--reports", action="store_true", help="print the stored user reports and exit")
    ap.add_argument("--google-client-id", default=os.environ.get("COSMSG_GOOGLE_CLIENT_ID", ""), help="OAuth client id (type 'TVs and Limited Input devices') - enables Google sign-in")
    ap.add_argument("--google-client-secret", default=os.environ.get("COSMSG_GOOGLE_CLIENT_SECRET", ""))
    ap.add_argument("--google-domains", default="gmail.com,googlemail.com", help="e-mail domains that may sign in")
    ap.add_argument("--require-e2e", action="store_true", help="refuse 1:1 messages that are not end-to-end encrypted")
    ap.add_argument("--require-google", action="store_true", help="no anonymous accounts: everybody has to sign in with Google")
    a = ap.parse_args()
    if a.reports:
        db = sqlite3.connect(a.db)
        for rid, rep_, tgt, ts, reason, ev in db.execute("SELECT id,reporter,target,ts,reason,evidence FROM reports ORDER BY id DESC LIMIT 200"):
            tn = db.execute("SELECT name,COALESCE(restricted,0) FROM users WHERE id=?", (tgt,)).fetchone() or ("?", 0)
            print("#%d %s  reporter %s -> %s (%s)%s\n  reason: %s\n  evidence: %s" % (rid, time.strftime("%Y-%m-%d %H:%M", time.localtime(ts)), rep_, tgt, tn[0], "  [RESTRICTED]" if tn[1] else "", reason, ev.replace("\n", " | ")))
        return
    global VERBOSE
    VERBOSE = a.verbose
    google = GoogleBackend(a.google_client_id, a.google_client_secret, a.google_domains.split(",")) if a.google_client_id and a.google_client_secret else None
    if a.require_google and not google: raise SystemExit("--require-google needs --google-client-id and --google-client-secret")
    srv = Server(a.db, google, a.require_google, a.require_e2e)
    server = await asyncio.start_server(srv.handle, a.bind, a.port, ssl=make_context(a.cert, a.key))
    print(f"cosmsg server (TLS) listening on {a.bind}:{a.port}, db={os.path.abspath(a.db)}", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())

#!/usr/bin/env python3
"""Web GUI / HTTP サーバーの安全性と並行性の実機検査。

本物の build/python-code-to-c-gui を起動し、ソケットで生の HTTP 要求を送って次を確かめる。

  - トークン無しの API 呼び出しは拒否される（CSRF / 外部ページからの実行を防ぐ）
  - 許可しない Host / Origin は拒否される（DNS リバインディング・別サイトからの要求）
  - 本文が Content-Length まで届かない要求は処理されない（400）
  - 過大なヘッダ・ボディ・不正な Content-Length・chunked は拒否される
  - 遅い接続が他の利用者の応答を止めない（接続ごとの子プロセス）
  - 無限ループのプログラムは期限で止まり、サーバーは応答を続ける
  - 公開（--allow-remote）時は /api/run が既定で無効
  - 一時ディレクトリが残らない

使い方: python3 tests/gui_web_security_alpha10.py [GUI実行ファイル]
"""
import glob
import http.client
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BUILD = os.environ.get("P2C_BUILD_DIR", "build")
GUI = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, BUILD, "python-code-to-c-gui")
PORT = int(os.environ.get("P2C_TEST_PORT", "18765"))
failures = []


def check(cond, label):
    status = "ok  " if cond else "FAIL"
    print(f"{status} {label}")
    if not cond:
        failures.append(label)


def start_server(extra=()):
    env = dict(os.environ)
    cmd = [GUI, f"--port={PORT}", "--no-browser", *extra]
    proc = subprocess.Popen(cmd, cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            start_new_session=True)
    for _ in range(100):
        try:
            with socket.create_connection(("127.0.0.1", PORT), timeout=0.2):
                return proc
        except OSError:
            if proc.poll() is not None:
                break
            time.sleep(0.05)
    out, err = proc.communicate(timeout=2) if proc.poll() is not None else (b"", b"")
    raise SystemExit(f"server did not start: {err.decode(errors='replace')}")


def stop_server(proc):
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except OSError:
        pass
    proc.wait(timeout=5)


def request(method, path, body=None, headers=None, host=None, timeout=20):
    conn = http.client.HTTPConnection("127.0.0.1", PORT, timeout=timeout)
    hdrs = dict(headers or {})
    if host is not None:
        hdrs["Host"] = host
    try:
        conn.request(method, path, body=body, headers=hdrs)
        resp = conn.getresponse()
        data = resp.read()
        return resp.status, data
    finally:
        conn.close()


def raw(payload, wait=3.0):
    """生のバイト列を送り、応答の先頭行を返す（送信を途中で止めたいケース用）。"""
    s = socket.create_connection(("127.0.0.1", PORT), timeout=wait)
    data = b""
    try:
        try:
            s.sendall(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass  # サーバーが先に拒否して閉じた場合。応答は下で読む。
        end = time.time() + wait
        while time.time() < end:
            try:
                chunk = s.recv(4096)
            except socket.timeout:
                break
            except ConnectionResetError:
                break
            if not chunk:
                break
            data += chunk
        return data.split(b"\r\n", 1)[0].decode(errors="replace"), data
    finally:
        s.close()


def main():
    if not os.path.exists(GUI):
        raise SystemExit(f"GUI binary not found: {GUI} (run: make gui)")
    before = set(glob.glob("/tmp/p2c-gui-*"))

    proc = start_server()
    try:
        status, page = request("GET", "/")
        text = page.decode("utf-8", "replace")
        m = re.search(r'const P2C_TOKEN = "([0-9a-f]{64})"', text)
        check(status == 200 and m is not None, "GET / serves the page with a 256-bit token")
        token = m.group(1) if m else ""
        check("__P2C_TOKEN__" not in text, "token placeholder is replaced")

        ok_host = f"127.0.0.1:{PORT}"
        auth = {"X-P2C-Token": token, "Content-Type": "text/plain; charset=utf-8"}

        st, _ = request("POST", "/api/python_code_to_c", body="print(1)",
                        headers={"Content-Type": "text/plain"}, host=ok_host)
        check(st == 403, "API without token is refused (403)")

        st, _ = request("POST", "/api/python_code_to_c", body="print(1)",
                        headers={**auth, "X-P2C-Token": "00" * 32}, host=ok_host)
        check(st == 403, "API with a wrong token is refused (403)")

        st, body = request("POST", "/api/python_code_to_c", body="print(1)", headers=auth, host=ok_host)
        check(st == 200 and b'"ok":true' in body, "API with the right token converts code")

        st, _ = request("POST", "/api/run", body="print(1)", headers=auth, host="evil.example:%d" % PORT)
        check(st == 403, "foreign Host header is refused (DNS rebinding)")

        st, _ = request("POST", "/api/run", body="print(1)",
                        headers={**auth, "Origin": "http://evil.example"}, host=ok_host)
        check(st == 403, "cross-origin POST with the token is refused (Origin check)")

        st, body = request("POST", "/api/run", body='print("hello from run")',
                           headers={**auth, "Origin": f"http://127.0.0.1:{PORT}"}, host=ok_host)
        check(st == 200 and b"hello from run" in body, "same-origin run executes the program")

        # 本文が途中で切れた要求は処理されない（Content-Length より少ない本文）
        req = (f"POST /api/run HTTP/1.1\r\nHost: {ok_host}\r\nX-P2C-Token: {token}\r\n"
               f"Content-Length: 500\r\n\r\nprint('truncated')").encode()
        line, data = raw(req, wait=1.0)
        check(b"hello" not in data and b"truncated" not in data.split(b"\r\n\r\n", 1)[-1],
              "truncated body is not processed")
        check(line.startswith("HTTP/1.1 400") or line == "", "truncated body gets 400 (or the connection closes)")

        line, _ = raw(b"POST /api/run HTTP/1.1\r\nHost: %s\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n"
                      % ok_host.encode(), wait=2.0)
        check(line.startswith("HTTP/1.1 501"), "chunked transfer encoding is refused (501)")

        line, _ = raw(b"POST /api/run HTTP/1.1\r\nHost: %s\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabc4"
                      % ok_host.encode(), wait=2.0)
        check(line.startswith("HTTP/1.1 400"), "conflicting Content-Length headers are refused (400)")

        line, _ = raw(b"POST /api/run HTTP/1.1\r\nHost: %s\r\nContent-Length: abc\r\n\r\n" % ok_host.encode(),
                      wait=2.0)
        check(line.startswith("HTTP/1.1 400"), "non-numeric Content-Length is refused (400)")

        line, _ = raw(b"POST /api/run HTTP/1.1\r\nHost: %s\r\nContent-Length: 99999999999\r\n\r\n"
                      % ok_host.encode(), wait=2.0)
        check(line.startswith("HTTP/1.1 413"), "oversized body is refused before reading (413)")

        big_header = b"GET / HTTP/1.1\r\nHost: " + ok_host.encode() + b"\r\nX-Pad: " + b"a" * 70000 + b"\r\n\r\n"
        line, _ = raw(big_header, wait=2.0)
        check(line.startswith("HTTP/1.1 431") or line == "", "oversized headers are refused (431)")

        # 遅い接続（ヘッダを途中まで送って止める）が、別の利用者の応答を止めないこと
        slow = socket.create_connection(("127.0.0.1", PORT), timeout=5)
        slow.sendall(b"GET / HTTP/1.1\r\nHost: " + ok_host.encode() + b"\r\n")
        t0 = time.time()
        st, _ = request("GET", "/api/version", host=ok_host, timeout=5)
        elapsed = time.time() - t0
        check(st == 200 and elapsed < 2.0, f"a stalled connection does not block others ({elapsed:.2f}s)")
        slow.close()

        # 無限ループは期限で止まり、その後もサーバーは応答する
        t0 = time.time()
        st, body = request("POST", "/api/run", body="while True:\n    pass\n", headers=auth,
                           host=ok_host, timeout=60)
        took = time.time() - t0
        check(st == 200 and "タイムアウト".encode() in body, "infinite loop is stopped by the run limit")
        check(took < 45, f"run limit enforced in bounded time ({took:.1f}s)")
        st, _ = request("GET", "/api/version", host=ok_host)
        check(st == 200, "server keeps answering after a run timeout")
    finally:
        stop_server(proc)

    # 公開モード: --allow-remote だけでは /api/run は無効
    remote = subprocess.run([GUI, f"--port={PORT}", "--host=0.0.0.0", "--no-browser"], cwd=ROOT,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
    check(remote.returncode != 0 and b"allow-remote" in remote.stderr,
          "binding to a non-loopback address requires --allow-remote")

    proc = start_server(["--allow-remote", "--host=0.0.0.0"])
    try:
        status, page = request("GET", "/")
        m = re.search(r'const P2C_TOKEN = "([0-9a-f]{64})"', page.decode("utf-8", "replace"))
        token = m.group(1) if m else ""
        auth = {"X-P2C-Token": token, "Content-Type": "text/plain"}
        st, body = request("POST", "/api/run", body='print("x")', headers=auth,
                           host=f"127.0.0.1:{PORT}")
        check(st == 403 and b"enable-run" in body, "run is disabled when served with --allow-remote")
    finally:
        stop_server(proc)

    after = set(glob.glob("/tmp/p2c-gui-*"))
    check(after - before == set(), "no p2c-gui temporary directories are left behind")

    if failures:
        print(f"\n{len(failures)} check(s) failed")
        return 1
    print("\ngui_web_security_ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())

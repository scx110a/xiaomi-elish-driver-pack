#!/usr/bin/env python3
"""Protocol/state tests for bioauthd (no root, mock backend)."""

import base64
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
DAEMON = HERE / "daemon" / "bioauthd.py"


class Daemon:
    def __init__(self, backend="mock-fail", max_failures=3, password="testpass"):
        self.tmp = Path(tempfile.mkdtemp(prefix="bioauth-test-"))
        self.sock = self.tmp / "bioauth.sock"
        self.state = self.tmp / "state.json"
        env = os.environ.copy()
        env.update({
            "BIOAUTH_SOCKET": str(self.sock),
            "BIOAUTH_STATE": str(self.state),
            "BIOAUTH_BACKEND": backend,
            "BIOAUTH_MAX_FAILURES": str(max_failures),
            "BIOAUTH_FP_TIMEOUT": "2",
            "BIOAUTH_TEST_PASSWORD": password,
        })
        self.proc = subprocess.Popen(
            [sys.executable, str(DAEMON)],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        deadline = time.time() + 5
        while time.time() < deadline:
            if self.sock.exists():
                return
            if self.proc.poll() is not None:
                out, err = self.proc.communicate(timeout=2)
                raise RuntimeError(f"daemon exited: {out}\n{err}")
            time.sleep(0.05)
        raise RuntimeError("daemon did not create socket")

    def request(self, obj):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect(str(self.sock))
        s.sendall((json.dumps(obj) + "\n").encode())
        data = b""
        while b"\n" not in data:
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
        s.close()
        return json.loads(data.decode())

    def stop(self):
        try:
            self.proc.terminate()
            self.proc.wait(timeout=3)
        except Exception:
            self.proc.kill()
        shutil.rmtree(self.tmp, ignore_errors=True)


USER = os.environ.get("USER") or "caitming"


class TestBioAuthD(unittest.TestCase):
    def setUp(self):
        self.d = Daemon()
        self.addCleanup(self.d.stop)

    def test_status_initial(self):
        r = self.d.request({"op": "status", "user": USER})
        self.assertTrue(r["ok"])
        self.assertFalse(r["locked"])
        self.assertEqual(r["fail_count"], 0)
        self.assertEqual(r["max_failures"], 3)

    def test_lock_then_password_unlock(self):
        for i in range(3):
            r = self.d.request({"op": "fingerprint", "user": USER})
            self.assertFalse(r["ok"])
            self.assertEqual(r["fail_count"], i + 1)
            self.assertEqual(r["locked"], i == 2)
        # while locked, fingerprint must be refused without a new attempt
        r = self.d.request({"op": "fingerprint", "user": USER})
        self.assertFalse(r["ok"])
        self.assertTrue(r["locked"])
        self.assertEqual(r["reason"], "locked")
        # wrong password must not unlock
        r = self.d.request({
            "op": "password", "user": USER,
            "password_b64": base64.b64encode(b"wrong").decode(),
        })
        self.assertFalse(r["ok"])
        # correct password unlocks and resets
        r = self.d.request({
            "op": "password", "user": USER,
            "password_b64": base64.b64encode(b"testpass").decode(),
        })
        self.assertTrue(r["ok"])
        self.assertFalse(r["locked"])
        self.assertEqual(r["fail_count"], 0)
        r = self.d.request({"op": "status", "user": USER})
        self.assertFalse(r["locked"])
        self.assertEqual(r["fail_count"], 0)

    def test_state_persists(self):
        for _ in range(3):
            self.d.request({"op": "fingerprint", "user": USER})
        # state file has the record
        obj = json.loads(self.d.state.read_text())
        rec = obj["users"][USER]
        self.assertTrue(rec["locked"])
        self.assertEqual(rec["fail_count"], 3)

    def test_bad_encoding(self):
        r = self.d.request({"op": "password", "user": USER, "password_b64": "!!!"})
        self.assertFalse(r["ok"])


class TestBackendUnavailable(unittest.TestCase):
    def test_device_error_does_not_lock(self):
        d = Daemon(backend="mock-device-error")
        self.addCleanup(d.stop)
        before = d.request({"op": "status", "user": USER})
        r = d.request({"op": "fingerprint", "user": USER})
        self.assertFalse(r["ok"])
        self.assertTrue(r.get("unavailable"))
        self.assertEqual(r["fail_count"], before["fail_count"])
        self.assertFalse(r["locked"])


class TestMockSuccess(unittest.TestCase):
    def test_success(self):
        d = Daemon(backend="mock-success")
        self.addCleanup(d.stop)
        r = d.request({"op": "fingerprint", "user": USER})
        self.assertTrue(r["ok"])


if __name__ == "__main__":
    unittest.main()


class TestTimeoutAndCancel(unittest.TestCase):
    """Timeout must not lock; cancel must not count a failure (req-scoped)."""

    def _daemon(self, **kw):
        d = Daemon(backend="mock-fail", max_failures=3, **kw)
        self.addCleanup(d.stop)
        return d

    def test_scan_timeout_is_not_a_failure(self):
        d = Daemon(backend="mock-success", max_failures=3)
        self.addCleanup(d.stop)
        # mock-success with a long delay + 1s fingerprint timeout => timeout
        d.stop()
        import subprocess as sp
        import time as _t
        tmp = Path(tempfile.mkdtemp(prefix="bioauth-to-"))
        self.addCleanup(shutil.rmtree, tmp, True)
        env = os.environ.copy()
        env.update({
            "BIOAUTH_SOCKET": str(tmp / "s.sock"), "BIOAUTH_STATE": str(tmp / "st.json"),
            "BIOAUTH_BACKEND": "mock-success", "BIOAUTH_MAX_FAILURES": "3",
            "BIOAUTH_FP_TIMEOUT": "1", "BIOAUTH_MOCK_DELAY": "5",
            "BIOAUTH_TEST_PASSWORD": "x",
        })
        proc = sp.Popen([sys.executable, str(DAEMON)], env=env,
                        stdout=sp.DEVNULL, stderr=sp.DEVNULL)
        self.addCleanup(lambda: (proc.terminate(), proc.wait(timeout=3)))
        sock = tmp / "s.sock"
        for _ in range(100):
            if sock.exists():
                break
            _t.sleep(0.05)

        def req(obj):
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(10)
            s.connect(str(sock))
            s.sendall((json.dumps(obj) + "\n").encode())
            data = b""
            while b"\n" not in data:
                data += s.recv(4096)
            s.close()
            return json.loads(data.decode())

        r = req({"op": "fingerprint", "user": USER, "req": "to:1"})
        self.assertFalse(r["ok"])
        self.assertTrue(r.get("timeout"))
        self.assertEqual(r["fail_count"], 0)
        self.assertFalse(r["locked"])
        r = req({"op": "fingerprint", "user": USER, "req": "to:2"})
        self.assertEqual(r["fail_count"], 0)

    def test_cancel_in_flight_is_scoped_and_not_counted(self):
        d = Daemon(backend="mock-fail", max_failures=3)
        self.addCleanup(d.stop)
        d.stop()
        import subprocess as sp
        import threading
        import time as _t
        tmp = Path(tempfile.mkdtemp(prefix="bioauth-cancel-"))
        self.addCleanup(shutil.rmtree, tmp, True)
        env = os.environ.copy()
        env.update({
            "BIOAUTH_SOCKET": str(tmp / "s.sock"), "BIOAUTH_STATE": str(tmp / "st.json"),
            "BIOAUTH_BACKEND": "mock-fail", "BIOAUTH_MAX_FAILURES": "3",
            "BIOAUTH_FP_TIMEOUT": "10", "BIOAUTH_MOCK_DELAY": "5",
            "BIOAUTH_TEST_PASSWORD": "x",
        })
        proc = sp.Popen([sys.executable, str(DAEMON)], env=env,
                        stdout=sp.DEVNULL, stderr=sp.DEVNULL)
        self.addCleanup(lambda: (proc.terminate(), proc.wait(timeout=3)))
        sock = tmp / "s.sock"
        for _ in range(100):
            if sock.exists():
                break
            _t.sleep(0.05)

        def req(obj):
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(10)
            s.connect(str(sock))
            s.sendall((json.dumps(obj) + "\n").encode())
            data = b""
            while b"\n" not in data:
                chunk = s.recv(4096)
                if not chunk:
                    break
                data += chunk
            s.close()
            return json.loads(data.decode())

        # In-flight cancel: fire cancel while the request is being served.
        sess = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sess.settimeout(10)
        sess.connect(str(sock))
        sess.sendall((json.dumps({"op": "fingerprint", "user": USER,
                                  "req": "c:2"}) + "\n").encode())
        _t.sleep(0.2)
        cancel = req({"op": "cancel", "user": USER, "req": "c:2"})
        self.assertTrue(cancel["ok"])
        data = b""
        while b"\n" not in data:
            chunk = sess.recv(4096)
            if not chunk:
                break
            data += chunk
        sess.close()
        r = json.loads(data.decode())
        self.assertTrue(r.get("canceled"))
        self.assertFalse(r["ok"])
        st = req({"op": "status", "user": USER})
        self.assertEqual(st["fail_count"], 0)
        self.assertFalse(st["locked"])


class TestPasswordBackend(unittest.TestCase):
    """Real-shadow password verification logic (no root: mocked shadow entry)."""

    @classmethod
    def setUpClass(cls):
        import importlib.util
        spec = importlib.util.spec_from_file_location("bioauthd_mod", DAEMON)
        cls.mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.mod)

    def _verify(self, expire, password="secret", stored=None):
        import unittest.mock as mock
        if stored is None:
            stored = self.mod.crypt.crypt(password, "$6$" + "a" * 16 + "$")
        sp = type("SP", (), {"sp_pwdp": stored, "sp_expire": expire})()
        with mock.patch.object(self.mod.spwd, "getspnam", return_value=sp):
            return self.mod.PasswordBackend().verify("caitming", password)

    def test_empty_expiry_minus_one_is_not_expired(self):
        # shadow(5) + spwd: empty expire field is -1, not 0/None
        self.assertTrue(self._verify(-1))
        self.assertTrue(self._verify(0))
        self.assertTrue(self._verify(None))

    def test_past_expiry_denies(self):
        import time
        past = int(time.time() // 86400) - 10
        self.assertFalse(self._verify(past))

    def test_future_expiry_allows(self):
        import time
        future = int(time.time() // 86400) + 10
        self.assertTrue(self._verify(future))

    def test_wrong_password_denies(self):
        stored = self.mod.crypt.crypt("right", "$6$" + "b" * 16 + "$")
        self.assertFalse(self._verify(-1, password="wrong", stored=stored))

    def test_locked_hash_denies(self):
        self.assertFalse(self._verify(-1, stored="!$6$xxxx"))

#!/usr/bin/env python3
"""
bioauthd - generic biometric PAM authentication daemon (MVP)

Design goals:
  * device-agnostic: the default backend is fprintd (any libfprint device)
  * no vendor/TA/QSEE code, no /dev/goodix_fp assumptions
  * root-owned lockout state: N fingerprint failures -> fingerprint disabled
    until a successful password authentication
  * terminal/UI agnostic: the PAM client decides how to talk to the user

Protocol: newline-delimited JSON over a Unix socket.
  request  {"op":"status","user":"caitming"}
  request  {"op":"fingerprint","user":"caitming"}
  request  {"op":"password","user":"caitming","password_b64":"..."}
  response {"ok":true,"locked":false,"fail_count":0,"max_failures":3,...}

This MVP is intentionally small; backends are pluggable in principle.
"""

from __future__ import annotations

import argparse
import base64
import binascii
import warnings

warnings.filterwarnings("ignore", category=DeprecationWarning)

import crypt  # noqa: S404 - local shadow verification, deprecated on 3.13
import hmac
import json
import os
import pwd
import signal
import socket
import spwd  # noqa: S403 - root-only shadow access
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

warnings.filterwarnings("ignore", category=DeprecationWarning)

DEFAULT_SOCKET = "/run/bioauth.sock"
DEFAULT_STATE = "/var/lib/bioauth/state.json"
DEFAULT_MAX_FAILURES = 3
DEFAULT_FINGERPRINT_TIMEOUT = 30
MAX_MESSAGE = 64 * 1024


def log(msg: str) -> None:
    print(f"bioauthd: {msg}", flush=True, file=sys.stderr)


class State:
    """Root-owned state, keyed by user. Persisted atomically."""

    def __init__(self, path: Path):
        self.path = path
        self.lock = threading.Lock()
        self.data: dict = {"version": 1, "users": {}}
        self._load()

    def _load(self) -> None:
        try:
            raw = self.path.read_text(encoding="utf-8")
            obj = json.loads(raw)
            if isinstance(obj, dict) and isinstance(obj.get("users"), dict):
                self.data = obj
        except FileNotFoundError:
            pass
        except Exception as exc:  # corrupt state must not block auth
            log(f"state load failed ({exc}); starting fresh")

    def _save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(self.path.suffix + ".tmp")
        tmp.write_text(json.dumps(self.data, ensure_ascii=False, sort_keys=True), encoding="utf-8")
        os.chmod(tmp, 0o600)
        try:
            fd = os.open(tmp, os.O_RDONLY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
        except OSError:
            pass
        os.replace(tmp, self.path)
        try:
            dfd = os.open(str(self.path.parent), os.O_RDONLY)
            try:
                os.fsync(dfd)
            finally:
                os.close(dfd)
        except OSError:
            pass

    def _user(self, user: str) -> dict:
        users = self.data.setdefault("users", {})
        rec = users.get(user)
        if not isinstance(rec, dict):
            rec = {"fail_count": 0, "locked": False}
            users[user] = rec
        rec.setdefault("fail_count", 0)
        rec.setdefault("locked", False)
        return rec

    def status(self, user: str) -> dict:
        with self.lock:
            rec = self._user(user)
            return {
                "ok": True,
                "user": user,
                "fail_count": int(rec.get("fail_count", 0)),
                "locked": bool(rec.get("locked", False)),
            }

    def record_failure(self, user: str, max_failures: int) -> dict:
        with self.lock:
            rec = self._user(user)
            rec["fail_count"] = int(rec.get("fail_count", 0)) + 1
            if max_failures > 0 and rec["fail_count"] >= max_failures:
                rec["locked"] = True
            self._save()
            return {
                "ok": False,
                "user": user,
                "fail_count": rec["fail_count"],
                "locked": rec["locked"],
            }

    def record_success(self, user: str) -> dict:
        with self.lock:
            rec = self._user(user)
            rec["fail_count"] = 0
            rec["locked"] = False
            self._save()
            return {"ok": True, "user": user, "fail_count": 0, "locked": False}


class BackendError(Exception):
    pass


class MockBackend:
    """Deterministic backend for tests.  BIOAUTH_MOCK_DELAY makes a verify take
    that many seconds so the switch/cancel path can be exercised; cancel()
    interrupts it, exactly like the real backend."""

    name = "mock"

    def __init__(self, mode: str = "fail", delay: float = 0.0):
        self.mode = mode
        self.delay = max(0.0, float(delay))
        self.lock = threading.Lock()          # serializes verify(), held during the scan
        self.book = threading.Lock()          # request/cancel bookkeeping only
        self.cancel_event = threading.Event()
        self.current_req: str | None = None
        self.pending_cancels: dict[str, None] = {}

    def _mark_request_locked(self, req: str | None) -> bool:
        self.current_req = req
        self.cancel_event.clear()
        if req is not None and req in self.pending_cancels:
            self.pending_cancels.pop(req, None)
            return True
        return False

    def cancel(self, req: str | None = None) -> bool:
        with self.book:
            if req is not None:
                if req == self.current_req:
                    self.cancel_event.set()
                else:
                    self.pending_cancels[req] = None
                    while len(self.pending_cancels) > 64:
                        self.pending_cancels.pop(next(iter(self.pending_cancels)))
                return True
            self.cancel_event.set()
            return True

    def verify(self, user: str, timeout: int,
               req: str | None = None) -> tuple[bool, str]:
        with self.lock:
            with self.book:
                if self._mark_request_locked(req):
                    return False, "canceled"
            start = time.monotonic()
            deadline = start + self.delay
            scan_deadline = deadline
            if timeout and timeout > 0:
                scan_deadline = min(deadline, start + timeout)
            while time.monotonic() < scan_deadline:
                if self.cancel_event.wait(0.05):
                    return False, "canceled"
            if scan_deadline < deadline:
                # modelled the same way as the real backend: the scan timed out
                # before a finger was presented, this is not a mismatch.
                return False, "timeout"
            if self.mode == "success":
                return True, ""
            if self.mode == "device-error":
                return False, "device-error"
            return False, "no-match"


class FprintdBackend:
    """Generic fprintd backend; no device-specific code."""

    name = "fprintd"

    def __init__(self):
        self.lock = threading.Lock()
        # Prefer the system-wide queue wrapper so our daemon and direct
        # terminal/test calls do not overlap fingerprint sessions.
        self.verify_cmd = ("/usr/local/bin/fprintd-verify"
                           if os.access("/usr/local/bin/fprintd-verify", os.X_OK)
                           else "/usr/bin/fprintd-verify")
        self.active_lock = threading.Lock()
        self.proc: subprocess.Popen | None = None
        self.cancel_event = threading.Event()
        self.current_req: str | None = None
        # A cancel can arrive on its own connection before the fingerprint
        # request it belongs to has been dispatched (thread scheduling).  Keep
        # the request ids so verify() can honour an early cancel instead of
        # losing it and running a full scan that nobody waits for.
        self.pending_cancels: dict[str, None] = {}

    def _cmd(self, user: str) -> list[str]:
        # fprintd polkit rejects Claim from a runuser-created session (not the
        # user's active session). The daemon runs as root, which polkit allows.
        return [self.verify_cmd, user]

    def _mark_request(self, req: str | None) -> bool:
        """Remember the request being started; True if it was already cancelled."""
        with self.active_lock:
            self.current_req = req
            self.cancel_event.clear()
            if req is not None and req in self.pending_cancels:
                self.pending_cancels.pop(req, None)
                return True
        return False

    def cancel(self, req: str | None = None) -> bool:
        """Interrupt the verify identified by req (or any verify if req is None).

        Called from another thread than verify().  If the cancel belongs to a
        request that has not reached verify() yet it is stored and consumed by
        _mark_request(), so the early-cancel race cannot be lost.
        """
        with self.active_lock:
            if req is None:
                self.cancel_event.set()
                proc = self.proc
            elif req == self.current_req:
                self.cancel_event.set()
                proc = self.proc
            else:
                self.pending_cancels[req] = None
                while len(self.pending_cancels) > 64:
                    self.pending_cancels.pop(next(iter(self.pending_cancels)))
                proc = None
        if proc is None:
            return False
        try:
            os.killpg(proc.pid, signal.SIGINT)
        except Exception:
            pass
        return True

    def verify(self, user: str, timeout: int,
               req: str | None = None) -> tuple[bool, str]:
        with self.lock:  # fprintd is a serialized device; one at a time
            if self._mark_request(req):
                log(f"fingerprint {req} cancelled before it started")
                return False, "canceled"
            try:
                proc = subprocess.Popen(
                    self._cmd(user),
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    start_new_session=True,
                )
            except OSError as exc:
                raise BackendError(f"cannot start fprintd-verify: {exc}") from exc
            with self.active_lock:
                self.proc = proc
            deadline = time.monotonic() + timeout
            timed_out = False
            canceled = False
            out = ""
            try:
                while proc.poll() is None:
                    if self.cancel_event.is_set():
                        canceled = True
                        try:
                            os.killpg(proc.pid, signal.SIGINT)
                        except Exception:
                            pass
                        break
                    if time.monotonic() >= deadline:
                        timed_out = True
                        try:
                            os.killpg(proc.pid, signal.SIGINT)
                        except Exception:
                            pass
                        break
                    time.sleep(0.05)
                # Give fprintd-verify ~0.6s to honour SIGINT, then SIGKILL.
                # This bounds how long a cancel blocks the next fingerprint
                # request (switch fingerprint -> password -> fingerprint).
                for _ in range(12):
                    if proc.poll() is not None:
                        break
                    time.sleep(0.05)
                if proc.poll() is None:
                    try:
                        os.killpg(proc.pid, signal.SIGKILL)
                    except Exception:
                        pass
                try:
                    out, _ = proc.communicate(timeout=1)
                except subprocess.TimeoutExpired:
                    try:
                        os.killpg(proc.pid, signal.SIGKILL)
                    except Exception:
                        pass
                    out, _ = proc.communicate(timeout=1)
            finally:
                with self.active_lock:
                    self.proc = None
            if canceled:
                return False, "canceled"
            if timed_out:
                return False, "timeout"
            if proc.returncode == 0:
                return True, ""
            if "verify-no-match" in (out or ""):
                return False, "no-match"
            if any(x in (out or "") for x in (
                "failed to claim device", "Goodix command", "Open failed",
                "opening Goodix TA", "verify-unknown-error", "QSEE",
                "No prints enrolled", "no prints",
            )):
                return False, "device-error"
            return False, "error"


class PasswordBackend:
    """MVP password verification from shadow(5). Root daemon only.

    NOTE: this is deliberately isolated so it can later be replaced by a
    nested PAM helper without touching the state machine.
    """

    def __init__(self):
        self.test_password = os.environ.get("BIOAUTH_TEST_PASSWORD")

    def verify(self, user: str, password: str) -> bool:
        if self.test_password is not None:
            return hmac.compare_digest(password, self.test_password)
        try:
            sp = spwd.getspnam(user)
        except (KeyError, PermissionError):
            return False
        stored = sp.sp_pwdp
        if not stored or stored[0] in "!*":
            return False
        today = int(time.time() // 86400)
        # shadow(5): an empty expiry field is reported by spwd as -1 (not 0).
        # Only a positive expiry date in the past disables the account; treating
        # -1 as "expired" makes every password fail (found in real-hardware
        # testing: sudo password auth always said 密码错误).
        if sp.sp_expire is not None and sp.sp_expire > 0 and sp.sp_expire < today:
            log(f"password user={user} denied: account expired")
            return False
        try:
            computed = crypt.crypt(password, stored)
        except Exception:
            return False
        return bool(computed) and hmac.compare_digest(computed, stored)


class BioAuthD:
    def __init__(self, socket_path: Path, state_path: Path, backend, max_failures: int,
                 fingerprint_timeout: int, password_tries: int = 3,
                 auth_timeout: int = 120):
        self.socket_path = socket_path
        self.state = State(state_path)
        self.backend = backend
        self.passwords = PasswordBackend()
        # 0 means "no lockout" / "unlimited"; the PAM client displays it as such.
        self.max_failures = max(0, int(max_failures))
        self.fingerprint_timeout = max(1, int(fingerprint_timeout))
        self.password_tries = max(0, int(password_tries))
        self.auth_timeout = max(0, int(auth_timeout))
        self.srv: socket.socket | None = None
        self.stop_event = threading.Event()

    def _uid_of(self, user: str) -> int | None:
        try:
            return pwd.getpwnam(user).pw_uid
        except KeyError:
            return None

    def _peer_allowed(self, peer_uid: int, user: str) -> bool:
        if peer_uid == 0:
            return True
        uid = self._uid_of(user)
        return uid is not None and peer_uid == uid

    @staticmethod
    def _peer_uid(conn: socket.socket) -> int:
        try:
            raw = conn.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize("3i"))
            _pid, uid, _gid = struct.unpack("3i", raw)
            return uid
        except OSError:
            return -1

    def _dispatch(self, req: dict, peer_uid: int) -> dict:
        op = req.get("op")
        user = req.get("user")
        if not isinstance(user, str) or not user:
            return {"ok": False, "error": "missing user"}
        if not self._peer_allowed(peer_uid, user):
            return {"ok": False, "error": "permission denied"}

        if op == "status":
            st = self.state.status(user)
            st["max_failures"] = self.max_failures
            st["password_tries"] = self.password_tries
            st["auth_timeout"] = self.auth_timeout
            st["backend"] = getattr(self.backend, "name", "unknown")
            return st

        if op == "fingerprint":
            st = self.state.status(user)
            if st["locked"]:
                st["ok"] = False
                st["max_failures"] = self.max_failures
                st["reason"] = "locked"
                log(f"fingerprint user={user} refused: locked")
                return st
            req_id = req.get("req")
            if not isinstance(req_id, str) or not req_id:
                session = req.get("session")
                seq = req.get("seq")
                req_id = f"{session}:{seq}" if session is not None and seq is not None else None
            try:
                ok, reason = self.backend.verify(user, self.fingerprint_timeout, req_id)
            except BackendError as exc:
                st["ok"] = False
                st["unavailable"] = True
                st["reason"] = "unavailable"
                st["error"] = str(exc)
                st["max_failures"] = self.max_failures
                return st
            if reason == "canceled":
                st = self.state.status(user)
                st["ok"] = False
                st["canceled"] = True
                st["reason"] = "canceled"
                st["max_failures"] = self.max_failures
                log(f"fingerprint user={user} req={req_id} canceled (not counted)")
                return st
            if ok:
                self.state.record_success(user)
                log(f"fingerprint user={user} req={req_id} match")
                return {"ok": True, "user": user, "fail_count": 0,
                        "locked": False, "max_failures": self.max_failures}
            if reason == "timeout":
                # No finger was placed within fingerprint_timeout.  This is not
                # a mismatch: it must not advance the lockout counter (R7).
                st["ok"] = False
                st["timeout"] = True
                st["reason"] = "timeout"
                st["max_failures"] = self.max_failures
                log(f"fingerprint user={user} req={req_id} scan timeout; not counted")
                return st
            if reason in ("device-error", "backend-unavailable", "no-prints"):
                # Device/backend is not usable: do NOT count as a fingerprint
                # failure and do NOT lock. The PAM client switches to password.
                st["ok"] = False
                st["unavailable"] = True
                st["reason"] = "unavailable"
                st["max_failures"] = self.max_failures
                log(f"fingerprint user={user} unavailable ({reason}); not counted")
                return st
            res = self.state.record_failure(user, self.max_failures)
            res["reason"] = reason
            res["max_failures"] = self.max_failures
            log(f"fingerprint user={user} req={req_id} {reason} "
                f"fail={res['fail_count']}/{self.max_failures} locked={res['locked']}")
            return res

        if op == "password":
            raw = req.get("password_b64", "")
            try:
                password = base64.b64decode(raw, validate=True).decode("utf-8", "surrogateescape")
            except (binascii.Error, ValueError):
                return {"ok": False, "error": "bad password encoding"}
            if self.passwords.verify(user, password):
                st = self.state.record_success(user)
                st["max_failures"] = self.max_failures
                log(f"password user={user} ok (fail count reset)")
                return st
            st = self.state.status(user)
            st["max_failures"] = self.max_failures
            log(f"password user={user} invalid")
            return {"ok": False, "user": user, "error": "invalid password",
                    "fail_count": st["fail_count"], "locked": st["locked"]}

        if op == "cancel":
            req_id = req.get("req")
            if not isinstance(req_id, str) or not req_id:
                req_id = None
            hit = self.backend.cancel(req_id)
            log(f"cancel user={user} req={req_id} hit={hit}")
            return {"ok": True, "canceled": True}

        if op == "ping":
            return {"ok": True, "service": "bioauthd"}

        return {"ok": False, "error": f"unknown op: {op!r}"}

    def _handle(self, conn: socket.socket) -> None:
        peer_uid = self._peer_uid(conn)
        try:
            conn.settimeout(30)
            buf = b""
            while b"\n" not in buf and len(buf) < MAX_MESSAGE:
                chunk = conn.recv(4096)
                if not chunk:
                    return
                buf += chunk
            line, _, _ = buf.partition(b"\n")
            try:
                req = json.loads(line.decode("utf-8"))
            except Exception:
                resp = {"ok": False, "error": "bad json"}
            else:
                if not isinstance(req, dict):
                    resp = {"ok": False, "error": "bad request"}
                else:
                    resp = self._dispatch(req, peer_uid)
            data = (json.dumps(resp, ensure_ascii=False) + "\n").encode("utf-8")
            conn.sendall(data)
        except BrokenPipeError:
            # Expected: the PAM client switches to password and drops the
            # fingerprint request after cancel; the response has nowhere to go.
            pass
        except Exception as exc:
            log(f"connection error: {exc}")
        finally:
            try:
                conn.close()
            except OSError:
                pass

    def serve(self) -> None:
        if self.socket_path.exists():
            try:
                self.socket_path.unlink()
            except OSError:
                pass
        self.socket_path.parent.mkdir(parents=True, exist_ok=True)
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(str(self.socket_path))
        os.chmod(self.socket_path, 0o666)  # peer credentials enforce authorization
        srv.listen(16)
        srv.settimeout(0.5)
        self.srv = srv
        log(f"listening on {self.socket_path} backend={getattr(self.backend, 'name', '?')} "
            f"max_failures={self.max_failures}")
        while not self.stop_event.is_set():
            try:
                conn, _ = srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            threading.Thread(target=self._handle, args=(conn,), daemon=True).start()
        try:
            srv.close()
        except OSError:
            pass

    def stop(self, *_args) -> None:
        self.stop_event.set()


def load_config(path: str) -> dict:
    try:
        with open(path, "r", encoding="utf-8") as fh:
            obj = json.load(fh)
        return obj if isinstance(obj, dict) else {}
    except FileNotFoundError:
        return {}
    except Exception as exc:
        log(f"config load failed ({path}: {exc}); using defaults")
        return {}


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description="generic biometric PAM daemon (MVP)")
    p.add_argument("--config", default=os.environ.get("BIOAUTH_CONFIG", "/etc/bioauth/config.json"))
    p.add_argument("--socket", default=None)
    p.add_argument("--state", default=None)
    p.add_argument("--backend", default=None,
                   choices=["fprintd", "mock-success", "mock-fail", "mock-device-error"])
    p.add_argument("--max-failures", type=int, default=None)
    p.add_argument("--fingerprint-timeout", type=int, default=None)
    p.add_argument("--password-tries", type=int, default=None)
    p.add_argument("--auth-timeout", type=int, default=None)
    args = p.parse_args(argv)

    cfg = load_config(args.config)
    socket_path = args.socket or os.environ.get("BIOAUTH_SOCKET") or cfg.get("socket") or DEFAULT_SOCKET
    state_path = args.state or os.environ.get("BIOAUTH_STATE") or cfg.get("state_file") or DEFAULT_STATE
    backend_name = args.backend or os.environ.get("BIOAUTH_BACKEND") or cfg.get("backend") or "fprintd"
    max_failures = args.max_failures if args.max_failures is not None else int(
        os.environ.get("BIOAUTH_MAX_FAILURES", cfg.get("max_failures", DEFAULT_MAX_FAILURES)))
    fp_timeout = args.fingerprint_timeout if args.fingerprint_timeout is not None else int(
        os.environ.get("BIOAUTH_FP_TIMEOUT", cfg.get("fingerprint_timeout", DEFAULT_FINGERPRINT_TIMEOUT)))
    password_tries = args.password_tries if args.password_tries is not None else int(
        os.environ.get("BIOAUTH_PASSWORD_TRIES", cfg.get("password_tries", 3)))
    auth_timeout = args.auth_timeout if args.auth_timeout is not None else int(
        os.environ.get("BIOAUTH_AUTH_TIMEOUT", cfg.get("auth_timeout", 120)))

    try:
        mock_delay = float(os.environ.get("BIOAUTH_MOCK_DELAY", "0") or 0)
    except ValueError:
        mock_delay = 0.0
    if backend_name == "mock-success":
        backend = MockBackend("success", mock_delay)
    elif backend_name == "mock-fail":
        backend = MockBackend("fail", mock_delay)
    elif backend_name == "mock-device-error":
        backend = MockBackend("device-error", mock_delay)
    else:
        backend = FprintdBackend()

    d = BioAuthD(Path(socket_path), Path(state_path), backend,
                 max_failures, fp_timeout, password_tries, auth_timeout)
    signal.signal(signal.SIGTERM, d.stop)
    signal.signal(signal.SIGINT, d.stop)
    d.serve()
    return 0


if __name__ == "__main__":
    sys.exit(main())

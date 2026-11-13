#!/usr/bin/env python3
"""E2E harness: real pam_bioauth.so + real helper over the live Wayland session.

No root is required: a private bioauthd instance listens on a private socket and
libpam's pam_start_confdir(3) loads the private PAM config that points at the
locally built module (absolute path) and passes socket=/helper= arguments.

The helper is driven like a user would drive it: through AT-SPI (accessibility)
clicks and text entry, and verified with full-screen screenshots.
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PAM_MODULE = ROOT / "build" / "pam_bioauth.so"
PAM_DRIVER = HERE / "pam_auth.py"
SHOTS = HERE / "screenshots"
GUI_ENV = {
    "XDG_RUNTIME_DIR": "/run/user/1000",
    "WAYLAND_DISPLAY": "wayland-0",
    "DBUS_SESSION_BUS_ADDRESS": "unix:path=/run/user/1000/bus",
    "DISPLAY": "",
}
PAM_RESULT_NAMES = {
    "PAM_SUCCESS": 0, "PAM_AUTH_ERR": 7, "PAM_IGNORE": 25,
}


def gui_env():
    env = os.environ.copy()
    env.update({k: v for k, v in GUI_ENV.items() if v})
    return env


def ppid_of(pid):
    try:
        with open(f"/proc/{pid}/stat", "r", encoding="utf-8") as fh:
            data = fh.read()
        return int(data.rsplit(")", 1)[1].split()[1])
    except Exception:
        return -1


def screenshot(name):
    SHOTS.mkdir(parents=True, exist_ok=True)
    path = SHOTS / name
    subprocess.run(["gnome-screenshot", "-f", str(path)], env=gui_env(),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   timeout=20)
    return path


class Scenario:
    """One isolated bioauth stack: private daemon + private PAM conf + driver."""

    def __init__(self, backend="mock-fail", max_failures=3, fingerprint_timeout=8,
                 password_tries=3, auth_timeout=120, helper=None,
                 mock_delay=0.0, ready_timeout=5000):
        self.tmp = Path(tempfile.mkdtemp(prefix="bioauth-e2e-"))
        self.sock = self.tmp / "bioauth.sock"
        self.state = self.tmp / "state.json"
        self.confdir = self.tmp / "pam.d"
        self.confdir.mkdir()
        self.helper = helper or str(ROOT / "gui" / "bioauth-ui")
        self.ready_timeout = ready_timeout
        if helper is None and not Path(self.helper).exists():
            self.helper = "/usr/local/libexec/bioauth-ui"
        env = os.environ.copy()
        env.update({
            "BIOAUTH_SOCKET": str(self.sock),
            "BIOAUTH_STATE": str(self.state),
            "BIOAUTH_BACKEND": backend,
            "BIOAUTH_MAX_FAILURES": str(max_failures),
            "BIOAUTH_FP_TIMEOUT": str(fingerprint_timeout),
            "BIOAUTH_PASSWORD_TRIES": str(password_tries),
            "BIOAUTH_AUTH_TIMEOUT": str(auth_timeout),
            "BIOAUTH_TEST_PASSWORD": "testpass",
            "BIOAUTH_MOCK_DELAY": str(mock_delay),
        })
        self.daemon_log = self.tmp / "daemon.log"
        self._daemon_fh = open(self.daemon_log, "w", encoding="utf-8")
        self.daemon = subprocess.Popen(
            [sys.executable, str(ROOT / "daemon" / "bioauthd.py"),
             "--socket", str(self.sock), "--state", str(self.state),
             "--backend", backend, "--max-failures", str(max_failures),
             "--fingerprint-timeout", str(fingerprint_timeout),
             "--password-tries", str(password_tries),
             "--auth-timeout", str(auth_timeout)],
            env=env, stdout=self._daemon_fh, stderr=subprocess.STDOUT, text=True)
        self._wait_socket()
        self.pam_log = self.tmp / "pam.log"
        self.ui_log = self.tmp / "ui.log"
        # Same control line as /etc/pam.d/sudo, plus a stand-in for common-auth
        # (pam_permit) so PAM_IGNORE really falls through to the rescue stack.
        (self.confdir / "bioauth-dev").write_text(
            "auth     [success=done default=die ignore=ignore]  "
            f"{PAM_MODULE} socket={self.sock} helper={self.helper} "
            f"ready_timeout={self.ready_timeout} debug\n"
            "auth     sufficient  pam_permit.so\n",
            encoding="utf-8")
        self.auth_proc = None

    # -- lifecycle ---------------------------------------------------
    def _wait_socket(self):
        deadline = time.time() + 10
        while time.time() < deadline:
            if self.sock.exists():
                return
            if self.daemon.poll() is not None:
                out = self.daemon.stdout.read() if self.daemon.stdout else ""
                raise RuntimeError(f"daemon exited early:\n{out}")
            time.sleep(0.05)
        raise RuntimeError("daemon socket did not appear")

    def stop(self):
        for proc in (self.auth_proc, self.daemon):
            if proc is None:
                continue
            try:
                proc.terminate()
                proc.wait(timeout=3)
            except Exception:
                try:
                    proc.kill()
                except Exception:
                    pass
        try:
            self._daemon_fh.close()
        except Exception:
            pass
        shutil.rmtree(self.tmp, ignore_errors=True)

    # -- auth driver ------------------------------------------------
    def start_auth(self, tty=None, timeout=90):
        env = gui_env()
        env.update({
            "BIOAUTH_PAM_DEBUG": "1",
            "BIOAUTH_PAM_LOG": str(self.pam_log),
            "BIOAUTH_UI_LOG": str(self.ui_log),
        })
        cmd = [sys.executable, str(PAM_DRIVER), "--confdir", str(self.confdir),
               "--service", "bioauth-dev", "--user", "caitming"]
        if tty:
            cmd += ["--tty", tty]
        self.auth_proc = subprocess.Popen(
            cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True)
        return self.auth_proc

    def wait_auth(self, timeout=90):
        assert self.auth_proc is not None
        try:
            out, _ = self.auth_proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.auth_proc.kill()
            out, _ = self.auth_proc.communicate()
            raise AssertionError(f"pam_authenticate did not finish in {timeout}s\n{out}")
        return self.auth_proc.returncode, out

    def wait_helper_pid(self, timeout=20):
        """The helper is forked by the pam_auth process; find its pid."""
        assert self.auth_proc is not None
        deadline = time.time() + timeout
        while time.time() < deadline:
            for entry in Path("/proc").iterdir():
                if not entry.name.isdigit():
                    continue
                pid = int(entry.name)
                try:
                    cmdline = (entry / "cmdline").read_bytes()
                except Exception:
                    continue
                if b"bioauth-ui" in cmdline and ppid_of(pid) == self.auth_proc.pid:
                    return pid
            if self.auth_proc.poll() is not None:
                return None
            time.sleep(0.05)
        return None

    # -- helper state ------------------------------------------------
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

    def status(self):
        return self.request({"op": "status", "user": "caitming"})

    def daemon_output(self):
        try:
            self.daemon.terminate()
            self.daemon.wait(timeout=3)
        except Exception:
            pass
        try:
            self._daemon_fh.flush()
        except Exception:
            pass
        return self.daemon_log.read_text(errors="replace") if self.daemon_log.exists() else ""

    def logs(self):
        parts = []
        try:
            self._daemon_fh.flush()
        except Exception:
            pass
        for name in ("pam.log", "ui.log", "daemon.log"):
            path = self.tmp / name
            if path.exists():
                parts.append(f"--- {name}\n" + path.read_text(errors="replace"))
        return "\n".join(parts)


def assert_no_secret_in_logs(logs, secret="testpass"):
    """The debug log must never contain the submitted password or its base64."""
    import base64
    token = base64.b64encode(secret.encode()).decode()
    for needle in (secret, token, '"value"'):
        assert needle not in logs, f"secret {needle!r} leaked into logs:\n{logs}"


def enable_a11y():
    subprocess.run(["gsettings", "set", "org.gnome.desktop.interface",
                    "toolkit-accessibility", "true"], env=gui_env(),
                   check=False)


def disable_a11y(restore=False):
    if restore:
        subprocess.run(["gsettings", "set", "org.gnome.desktop.interface",
                        "toolkit-accessibility", "false"], env=gui_env(),
                       check=False)

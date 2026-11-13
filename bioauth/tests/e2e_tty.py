#!/usr/bin/env python3
"""End-to-end TTY tests for pam_bioauth (no GUI window involved).

A pseudo-terminal is created, PAM_TTY is set to its slave, and the module's
terminal UI (fingerprint + "P" switch + "Q" cancel + hidden password prompt) is
driven from the master side.  This covers R15 (keyboard switch during scan).

Run:  python3 tests/e2e_tty.py [scenario ...]
"""
import os
import pty
import select
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import e2e_lib  # noqa: E402


class Pty:
    def __init__(self):
        self.master, self.slave = pty.openpty()
        self.path = os.ttyname(self.slave)
        self.buf = b""

    def send(self, data):
        os.write(self.master, data)

    def read_until(self, needle, timeout=10.0):
        needle = needle.encode() if isinstance(needle, str) else needle
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in self.buf:
                return True
            r, _, _ = select.select([self.master], [], [], 0.2)
            if r:
                try:
                    chunk = os.read(self.master, 4096)
                except OSError:
                    break
                if not chunk:
                    break
                self.buf += chunk
        return needle in self.buf

    def text(self):
        return self.buf.decode("utf-8", "replace")

    def close(self):
        for fd in (self.master, self.slave):
            try:
                os.close(fd)
            except OSError:
                pass


def _start(sc):
    pty_dev = Pty()
    sc.start_auth(tty=pty_dev.path)
    return pty_dev


def test_tty_switch_to_password():
    sc = e2e_lib.Scenario(backend="mock-fail", fingerprint_timeout=20,
                          mock_delay=8.0)
    p = _start(sc)
    try:
        assert p.read_until("请把手指", timeout=10), p.text()
        assert p.read_until("按 P 切换密码", timeout=5), p.text()
        p.send(b"p")  # switch during scan
        assert p.read_until("密码", timeout=5), p.text()
        p.send(b"testpass\n")
        rc, out = sc.wait_auth(timeout=40)
        assert rc == 0, f"rc={rc}\n{out}\n{p.text()}\n{sc.logs()}"
        assert sc.status()["fail_count"] == 0
        e2e_lib.assert_no_secret_in_logs(sc.logs())
    finally:
        p.close()
        sc.stop()


def test_tty_cancel():
    sc = e2e_lib.Scenario(backend="mock-fail", fingerprint_timeout=20,
                          mock_delay=8.0)
    p = _start(sc)
    try:
        assert p.read_until("请把手指", timeout=10), p.text()
        p.send(b"q")
        rc, out = sc.wait_auth(timeout=30)
        # Q = user abort -> PAM_IGNORE -> fallback stack (pam_permit in the
        # private confdir).  Under real sudo that is common-auth.
        assert rc == 0, f"rc={rc}\n{out}\n{p.text()}\n{sc.logs()}"
        assert "exit rc=25" in sc.logs(), sc.logs()
    finally:
        p.close()
        sc.stop()


def test_tty_lockout_then_password():
    sc = e2e_lib.Scenario(backend="mock-fail", max_failures=2,
                          fingerprint_timeout=20, mock_delay=0.2)
    p = _start(sc)
    try:
        assert p.read_until("指纹不匹配（1/2）", timeout=15), p.text()
        assert p.read_until("指纹连续失败", timeout=15), p.text()
        assert p.read_until("密码", timeout=10), p.text()
        p.send(b"testpass\n")
        rc, out = sc.wait_auth(timeout=40)
        assert rc == 0, f"rc={rc}\n{out}\n{p.text()}\n{sc.logs()}"
        st = sc.status()
        assert st["fail_count"] == 0 and not st["locked"], st
    finally:
        p.close()
        sc.stop()


def test_tty_idle_timeout_message():
    sc = e2e_lib.Scenario(backend="mock-fail", fingerprint_timeout=1,
                          mock_delay=5.0)
    p = _start(sc)
    try:
        assert p.read_until("未检测到手指", timeout=10), p.text()
        p.send(b"q")
        rc, out = sc.wait_auth(timeout=30)
        assert rc in (0, 25), f"rc={rc}\n{p.text()}\n{sc.logs()}"
        assert sc.status()["fail_count"] == 0
    finally:
        p.close()
        sc.stop()


SCENARIOS = [
    test_tty_switch_to_password,
    test_tty_cancel,
    test_tty_lockout_then_password,
    test_tty_idle_timeout_message,
]


def main():
    names = sys.argv[1:]
    selected = [s for s in SCENARIOS if not names or s.__name__ in names]
    failed = []
    for fn in selected:
        print(f"\n=== {fn.__name__} ===", flush=True)
        t0 = time.time()
        try:
            fn()
            print(f"PASS {fn.__name__} ({time.time() - t0:.1f}s)", flush=True)
        except Exception:
            import traceback
            traceback.print_exc()
            print(f"FAIL {fn.__name__} ({time.time() - t0:.1f}s)", flush=True)
            failed.append(fn.__name__)
    if failed:
        print("FAILED:", ", ".join(failed))
        return 1
    print("ALL E2E TTY SCENARIOS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())

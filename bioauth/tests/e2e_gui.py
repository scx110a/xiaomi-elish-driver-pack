#!/usr/bin/env python3
"""End-to-end GUI tests for pam_bioauth + bioauthd + bioauth-ui.

Each scenario runs a private bioauthd (mock backend) and a private PAM config;
the PAM module launches the real GTK4 helper on the live Wayland session.  The
test then behaves like a user: it finds the helper window through AT-SPI,
clicks the buttons, types the password, and verifies the outcome plus the
daemon state and full-screen screenshots.

Run:  python3 tests/e2e_gui.py [scenario ...]
"""
import os
import sys
import time

HERE = __import__("pathlib").Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import atspi_util  # noqa: E402
import e2e_lib  # noqa: E402

SHOT_SEQ = [0]


def shot(name):
    SHOT_SEQ[0] += 1
    return e2e_lib.screenshot(f"{SHOT_SEQ[0]:02d}-{name}.png")


def app_of(sc, timeout=20):
    """Wait until the helper's accessibility tree is actually queryable.

    AT-SPI registers a new application asynchronously; the first node can be a
    placeholder (or the a11y cache may not be populated yet), so re-resolve it
    until a frame with its content shows up."""
    hpid = sc.wait_helper_pid(timeout=timeout)
    assert hpid, f"helper process not found (auth rc={sc.auth_proc.poll()})\n" \
                 f"{sc.logs()}"
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        app = atspi_util.find_app_by_pid(hpid, timeout=2)
        if app is not None:
            try:
                if app.get_child_count() > 0 and \
                        atspi_util.find(app, role="frame", timeout=0) is not None:
                    return app
                last = atspi_util.texts(app)
            except Exception:
                pass
        if sc.auth_proc.poll() is not None and sc.auth_proc.returncode != 0:
            break
        time.sleep(0.2)
    raise AssertionError(
        f"AT-SPI tree for helper pid {hpid} not ready (last={last!r}); is "
        "toolkit-accessibility enabled?\n" + sc.logs())


def dump(app):
    return atspi_util.texts(app)


# ---------------------------------------------------------------- scenarios

def test_fingerprint_success():
    sc = e2e_lib.Scenario(backend="mock-success", mock_delay=2.0)
    try:
        sc.start_auth()
        app = app_of(sc)
        assert atspi_util.wait_text_in(app, "指纹验证", timeout=10), dump(app)
        shot("fingerprint-wait")
        rc, out = sc.wait_auth(timeout=40)
        assert rc == 0, f"rc={rc}\n{out}\n{sc.logs()}"
        assert sc.status()["fail_count"] == 0
        assert "main loop exited" in sc.logs(), sc.logs()
    finally:
        sc.stop()


def test_lockout_then_password():
    sc = e2e_lib.Scenario(backend="mock-fail", max_failures=3,
                          fingerprint_timeout=8)
    try:
        sc.start_auth()
        app = app_of(sc)
        assert atspi_util.wait_text_in(app, "指纹不匹配（1/3）", timeout=15), dump(app)
        shot("mismatch-1of3")
        # Auto-retry until the fingerprint is locked and password is promoted.
        assert atspi_util.wait_text_in(app, "已暂时停用", timeout=25), dump(app)
        shot("locked-password-only")
        assert atspi_util.wait_for_widget(app, "使用指纹", timeout=1) is None, \
            f"fingerprint switch must be hidden when locked: {dump(app)}"
        assert atspi_util.wait_for_widget(app, "验证密码", timeout=5), dump(app)
        # A wrong password must stay visible (R8/R9) and keep the form usable.
        atspi_util.type_password(app, "definitely-wrong")
        atspi_util.click_named(app, "验证密码")
        assert atspi_util.wait_text_in(app, "密码错误", timeout=5), dump(app)
        shot("wrong-password-visible")
        assert atspi_util.wait_for_widget(app, "验证密码", timeout=5), dump(app)
        # Then the correct password succeeds and resets the lockout.
        atspi_util.type_password(app, "testpass")
        atspi_util.click_named(app, "验证密码")
        rc, out = sc.wait_auth(timeout=40)
        assert rc == 0, f"rc={rc}\n{out}\n{sc.logs()}"
        st = sc.status()
        assert st["fail_count"] == 0 and not st["locked"], st
        e2e_lib.assert_no_secret_in_logs(sc.logs())
    finally:
        sc.stop()


def test_switch_during_scan():
    """Clicking 使用密码 while a fingerprint scan is running must cancel the
    scan quickly and land in password mode (R16), without counting a failure."""
    sc = e2e_lib.Scenario(backend="mock-fail", fingerprint_timeout=20,
                          mock_delay=8.0)
    # A long mock delay keeps the fingerprint request in flight.
    try:
        sc.start_auth()
        app = app_of(sc)
        assert atspi_util.wait_text_in(app, "指纹验证", timeout=10), dump(app)
        shot("scan-running")
        t0 = time.time()
        atspi_util.click_named(app, "使用密码", timeout=5)
        assert atspi_util.wait_text_in(app, "请输入密码", timeout=5), dump(app)
        elapsed = time.time() - t0
        assert elapsed < 3.0, f"switch took {elapsed:.1f}s; cancel was not effective"
        shot("switch-to-password")
        atspi_util.type_password(app, "testpass")
        atspi_util.click_named(app, "验证密码")
        rc, out = sc.wait_auth(timeout=40)
        assert rc == 0, f"rc={rc}\n{out}\n{sc.logs()}"
        st = sc.status()
        assert st["fail_count"] == 0, f"canceled scan must not count: {st}"
        logs = sc.logs()
        assert "canceled (not counted)" in logs or "canceled" in logs, logs
        e2e_lib.assert_no_secret_in_logs(logs)
    finally:
        sc.stop()


def test_switch_back_to_fingerprint():
    """password -> 使用指纹 -> fingerprint -> password must not deadlock (R6/R17)."""
    sc = e2e_lib.Scenario(backend="mock-fail", fingerprint_timeout=20,
                          max_failures=0)
    try:
        sc.start_auth()
        app = app_of(sc)
        assert atspi_util.wait_any_text(app, ["请把手指", "请再次按指纹"],
                                       timeout=10), dump(app)
        atspi_util.click_named(app, "使用密码")
        assert atspi_util.wait_text_in(app, "请输入密码", timeout=5), dump(app)
        atspi_util.click_named(app, "使用指纹")
        assert atspi_util.wait_any_text(app, ["请把手指", "请再次按指纹"], timeout=10), \
            dump(app)
        shot("switch-back-fingerprint")
        atspi_util.click_named(app, "使用密码")
        assert atspi_util.wait_text_in(app, "请输入密码", timeout=5), dump(app)
        atspi_util.type_password(app, "testpass")
        atspi_util.click_named(app, "验证密码")
        rc, out = sc.wait_auth(timeout=40)
        assert rc == 0, f"rc={rc}\n{out}\n{sc.logs()}"
    finally:
        sc.stop()


def test_helper_without_ready_falls_back():
    """A deployed-old helper (no ready event) must be detected and killed, not
    left as an invisible authentication UI.  The PAM stack then falls through to
    the password rescue module (rc=0 via pam_permit)."""
    old_helper = "/usr/local/libexec/bioauth-ui"
    if not os.path.exists(old_helper):
        print("skip: no old helper installed")
        return
    sc = e2e_lib.Scenario(backend="mock-fail", fingerprint_timeout=20,
                          helper=old_helper, ready_timeout=2000)
    try:
        sc.start_auth()
        rc, out = sc.wait_auth(timeout=30)
        assert rc == 0, f"fallback expected (rc=0), got rc={rc}\n{out}\n{sc.logs()}"
        logs = sc.logs()
        assert "not ready" in logs or "did not become ready" in logs, logs
    finally:
        sc.stop()


def test_cancel_button():
    """User abort must not hard-fail PAM: it falls through to the password
    rescue stack (PAM_IGNORE), so sudo can still prompt for a password."""
    sc = e2e_lib.Scenario(backend="mock-fail", fingerprint_timeout=20)
    try:
        sc.start_auth()
        app = app_of(sc)
        assert atspi_util.wait_text_in(app, "指纹验证", timeout=10), dump(app)
        atspi_util.click_named(app, "取消", timeout=5)
        rc, out = sc.wait_auth(timeout=30)
        assert rc == 0, f"cancel must fall through to rescue stack (rc=0), got " \
                        f"rc={rc}\n{out}\n{sc.logs()}"
        assert "exit rc=25" in sc.logs(), sc.logs()
    finally:
        sc.stop()


def test_unavailable_promotes_password():
    sc = e2e_lib.Scenario(backend="mock-device-error")
    try:
        sc.start_auth()
        app = app_of(sc)
        assert atspi_util.wait_text_in(app, "指纹不可用", timeout=15), dump(app)
        assert atspi_util.wait_for_widget(app, "使用指纹", timeout=5), dump(app)
        shot("fingerprint-unavailable")
        atspi_util.type_password(app, "testpass")
        atspi_util.click_named(app, "验证密码")
        rc, out = sc.wait_auth(timeout=40)
        assert rc == 0, f"rc={rc}\n{out}\n{sc.logs()}"
        assert sc.status()["fail_count"] == 0
    finally:
        sc.stop()


def test_password_tries_exhausted():
    sc = e2e_lib.Scenario(backend="mock-fail", password_tries=2,
                          fingerprint_timeout=20, max_failures=0)
    try:
        sc.start_auth()
        app = app_of(sc)
        assert atspi_util.wait_any_text(app, ["请把手指", "请再次按指纹"],
                                       timeout=10), dump(app)
        atspi_util.click_named(app, "使用密码")
        assert atspi_util.wait_text_in(app, "请输入密码", timeout=5), dump(app)
        atspi_util.type_password(app, "wrong")
        atspi_util.click_named(app, "验证密码")
        assert atspi_util.wait_text_in(app, "密码错误", timeout=5), dump(app)
        # The helper re-enables the form only on the next password op (after
        # PAM's 1.2 s red-text dwell); retry the submit until accepted.
        for _ in range(4):
            if sc.auth_proc.poll() is not None:
                break
            time.sleep(1.5)
            if sc.auth_proc.poll() is not None:
                break
            atspi_util.type_password(app, f"wrong-{_}")
            try:
                atspi_util.click_named(app, "验证密码", timeout=2)
            except Exception:
                continue
            if atspi_util.wait_text_in(app, "密码尝试次数已用完", timeout=2):
                break
        rc, out = sc.wait_auth(timeout=40)
        assert rc == 7, f"rc={rc}\n{out}\n{sc.logs()}"
        assert "密码尝试次数已用完" in sc.logs(), sc.logs()
    finally:
        sc.stop()


def test_idle_scan_timeout_not_counted():
    """A no-touch scan timeout must not advance fail_count / lock fingerprint,
    and a later password success must still work (R7 + auth timeout interplay)."""
    sc = e2e_lib.Scenario(backend="mock-fail", fingerprint_timeout=1,
                          max_failures=3, mock_delay=5.0)
    try:
        sc.start_auth()
        app = app_of(sc)
        assert atspi_util.wait_text_in(app, "未检测到手指", timeout=10), dump(app)
        shot("idle-scan-timeout")
        # one or two idle timeouts must leave fail_count at 0
        time.sleep(2.5)
        st = sc.status()
        assert st["fail_count"] == 0 and not st["locked"], st
        atspi_util.click_named(app, "使用密码")
        assert atspi_util.wait_text_in(app, "请输入密码", timeout=8), dump(app)
        atspi_util.type_password(app, "testpass")
        atspi_util.click_named(app, "验证密码")
        rc, out = sc.wait_auth(timeout=40)
        assert rc == 0, f"rc={rc}\n{out}\n{sc.logs()}"
        st = sc.status()
        assert st["fail_count"] == 0 and not st["locked"], st
    finally:
        sc.stop()


def test_auth_timeout():
    sc = e2e_lib.Scenario(backend="mock-success", auth_timeout=3,
                          fingerprint_timeout=20, mock_delay=20.0)
    # A long mock delay means no result before auth_timeout fires.
    try:
        sc.start_auth()
        app = app_of(sc)
        assert atspi_util.wait_text_in(app, "指纹验证", timeout=10), dump(app)
        t0 = time.time()
        rc, out = sc.wait_auth(timeout=20)
        elapsed = time.time() - t0
        assert rc == 7, f"timeout must be PAM_AUTH_ERR(7), rc={rc}\n{out}\n{sc.logs()}"
        assert 2.0 <= elapsed <= 8.0, f"auth_timeout=3 but took {elapsed:.1f}s"
        shot("auth-timeout")
    finally:
        sc.stop()


SCENARIOS = [
    test_fingerprint_success,
    test_lockout_then_password,
    test_switch_during_scan,
    test_switch_back_to_fingerprint,
    test_helper_without_ready_falls_back,
    test_cancel_button,
    test_unavailable_promotes_password,
    test_password_tries_exhausted,
    test_idle_scan_timeout_not_counted,
    test_auth_timeout,
]


def main():
    names = sys.argv[1:]
    selected = [s for s in SCENARIOS if not names or s.__name__ in names]
    if not selected:
        print("unknown scenario; available:", ", ".join(s.__name__ for s in SCENARIOS))
        return 2
    e2e_lib.enable_a11y()
    failed = []
    for fn in selected:
        print(f"\n=== {fn.__name__} ===", flush=True)
        t0 = time.time()
        try:
            fn()
            print(f"PASS {fn.__name__} ({time.time() - t0:.1f}s)", flush=True)
        except Exception as exc:
            import traceback
            traceback.print_exc()
            print(f"FAIL {fn.__name__} ({time.time() - t0:.1f}s)", flush=True)
            failed.append(fn.__name__)
    print()
    if failed:
        print("FAILED:", ", ".join(failed))
        return 1
    print("ALL E2E GUI SCENARIOS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/python3
"""Reusable driver for gui/bioauth-ui -- mock PAM parent + screenshots + AT-SPI.

Why this exists
---------------
bioauth-ui normally runs as a child of pam_bioauth.so: the PAM module creates a
socketpair, forks, dup2()s the child end to fd 3 and exec()s the helper.  This
driver reproduces exactly that contract so the helper can be exercised on a
real Wayland session without root, without PAM and without touching the
installed (root-owned) copy.

Usage
-----
  python3 tests/helper_driver.py gallery     # screenshots + geometry checks
  python3 tests/helper_driver.py lifecycle   # exit on success/EOF/timeout/cancel
  python3 tests/helper_driver.py interact    # AT-SPI: click/type, duplicate submit
  python3 tests/helper_driver.py all         # everything

Screenshots land in tests/screenshots/ and are full-screen (2560x1600 here),
so they show whether the dim overlay really covers the screen.

Environment
-----------
The session values below are the ones of user caitming's GNOME 46 Wayland
session on this machine; override with BIOAUTH_TEST_XDG_RUNTIME_DIR etc. if
needed.  The driver never uses sudo and never edits installed files.
"""
import argparse
import json
import os
import select
import signal
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
GUI_DIR = os.path.dirname(HERE)
DEFAULT_HELPER = os.path.join(GUI_DIR, "bioauth-ui")
SHOT_DIR = os.path.join(HERE, "screenshots")

DISPLAY_ENV = {
    "XDG_RUNTIME_DIR": os.environ.get(
        "BIOAUTH_TEST_XDG_RUNTIME_DIR", "/run/user/1000"
    ),
    "WAYLAND_DISPLAY": os.environ.get("BIOAUTH_TEST_WAYLAND_DISPLAY", "wayland-0"),
    "DBUS_SESSION_BUS_ADDRESS": os.environ.get(
        "BIOAUTH_TEST_DBUS_ADDRESS", "unix:path=/run/user/1000/bus"
    ),
}

READY = "ready"
PASSWORD = "password"
CANCEL = "cancel"
SWITCH_PASSWORD = "switch_password"
SWITCH_FINGERPRINT = "switch_fingerprint"


def base_env(extra=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith("BIOAUTH_UI_")}
    env.update(DISPLAY_ENV)
    # Make sure GTK does not try X11 if a stale DISPLAY is exported.
    if "DISPLAY" in env and not env.get("DISPLAY"):
        del env["DISPLAY"]
    if extra:
        env.update(extra)
    return env


class HelperSession:
    """One bioauth-ui process with its mock PAM parent end of the fd3 socket."""

    def __init__(self, helper=DEFAULT_HELPER, log_path=None, env_extra=None):
        self.helper = helper
        self.log_path = log_path
        self.parent_sock, child_sock = socket.socketpair(
            socket.AF_UNIX, socket.SOCK_STREAM
        )
        parent_fd = self.parent_sock.fileno()
        child_fd = child_sock.fileno()
        env = base_env(env_extra)
        if log_path:
            try:  # start every session with a fresh log file
                os.unlink(log_path)
            except FileNotFoundError:
                pass
            env["BIOAUTH_UI_LOG"] = log_path
        pid = os.fork()
        if pid == 0:  # child: become the PAM module
            try:
                os.close(parent_fd)
                os.dup2(child_fd, 3)
                if child_fd != 3:
                    os.close(child_fd)
                os.execve(self.helper, ["bioauth-ui"], env)
            finally:
                os._exit(127)
        self.pid = pid
        os.close(child_fd)
        self.parent_sock.settimeout(0.15)
        self.file = self.parent_sock.makefile("rwb", buffering=0)
        self.events = []          # every parsed event received so far
        self.eof = False
        self._buf = b""

    # ---------------------------------------------------------------- I/O
    def send(self, obj):
        data = (json.dumps(obj, ensure_ascii=False) + "\n").encode("utf-8")
        try:
            self.file.write(data)
        except OSError as exc:
            print("PARENT: write failed: %r" % (exc,), flush=True)
            return False
        print("PARENT -> %s" % json.dumps(obj, ensure_ascii=False), flush=True)
        return True

    def read_events(self, duration=0.5, until=None):
        """Read for `duration` seconds; return events parsed in this call.

        Uses select()+recv() on the raw socket instead of a buffered file:
        a file object left in a timed-out state silently drops later data
        (that bug made a real submit event invisible to the test harness).
        """
        deadline = time.time() + duration
        fresh = []
        while time.time() < deadline:
            remaining = max(0.0, deadline - time.time())
            readable, _, _ = select.select([self.parent_sock], [], [], remaining)
            if not readable:
                break
            try:
                chunk = self.parent_sock.recv(65536)
            except OSError:
                self.eof = True
                break
            if not chunk:
                self.eof = True
                break
            self._buf += chunk
            while b"\n" in self._buf:
                raw, self._buf = self._buf.split(b"\n", 1)
                text = raw.decode("utf-8", "replace").strip()
                if not text:
                    continue
                try:
                    event = json.loads(text)
                except json.JSONDecodeError:
                    print("PARENT: non-json line %r" % text, flush=True)
                    continue
                self.events.append(event)
                fresh.append(event)
                print("HELPER -> %s" % text, flush=True)
                if until and event.get("event") == until:
                    return fresh
        return fresh

    def wait_event(self, name, timeout=10.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            rest = max(0.05, deadline - time.time())
            fresh = self.read_events(min(0.5, rest), until=name)
            for event in fresh:
                if event.get("event") == name:
                    return event
            if self.eof:
                return None
        return None

    def event_names(self):
        return [e.get("event") for e in self.events]

    def count(self, name):
        return sum(1 for e in self.events if e.get("event") == name)

    # ----------------------------------------------------------- process
    def running(self):
        try:
            pid, _ = os.waitpid(self.pid, os.WNOHANG)
        except ChildProcessError:
            return False
        return pid == 0

    def wait_exit(self, timeout=5.0):
        """Return (exit_status_or_negative_signal, elapsed_seconds) or (None, t)."""
        start = time.time()
        while time.time() - start < timeout:
            try:
                pid, status = os.waitpid(self.pid, os.WNOHANG)
            except ChildProcessError:
                return None, time.time() - start
            if pid != 0:
                if os.WIFEXITED(status):
                    return os.WEXITSTATUS(status), time.time() - start
                if os.WIFSIGNALED(status):
                    return -os.WTERMSIG(status), time.time() - start
                return None, time.time() - start
            time.sleep(0.03)
        return None, time.time() - start

    def kill(self):
        try:
            os.kill(self.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            os.waitpid(self.pid, 0)
        except ChildProcessError:
            pass
        try:
            self.file.close()
        except Exception:
            pass
        try:
            self.parent_sock.close()
        except Exception:
            pass

    def close_writer(self):
        """Close our end of fd3 -> the helper sees EOF (dead PAM module)."""
        try:
            self.file.close()
        except Exception:
            pass
        try:
            self.parent_sock.close()
        except Exception:
            pass

    # -------------------------------------------------------- screenshots
    def screenshot(self, name):
        os.makedirs(SHOT_DIR, exist_ok=True)
        path = os.path.join(SHOT_DIR, name)
        subprocess.run(
            ["gnome-screenshot", "-f", path],
            env=base_env(),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=20,
        )
        ok = os.path.exists(path) and os.path.getsize(path) > 0
        print(
            "SHOT %s (%s)" % (path, "ok" if ok else "MISSING"),
            flush=True,
        )
        return path if ok else None

    def read_log(self):
        if not self.log_path or not os.path.exists(self.log_path):
            return ""
        try:
            with open(self.log_path, "r", encoding="utf-8", errors="replace") as fh:
                return fh.read()
        except OSError:
            return ""


# --------------------------------------------------------------------------
# AT-SPI helpers (available via python3 GI; the standalone pyatspi is not
# installed on this machine).
# --------------------------------------------------------------------------
def atspi():
    import gi

    gi.require_version("Atspi", "2.0")
    from gi.repository import Atspi

    Atspi.init()
    return Atspi


def ensure_a11y():
    subprocess.run(
        ["gsettings", "set", "org.gnome.desktop.interface", "toolkit-accessibility", "true"],
        env=base_env(),
        check=False,
        capture_output=True,
        timeout=10,
    )


def walk(node, depth=0):
    yield node, depth
    if depth > 12:
        return
    try:
        count = node.get_child_count()
    except Exception:
        return
    for index in range(count):
        try:
            child = node.get_child_at_index(index)
        except Exception:
            continue
        if child is not None:
            yield from walk(child, depth + 1)


def app_node(Atspi, timeout=8.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        desktop = Atspi.get_desktop(0)
        for index in range(desktop.get_child_count()):
            node = desktop.get_child_at_index(index)
            if node is None:
                continue
            try:
                name = node.get_name() or ""
            except Exception:
                continue
            if "bioauth" in name.lower():
                return node
        time.sleep(0.2)
    return None


def find_nodes(root, *, name=None, role=None, substring=False):
    out = []
    for node, _depth in walk(root):
        try:
            node_role = node.get_role()
        except Exception:
            node_role = None
        if role is not None and node_role != role:
            continue
        if name is not None:
            try:
                node_name = node.get_name() or ""
            except Exception:
                node_name = ""
            if substring:
                if name not in node_name:
                    continue
            elif node_name != name:
                continue
        out.append(node)
    return out


def node_enabled(node, Atspi):
    try:
        return bool(node.get_state_set().contains(Atspi.StateType.ENABLED))
    except Exception:
        return None


def find_entries(Atspi, app):
    """GTK4 Gtk.Entry exports the ATK role 'text' (not PASSWORD_TEXT)."""
    found = []
    for node, _depth in walk(app):
        try:
            role_name = node.get_role_name()
        except Exception:
            continue
        if role_name in ("text", "password text", "entry"):
            found.append(node)
    return found


def do_click(node):
    try:
        action = node.get_action_iface()
    except Exception:
        action = None
    if action is None:
        return False
    try:
        count = action.get_n_actions()
    except Exception:
        count = 0
    if count <= 0:
        return False
    for index in range(count):
        try:
            name = action.get_action_name(index) or ""
        except Exception:
            name = ""
        if name.lower() in ("click", "activate", "press"):
            try:
                return bool(action.do_action(index))
            except Exception:
                return False
    try:
        return bool(action.do_action(0))
    except Exception:
        return False


def set_text(node, text):
    try:
        editable = node.get_editable_text_iface()
    except Exception:
        editable = None
    if editable is not None:
        try:
            editable.set_text_contents(text)
            return True
        except Exception:
            pass
    return False


# --------------------------------------------------------------------------
# Scenarios
# --------------------------------------------------------------------------
def scenario_gallery(helper, log_path):
    print("\n=== gallery: fullscreen overlay + state screenshots ===", flush=True)
    baseline = take_desktop_baseline()
    session = HelperSession(helper, log_path=log_path)
    try:
        ready = session.wait_event(READY, timeout=10)
        assert ready, "helper never sent ready event"
        assert ready.get("proto") == 2, "ready event must carry proto=2: %r" % (ready,)
        session.send({"op": "start", "service": "bioauth-test"})
        session.send({"op": "fingerprint_wait", "text": "请把手指放在指纹传感器上…"})
        time.sleep(0.8)
        session.screenshot("01_fingerprint.png")

        session.send(
            {"op": "fingerprint_result", "ok": False, "text": "指纹不匹配（1/3）。"}
        )
        time.sleep(0.5)
        session.screenshot("02_fingerprint_mismatch.png")

        session.send({"op": "fingerprint_wait", "text": "请把手指放在指纹传感器上…"})
        time.sleep(0.4)
        session.screenshot("03_fingerprint_retry_persistent.png")

        session.send(
            {"op": "password", "text": "已切换到密码验证。", "allow_fingerprint": True}
        )
        time.sleep(0.6)
        session.screenshot("04_password_mode.png")

        session.send(
            {"op": "password_result", "ok": False, "text": "密码错误（1/3）"}
        )
        time.sleep(0.5)
        session.screenshot("05_password_mismatch.png")

        session.send(
            {
                "op": "password",
                "text": "指纹已暂时停用（达到失败上限），请使用密码。",
                "allow_fingerprint": False,
            }
        )
        time.sleep(0.6)
        session.screenshot("06_locked_no_fingerprint.png")

        session.send(
            {"op": "password", "text": "指纹不可用，请使用密码。", "allow_fingerprint": True}
        )
        time.sleep(0.6)
        session.screenshot("07_fingerprint_unavailable.png")

        session.send({"op": "success", "text": "验证通过"})
        time.sleep(0.25)
        session.screenshot("08_success_green.png")
        status, elapsed = session.wait_exit(timeout=4.0)
        print(
            "EXIT after success: status=%r elapsed=%.2fs" % (status, elapsed),
            flush=True,
        )

        analysis = analyse_gallery(baseline)
        for key, value in analysis.items():
            print("CHECK %s: %s" % (key, value), flush=True)
        return {
            "ready_event": ready,
            "exit_status": status,
            "exit_elapsed": elapsed,
            "analysis": analysis,
        }
    finally:
        session.kill()


def take_desktop_baseline():
    os.makedirs(SHOT_DIR, exist_ok=True)
    path = os.path.join(SHOT_DIR, "00_desktop_baseline.png")
    subprocess.run(
        ["gnome-screenshot", "-f", path],
        env=base_env(),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=20,
    )
    return path if os.path.exists(path) else None


def analyse_gallery(baseline=None):
    """Verify the overlay covers the whole screen and the card is centred.

    The shipped overlay is intentionally opaque near-black: on this mutter
    session a *fullscreen* translucent client is direct-scanned-out and its
    alpha renders black, so the verified contract is "opaque dark surface
    covering every pixel of the output, including the GNOME top bar, with a
    centered elevated card".  The baseline screenshot is used to prove that
    the desktop/panel pixels which were bright before are gone afterwards.
    """
    results = {}
    try:
        from PIL import Image
    except ImportError:
        return {"error": "PIL unavailable"}
    shot = os.path.join(SHOT_DIR, "01_fingerprint.png")
    if not os.path.exists(shot):
        return {"error": "missing 01_fingerprint.png"}
    image = Image.open(shot).convert("RGB")
    width, height = image.size
    results["screen"] = "%dx%d" % (width, height)

    # (1) whole-output coverage: the brightest pixel anywhere on the output
    # must belong to the card/icon/text, never to an undimmed desktop pixel.
    # The card is #1c1d22 (luma ~30); bright text/icons are ~150-255 but only
    # inside the centred card.  Sample a grid and require the four corners and
    # the screen edges (including the top bar strip) to be uniform dark.
    edge_points = []
    for fx, fy in (
        (0.001, 0.001),
        (0.999, 0.001),
        (0.001, 0.999),
        (0.999, 0.999),
        (0.5, 0.001),
        (0.001, 0.5),
        (0.999, 0.5),
        (0.5, 0.999),
        (0.1, 0.02),
        (0.9, 0.02),
    ):
        edge_points.append((int(fx * (width - 1)), int(fy * (height - 1))))
    edge_luma = [sum(image.getpixel(p)) / 3.0 for p in edge_points]
    edge_colors = [image.getpixel(p) for p in edge_points]
    results["bg_luma_min"] = round(min(edge_luma), 1)
    results["bg_luma_max"] = round(max(edge_luma), 1)
    results["bg_channel_spread"] = max(
        max(c) - min(c) for c in zip(*edge_colors)
    )
    results["corners_covered"] = max(edge_luma) < 60 and results["bg_channel_spread"] <= 2
    # (2) card centred and roughly 460 logical px wide (scale 2 -> ~920 px):
    # find the contiguous run of card-coloured pixels on the centre row/column.
    def runs(values, low, high):
        found, start = [], None
        for index, value in enumerate(values):
            inside = low <= value <= high
            if inside and start is None:
                start = index
            elif not inside and start is not None:
                found.append((start, index - 1))
                start = None
        if start is not None:
            found.append((start, len(values) - 1))
        return found

    row = [sum(image.getpixel((x, height // 2))) / 3.0 for x in range(width)]
    col = [sum(image.getpixel((width // 2, y))) / 3.0 for y in range(height)]
    row_runs = [r for r in runs(row, 18, 42) if r[1] - r[0] > 300]
    col_runs = [r for r in runs(col, 18, 42) if r[1] - r[0] > 100]
    if row_runs:
        run = max(row_runs, key=lambda r: r[1] - r[0])
        results["card_width_px"] = run[1] - run[0] + 1
        center = (run[0] + run[1]) / 2.0
        results["card_center_offset_px"] = round(center - width / 2.0, 1)
        results["card_centered"] = abs(center - width / 2.0) < 30
    card_bbox = None
    if col_runs:
        run = max(col_runs, key=lambda r: r[1] - r[0])
        results["card_height_px"] = run[1] - run[0] + 1
        card_bbox = (run[0], run[1])
    if row_runs:
        run = max(row_runs, key=lambda r: r[1] - r[0])
        results["card_x_range"] = [run[0], run[1]]

    # (3) baseline comparison: every bright desktop/panel pixel outside the
    # card must have been replaced by the dark overlay (i.e. really covered).
    if baseline and os.path.exists(baseline):
        base = Image.open(baseline).convert("RGB")
        x_range = results.get("card_x_range") or [width // 2, width // 2]
        bright_before = 0
        covered = 0
        for x in range(20, width, max(1, width // 40)):
            for y in range(20, height, max(1, height // 40)):
                if card_bbox and x_range[0] - 20 <= x <= x_range[1] + 20 and card_bbox[0] - 20 <= y <= card_bbox[1] + 20:
                    continue  # card text/icon is legitimately bright
                before = sum(base.getpixel((x, y))) / 3.0
                after = sum(image.getpixel((x, y))) / 3.0
                if before > 80:
                    bright_before += 1
                    if after < 60:
                        covered += 1
        results["bright_baseline_samples_outside_card"] = bright_before
        results["bright_baseline_covered_pct"] = (
            round(100.0 * covered / bright_before, 1) if bright_before else None
        )
    return results


def scenario_lifecycle(helper, log_path):
    print("\n=== lifecycle: success / EOF / timeout / cancel ===", flush=True)
    results = {}

    # success: destroy ~1 s after green, not on EOF
    log = log_path + ".success.log"
    session = HelperSession(helper, log_path=log)
    try:
        assert session.wait_event(READY, timeout=10), "no ready (success case)"
        session.send({"op": "start", "service": "bioauth-test"})
        session.send({"op": "fingerprint_wait", "text": "请把手指放在指纹传感器上…"})
        time.sleep(0.4)
        # PAM sends fingerprint_result ok and then success - only one timer.
        session.send({"op": "fingerprint_result", "ok": True, "text": "指纹验证成功"})
        session.send({"op": "success", "text": "验证通过"})
        status, elapsed = session.wait_exit(timeout=4.0)
        results["success_exit"] = {"status": status, "elapsed": round(elapsed, 2)}
        results["success_single_timer"] = session.read_log().count(
            "success dwell elapsed"
        )
    finally:
        session.kill()

    # EOF without success -> prompt exit
    log = log_path + ".eof.log"
    session = HelperSession(helper, log_path=log)
    try:
        assert session.wait_event(READY, timeout=10), "no ready (eof case)"
        session.send({"op": "start", "service": "bioauth-test"})
        session.send({"op": "fingerprint_wait", "text": "请把手指放在指纹传感器上…"})
        time.sleep(0.5)
        session.close_writer()
        status, elapsed = session.wait_exit(timeout=3.0)
        results["eof_exit"] = {"status": status, "elapsed": round(elapsed, 2)}
    finally:
        session.kill()

    # 180 s safety timeout path, shortened through the documented test hook
    log = log_path + ".timeout.log"
    session = HelperSession(
        helper, log_path=log, env_extra={"BIOAUTH_UI_TIMEOUT": "3"}
    )
    try:
        assert session.wait_event(READY, timeout=10), "no ready (timeout case)"
        session.send({"op": "start", "service": "bioauth-test"})
        session.send({"op": "fingerprint_wait", "text": "请把手指放在指纹传感器上…"})
        status, elapsed = session.wait_exit(timeout=6.0)
        fresh = session.read_events(0.2)
        results["timeout_exit"] = {
            "status": status,
            "elapsed": round(elapsed, 2),
            "cancel_event": any(e.get("event") == CANCEL for e in session.events),
        }
    finally:
        session.kill()

    for key, value in results.items():
        print("RESULT %s: %s" % (key, value), flush=True)
    return results


def scenario_interact(helper, log_path):
    print("\n=== interact: AT-SPI click/type + duplicate-submit guard ===", flush=True)
    ensure_a11y()
    Atspi = atspi()
    log = log_path + ".interact.log"
    session = HelperSession(helper, log_path=log)
    results = {}
    try:
        ready = session.wait_event(READY, timeout=10)
        assert ready, "no ready"
        results["ready_event"] = ready
        session.send({"op": "start", "service": "bioauth-test"})
        session.send({"op": "fingerprint_wait", "text": "请把手指放在指纹传感器上…"})
        time.sleep(1.0)

        app = app_node(Atspi)
        print("AT-SPI app: %r" % (app.get_name() if app else None), flush=True)
        if app is None:
            results["error"] = "AT-SPI application not found"
            return results
        buttons = find_nodes(app, role=Atspi.Role.PUSH_BUTTON)
        print(
            "AT-SPI buttons: %r"
            % [b.get_name() for b in buttons],
            flush=True,
        )
        results["atspi_buttons_fp_mode"] = [b.get_name() for b in buttons]

        switch = next((b for b in buttons if b.get_name() == "使用密码"), None)
        if switch is None:
            results["error"] = "switch button not found via AT-SPI"
            return results
        results["switch_via_atspi"] = do_click(switch)
        session.wait_event(SWITCH_PASSWORD, timeout=5)
        session.send(
            {
                "op": "password",
                "text": "已切换到密码验证。",
                "allow_fingerprint": True,
                "attempt": 11,
            }
        )
        time.sleep(0.7)
        session.screenshot("09_atspi_password_mode.png")

        buttons = find_nodes(app, role=Atspi.Role.PUSH_BUTTON)
        entries = find_entries(Atspi, app)
        if not entries:
            results["error"] = "password entry not found via AT-SPI"
            return results
        entry = entries[0]
        results["entry_text_set"] = set_text(entry, "test-password-123")
        verify = next((b for b in buttons if b.get_name() == "验证密码"), None)
        if verify is None:
            results["error"] = "verify button not found via AT-SPI"
            return results

        before = session.count(PASSWORD)
        results["click1"] = do_click(verify)
        # Second click while PAM has not answered: must NOT emit a second
        # password event (the entry/button are disabled until the next op).
        time.sleep(0.2)
        results["click2"] = do_click(verify)
        new_events = session.read_events(0.6)
        submit_events = [e for e in new_events if e.get("event") == PASSWORD]
        results["password_events_after_double_click"] = len(submit_events)
        results["attempt_echo_1"] = submit_events[0].get("attempt") if submit_events else None
        results["attempt_echo_matches_prompt"] = (
            bool(submit_events) and submit_events[0].get("attempt") == 11
        )
        session.screenshot("10_atspi_verifying.png")

        # PAM answers with a failure; the helper must re-enable input.
        session.send({"op": "password_result", "ok": False, "text": "密码错误（1/3）"})
        time.sleep(0.6)
        session.screenshot("11_atspi_password_retry.png")
        buttons = find_nodes(app, role=Atspi.Role.PUSH_BUTTON)
        verify = next((b for b in buttons if b.get_name() == "验证密码"), None)
        entries_after_fail = find_entries(Atspi, app)
        results["verify_present_after_failure"] = verify is not None
        results["entry_present_after_failure"] = len(entries_after_fail)
        session.send(
            {
                "op": "password",
                "text": "请重新输入密码。",
                "allow_fingerprint": True,
                "attempt": 12,
            }
        )
        time.sleep(0.4)
        entries = find_entries(Atspi, app)
        results["second_text_set"] = set_text(entries[0], "test-password-456")
        session.read_events(0.3)
        buttons = find_nodes(app, role=Atspi.Role.PUSH_BUTTON)
        verify = next((b for b in buttons if b.get_name() == "验证密码"), None)
        before = session.count(PASSWORD)
        do_click(verify)
        second_events = session.read_events(0.8)
        second = [e for e in second_events if e.get("event") == PASSWORD]
        results["second_submit_after_reenable"] = len(second) == 1
        results["attempt_echo_2"] = second[0].get("attempt") if second else None

        # Cancel button -> cancel event + prompt exit.
        buttons = find_nodes(app, role=Atspi.Role.PUSH_BUTTON)
        cancel = next((b for b in buttons if b.get_name() == "取消"), None)
        results["cancel_button_found"] = cancel is not None
        if cancel is not None:
            results["cancel_click"] = do_click(cancel)
            status, elapsed = session.wait_exit(timeout=3.0)
            session.read_events(0.5)
            results["cancel_exit"] = {
                "status": status,
                "elapsed": round(elapsed, 2),
                "cancel_event": any(e.get("event") == CANCEL for e in session.events),
            }
    finally:
        session.kill()
        print("--- helper log (interact) ---", flush=True)
        print(session.read_log(), flush=True)
    for key, value in results.items():
        print("RESULT %s: %s" % (key, value), flush=True)
    return results


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "scenario",
        nargs="?",
        default="all",
        choices=["gallery", "lifecycle", "interact", "all"],
    )
    parser.add_argument("--helper", default=DEFAULT_HELPER)
    parser.add_argument(
        "--log-prefix", default="/tmp/bioauth-ui-test", help="log path prefix"
    )
    args = parser.parse_args(argv)
    os.makedirs(SHOT_DIR, exist_ok=True)

    if not os.path.exists(args.helper):
        print("helper not found: %s" % args.helper, file=sys.stderr)
        return 2
    if not os.access(args.helper, os.X_OK):
        print("helper not executable: %s" % args.helper, file=sys.stderr)
        return 2

    summary = {}
    if args.scenario in ("gallery", "all"):
        summary["gallery"] = scenario_gallery(args.helper, args.log_prefix + ".gallery.log")
    if args.scenario in ("lifecycle", "all"):
        summary["lifecycle"] = scenario_lifecycle(
            args.helper, args.log_prefix + ".lifecycle"
        )
    if args.scenario in ("interact", "all"):
        summary["interact"] = scenario_interact(
            args.helper, args.log_prefix + ".interact"
        )
    print("\n=== SUMMARY ===", flush=True)
    print(json.dumps(summary, ensure_ascii=False, indent=2), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())

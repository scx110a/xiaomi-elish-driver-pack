#!/usr/bin/env python3
"""AT-SPI helpers for driving the bioauth GTK4 helper from the outside.

Requires the user session (XDG_RUNTIME_DIR/WAYLAND_DISPLAY/DBUS session bus) and
toolkit accessibility enabled:
    gsettings set org.gnome.desktop.interface toolkit-accessibility true

The helper runs as its own process; we locate it by process id (its parent is
the PAM test driver process) and interact through the accessibility bus, which
is what an assistive technology would do.  This is a genuine user-level
interaction test, not a protocol mock.
"""
import os
import time

# AT-SPI lives on the desktop user's session bus; make the test usable from a
# bare shell (the harness runs without a graphical environment by default).
os.environ.setdefault("XDG_RUNTIME_DIR", "/run/user/1000")
os.environ.setdefault("DBUS_SESSION_BUS_ADDRESS", "unix:path=/run/user/1000/bus")
os.environ.setdefault("WAYLAND_DISPLAY", "wayland-0")

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi  # noqa: E402


def desktop():
    return Atspi.get_desktop(0)


def iter_apps():
    d = desktop()
    for i in range(d.get_child_count()):
        try:
            yield d.get_child_at_index(i)
        except Exception:
            continue


def find_app_by_pid(pid, timeout=15.0):
    """Return the AT-SPI application node whose process id == pid."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        for app in iter_apps():
            try:
                if app.get_process_id() == pid:
                    return app
            except Exception:
                continue
        time.sleep(0.2)
    return None


def find_app_by_frame(title="bioauth", timeout=15.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        for app in iter_apps():
            try:
                name = app.get_name() or ""
                if title in name:
                    return app
                for i in range(app.get_child_count()):
                    child = app.get_child_at_index(i)
                    if child.get_role_name() == "frame" and \
                            title in (child.get_name() or ""):
                        return app
            except Exception:
                continue
        time.sleep(0.2)
    return None


def walk(node, depth=0, max_depth=15):
    """Yield (depth, node) for the accessible tree."""
    try:
        yield depth, node
    except Exception:
        return
    if depth >= max_depth:
        return
    try:
        count = node.get_child_count()
    except Exception:
        return
    for i in range(count):
        try:
            child = node.get_child_at_index(i)
        except Exception:
            continue
        yield from walk(child, depth + 1, max_depth)


def find(node, name=None, role=None, timeout=0.0):
    """Find first descendant (or node itself) matching name/role."""
    deadline = time.time() + timeout
    while True:
        for _depth, n in walk(node):
            try:
                if role is not None and n.get_role_name() != role:
                    continue
                if name is not None:
                    if (n.get_name() or "") != name:
                        continue
                return n
            except Exception:
                continue
        if time.time() >= deadline:
            return None
        time.sleep(0.1)


def texts(node, max_depth=15):
    """Return a list of the accessible names/labels in the tree."""
    out = []
    for _depth, n in walk(node, max_depth=max_depth):
        try:
            text = n.get_name() or ""
        except Exception:
            continue
        if text:
            out.append(text)
    return out


def all_text_blob(node):
    return " | ".join(texts(node))


def click(node):
    """Click an accessible widget through its Action interface."""
    try:
        iface = node.get_action_iface()
    except Exception:
        iface = None
    if iface is None:
        raise RuntimeError(f"no action interface on {node.get_role_name()} "
                           f"{node.get_name()!r}")
    names = []
    for i in range(iface.get_n_actions()):
        try:
            names.append(iface.get_action_name(i))
        except Exception:
            names.append("")
    idx = 0
    for i, nm in enumerate(names):
        if nm in ("click", "press", "activate"):
            idx = i
            break
    if not iface.do_action(idx):
        raise RuntimeError(f"do_action({idx}) failed for {names}")


def click_named(root, name, timeout=5.0):
    node = find(root, name=name, timeout=timeout)
    if node is None:
        raise RuntimeError(f"widget {name!r} not found; "
                           f"texts={texts(root)}")
    click(node)
    return node


def set_text(node, text):
    """Set text in an Entry through the EditableText interface."""
    et = node.get_editable_text_iface()
    if et is None:
        raise RuntimeError(f"no editable text interface on "
                           f"{node.get_role_name()} {node.get_name()!r}")
    if not et.set_text_contents(text):
        raise RuntimeError("set_text_contents failed")


def type_password(app, password, timeout=5.0):
    """Find the password entry (role text/entry) and fill it."""
    node = find(app, role="text", timeout=timeout)
    if node is None:
        node = find(app, role="entry", timeout=0.1)
    if node is None:
        raise RuntimeError(f"password entry not found; texts={texts(app)}")
    set_text(node, password)
    return node


def wait_for_text(node, needle, timeout=8.0, absent=False):
    deadline = time.time() + timeout
    while time.time() < deadline:
        blob = all_text_blob(node)
        if (needle in blob) != absent:
            return True
        time.sleep(0.1)
    return False


def wait_text_in(node, needle, timeout=8.0):
    return wait_for_text(node, needle, timeout=timeout, absent=False)


def wait_text_out(node, needle, timeout=8.0):
    return wait_for_text(node, needle, timeout=timeout, absent=True)


def wait_any_text(node, needles, timeout=8.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        blob = all_text_blob(node)
        if any(n in blob for n in needles):
            return True
        time.sleep(0.1)
    return False


def wait_for_widget(node, name, timeout=8.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        found = find(node, name=name, timeout=0)
        if found is not None:
            return found
        time.sleep(0.1)
    return None

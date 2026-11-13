# bioauth — generic biometric PAM authentication

`bioauth` is a small, **device-agnostic** biometric authentication stack for Linux
PAM services. It is intentionally not tied to a particular fingerprint sensor,
kernel driver, SoC or vendor TA.

```
pam_bioauth.so          PAM client: TTY UI or fullscreen GTK4 helper (bioauth-ui)
        │  Unix socket + SO_PEERCRED (session/seq scoped requests)
        ▼
bioauthd (root)         state machine / lockout / password verify / cancel
        │  backend interface
        ├── backend_fprintd   fprintd-verify (queue wrapper); any libfprint device
        └── backend_mock      tests / development (BIOAUTH_MOCK_DELAY for races)
```

The GUI presentation is a single **fullscreen, undecorated, dark "secure screen"
with a centered card** — the same interaction style as Windows UAC / pkexec
prompts. One window, no multi-step dialogs, fingerprint armed immediately.
See `uac-secure-screen-20261113.md` for what this can and cannot guarantee on
Wayland (a real trusted "secure desktop" needs compositor/greeter cooperation).

## Current behaviour

Fingerprint (default, no initial choice):

- window opens with "请把手指放在指纹传感器上…"; the helper emits a
  `{"event":"ready"}` handshake once the window is actually mapped (the PAM
  module kills a helper that never becomes ready and falls back — this is what
  turns the old silent "no popup" failure into a logged, recoverable event).
- mismatch → persistent red `指纹不匹配（n/N）。` for ~1.2 s, then auto-retry.
- scan timeout (no finger placed) → `未检测到手指，请重试。` and retry; it is
  **not** counted as a mismatch and never advances the lockout counter.
- N mismatches (`max_failures`) → fingerprint disabled → password only; a
  password success resets the counter and re-enables fingerprint.
- device/backend error → `指纹不可用`, password is offered, retry button stays.
- `使用密码` immediately cancels an in-flight scan (req-scoped daemon cancel, so
  the cancel cannot hit the next attempt) and switches to the password form.
- password wrong → red `密码错误` stays visible, form stays usable;
  `password_tries` attempts per authentication (0 = unlimited).
- password success → green `验证通过` ~1 s, counter reset, window closes.
- user `取消`/close = PAM_IGNORE so the normal password stack still runs
  (sudo rescue); real failures (wrong password after retries, auth timeout)
  are PAM_AUTH_ERR.
- `auth_timeout` (0 = off) bounds the whole authentication; fingerprint retries
  and password prompts share the same deadline.

TTY mode (sudo in a terminal): same state machine; while the scan is armed the
terminal is non-canonical and **P switches to password, Q cancels** (R15). In the
password prompt, **Ctrl-]** or **F12** switches back to fingerprint; pressing
**Enter on an empty password** does the same, which also works on an on-screen
keyboard (soft keyboards cannot send Ctrl-]/F12).  **Ctrl-C** cancels and falls
back to the normal password stack, matching the GUI's cancel path.  These
gestures cannot be produced as ordinary password characters, so normal
passwords are unaffected.

## Configuration

`/etc/bioauth/config.json` (restart `bioauthd` after editing):

| key | default | meaning |
|---|---|---|
| `max_failures` | 3 | fingerprint mismatches before fingerprint is locked; **0 = never lock** |
| `password_tries` | 3 | password attempts per PAM authentication; **0 = unlimited** |
| `auth_timeout` | 120 | seconds without a successful step before auto-reject; **0 = disabled** |
| `fingerprint_timeout` | 8 | per-attempt scan timeout; a no-touch scan is not counted as a failure |
| `socket` | /run/bioauth.sock | daemon socket |
| `state_file` | /var/lib/bioauth/state.json | root-owned lockout state (0600) |

PAM reads the limits from the daemon `status` response, so they change without
editing `/etc/pam.d/*`. Never override them with `Environment=` in the systemd
unit (`bioauth-doctor` checks this).

## Install (root) and verify

```bash
make                      # build/ -> /lib/<multiarch>/security/pam_bioauth.so
make check                # daemon unit tests (no root)
make check-tty            # end-to-end PAM flow over a pty (no root, no GUI)
make check-gui            # end-to-end GUI flow on the live Wayland session (no root)
```

```bash
sudo scripts/install-bioauth.sh                # install all components, keep sudo untouched
sudo scripts/install-bioauth.sh --enable-sudo  # + insert pam_bioauth into /etc/pam.d/sudo
sudo scripts/install-bioauth.sh --rollback     # restore the sudo PAM backup
```

The script installs **module + daemon + helper + doctor from one build** (mixed
installs are the known "no popup" cause: the module requires the helper's ready
handshake and the daemon's req-scoped cancel), restarts `bioauthd`, and then runs
`bioauth-doctor` as the desktop user. The doctor checks:

- installed files match the build tree (sha256) and carry the same protocol token
- the installed helper really emits `{"event":"ready","proto":2}` on the live
  session (spawned with a mock parent, then killed)
- the installed daemon has req-scoped cancel and timeout-is-not-failure
- `/etc/pam.d/sudo` has no `debug` argument; systemd unit has no limit overrides
- daemon status responds and whether the fingerprint is currently locked out

Run it any time with `bioauth-doctor` (or `make doctor`). Exit code 1 = do not
enable for sudo yet.

## Testing without root / without touching the system

`tests/` contains a private test stack: its own `bioauthd --backend mock-*` on a
private socket plus a private PAM config directory loaded through
`pam_start_confdir(3)` (Linux-PAM >= 1.5), with a stand-in `pam_permit` rescue
module mirroring sudo's `common-auth`. No `/etc/pam.d` or system files are
touched, and the real fingerprint device is never used.

- `tests/e2e_tty.py` — pty-driven: keyboard switch/cancel, lockout, timeout.
- `tests/e2e_gui.py` — drives the real fullscreen helper through AT-SPI
  (clicks/typing like a user), asserts daemon state, and saves full-screen
  screenshots under `tests/screenshots/`.
- `tests/test_daemon.py` — state machine, req-scoped cancel, timeout non-counting.

AT-SPI automation needs `gsettings set org.gnome.desktop.interface
toolkit-accessibility true` (the test enables it).

## Troubleshooting "no popup"

1. Run `bioauth-doctor`. It tells you whether installed module/daemon/helper are
   from the same build and whether the installed helper actually becomes ready.
2. Enable debug logs (only for tests, **never** on the real sudo line):
   `BIOAUTH_PAM_DEBUG=1` in the PAM environment writes to
   `/run/bioauth/pam.log` (0600, O_NOFOLLOW; falls back to `/tmp/bioauth-pam.log`
   if the runtime dir is missing). Helper startup logs go to `BIOAUTH_UI_LOG`.
   Password values are never written (helper lines are logged by event name only).
3. If the helper is killed with "did not become ready", check the helper log for
   GTK/Wayland errors and that the PAM process runs as a user with a live
   `$XDG_RUNTIME_DIR` and a Wayland/X11 display.
4. `bioauth/audit-pam-20261113.md` has a full PAM_TTY-vs-helper_start-vs-GTK
   decision tree.

## Enabling for sudo (after the test service passes)

Keep `pkexec` untouched as the rescue path and keep a root shell open while
testing. `/etc/pam.d/sudo` line:

```pam
auth     [success=done default=die ignore=ignore]  pam_bioauth.so
@include common-auth
```

- `success=done`: fingerprint/password success ends authentication.
- `default=die`: a *real* failure (wrong password after retries, auth timeout)
  does not silently re-prompt via `common-auth`.
- `ignore=ignore`: `PAM_IGNORE` (no TTY/daemon/helper, user cancel, helper
  crash) falls through to the password stack — the rescue path.

### Enabling for pkexec / polkit (optional, after sudo is stable)

pkexec and every polkit authorization prompt on the desktop run PAM through
gnome-shell's polkit agent (`polkit-agent-helper-1` -> `/etc/pam.d/polkit-1`).
Installing `pam.d/polkit-1` makes bioauth run **first** there:

- the module runs its own fullscreen secure-screen UI, so normally only the
  bioauth overlay is shown (no GNOME password dialog);
- if the user cancels / no GUI is available / the daemon is down, the module
  returns `PAM_IGNORE` and the stack falls through to `common-auth`: the
  GNOME password dialog appears exactly as before - the rescue path;
- `prefer=gui` is required: `polkit-agent-helper-1` has no usable terminal.

```bash
sudo scripts/install-bioauth.sh --enable-pkexec   # backs up + installs /etc/pam.d/polkit-1
sudo scripts/install-bioauth.sh --rollback        # restores sudo + polkit PAM
```

Scope note: PAM cannot tell *which* polkit action is being authorized (the
helper gets no action id), so this applies to **all polkit prompts** (pkexec,
package installation, time settings, ...), not only pkexec. All of them keep
the common-auth password fallback. GDM is still untouched (stock
`gdm-fingerprint` + `pam_fprintd`).

## Security notes

- Lockout state is root-owned (`/var/lib/bioauth/state.json`, 0600, fsync'd).
- Socket is 0666 but every connection is checked with `SO_PEERCRED`: only root
  or the target user may operate on that user. Requests carry a per-auth
  `session` + `seq` / fingerprint `req` id; cancel is scoped to one request.
- Password verification in the MVP uses shadow(5) from the root daemon; it is
  isolated so it can be replaced by a nested PAM helper later.
- The GUI is a fullscreen user-session window, not a trusted secure desktop:
  same-user processes can still read input devices (the test user is in group
  `input`) or draw windows. UAC's secure desktop requires compositor/greeter
  cooperation — see `uac-secure-screen-20261113.md`.
- Debug logging must stay off in production PAM configs; it is enabled via the
  environment (`BIOAUTH_PAM_DEBUG`) and writes only event names, never secrets.
- No device-specific code: no `/dev/goodix_fp`, no TA/QSEE/`gfenu`, no
  `/lib/firmware` assumptions. If fprintd has no device, the module falls back
  to password.

## Files

```
daemon/bioauthd.py          root daemon (state machine + backends + cancel)
pam/pam_bioauth.c           PAM client (TTY + GUI, session/seq scoped requests)
gui/bioauth-ui              fullscreen GTK4 secure-screen helper (fd3 JSON protocol)
gui/tests/helper_driver.py  standalone helper driver + screenshot/AT-SPI checks
scripts/fprintd-verify-queue serialized fprintd-verify wrapper
scripts/bioauth-doctor      installation / deploy-skew / live-ready diagnostics
scripts/install-bioauth.sh  root install + verify + rollback script
config/bioauth.json         default config
systemd/bioauthd.service    systemd unit
pam.d/bioauth-test          standalone PAM test service
tests/                      unit + TTY + GUI end-to-end tests (no root)
audit-pam-20261113.md       independent code audit + no-popup diagnostics
uac-secure-screen-20261113.md  UAC/pkexec comparison + Wayland limits
```

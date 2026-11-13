# bioauth — independent verification of the fixed build (2026-11-13)

Verifier: `auditor` (independent pass; read-only w.r.t. source; only this file written +
scratch under `/tmp/audit-verify/`). No root, no system changes, no real fingerprint
device, `/etc/pam.d` and the system daemon untouched. Verification window:
2026-11-13 12:56–13:05 CST, user `caitming`, live Wayland session
(`XDG_RUNTIME_DIR=/run/user/1000`, `WAYLAND_DISPLAY=wayland-0`), exclusive GUI session
(verified `pgrep -a bioauth-ui` empty before each suite). `toolkit-accessibility=true`
was enabled by the Lead/test harness for this run; Lead restores it afterwards.

## 0. Hashes — tree was frozen and stayed frozen

Recorded before the run, recomputed after all tests: **all four match the Lead's
hand-off values, and still match after the full verification** (no source drift, no
edits by me):

| file | sha256 (hand-off == measured, before and after) |
|---|---|
| `build/pam_bioauth.so` | `fca3974837b35a845496e989a40be09d667f364b2e6831aac73e4a5801ad30f4` |
| `daemon/bioauthd.py` | `16c71a63faa9fda1ae6f934a5dd7ee96e0539cb2ded6b40e88f71b45b5af0406` |
| `gui/bioauth-ui` | `915224c65d66b15cf4020a8832f97c6a960a44d7f742e005a3c7921a43c50cfb` |
| `scripts/bioauth-doctor` | `9e7328cb63e3c38dc8c61bf77cb0ab9b49564cdace0daf33217d8ee782c1619b` |

Installed system (unchanged by this verification, and still the OLD artifacts — see §4):
`/lib/aarch64-linux-gnu/security/pam_bioauth.so ce3fd33d…`,
`/usr/local/libexec/bioauthd 3d7ad70c…`, `/usr/local/libexec/bioauth-ui 58526bd7…`.

## 1. Result: PASS — 22/22 tests, 14/14 independent spot checks, doctor findings reproduced

| # | command | result | time | log |
|---|---|---|---|---|
| 1 | `make check` | **8/8 unit tests OK** (`Ran 8 tests … OK`) | 8.7 s | `/tmp/audit-verify/make-check.log` |
| 2 | `python3 tests/e2e_tty.py` | **4/4 PASS**, `ALL E2E TTY SCENARIOS PASSED` | 8.8 s | `/tmp/audit-verify/e2e-tty.log` |
| 3 | `python3 tests/e2e_gui.py` (run 1) | **10/10 PASS**, `ALL E2E GUI SCENARIOS PASSED` | 50.6 s | `/tmp/audit-verify/e2e-gui.log` |
| 4 | `python3 tests/e2e_gui.py` (run 2, flakiness) | **10/10 PASS** again | 50.7 s | `/tmp/audit-verify/e2e-gui-run2.log` |
| 5 | independent spot checks (`/tmp/audit-verify/spotcheck.py`) | **14/14 PASS** | ~8 s | `/tmp/audit-verify/` |
| 6 | `scripts/bioauth-doctor --no-live` (actual installed system) | **FAIL as expected**: 7 failures, 1 warning, 24 checks | 0.1 s | `/tmp/audit-verify/doctor-installed.log` |
| 7 | `make install DESTDIR=/tmp/audit-verify/destdir` + `bioauth-doctor --prefix … --no-live` | **0 failures, 0 warnings, 23 checks** | 0.1 s | `/tmp/audit-verify/doctor-destdir.log` |

GUI scenario names (identical in both runs): `test_fingerprint_success` (4.1 s),
`test_lockout_then_password` (9.3 s), `test_switch_during_scan` (5.5 s),
`test_switch_back_to_fingerprint` (5.2 s), `test_helper_without_ready_falls_back`
(3.1 s), `test_cancel_button` (1.0 s), `test_unavailable_promotes_password` (4.0 s),
`test_password_tries_exhausted` (5.5 s), `test_idle_scan_timeout_not_counted` (7.9 s),
`test_auth_timeout` (5.0 s). TTY: `test_tty_switch_to_password`,
`test_tty_cancel`, `test_tty_lockout_then_password`, `test_tty_idle_timeout_message`.
AT-SPI `dbind-WARNING … GetItems/atspi/cache` lines appear on some scenarios but are
non-fatal noise; every scenario asserted UI text/behaviour and PAM rc.

## 2. Independent spot checks (not re-reading the suite's assertions)

Implemented from scratch on the frozen build + private `pam_start_confdir` stack +
mock daemon, with a protocol-faithful fake helper (`ready` proto=2, attempt echo) so
the checks are independent of GUI/AT-SPI:

**A. Password redaction / log hygiene — 6/6**
- A password flow with `wrongpass` then `testpass` over the real module: `PAM_SUCCESS`.
- `BIOAUTH_PAM_LOG` content contains `helper<- event=password` (event name only) and
  **no** `testpass`, `wrongpass`, `dGVzdHBhc3M=`, `d3JvbmdwYXNz`, or `"value"`.
- log file mode is `0600`; `BIOAUTH_UI_LOG` contains no `"value"`.
- helper observed an `attempt` id in the `{"op":"password"}` message (echo wired).
- Code-path confirmation: `plog_helper_rx()` (`pam/pam_bioauth.c:184-200`) extracts only
  `event=`/`op=`; `plog()` opens with `O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW`,
  mode `0600`, and prefers `/run/bioauth/pam.log` when writable (`:139-166`).

**B. Cancel semantics / rescue fallback — 3/3**
- Private confdir is exactly the sudo control line plus the rescue stand-in:
  `auth [success=done default=die ignore=ignore] <module> … debug` +
  `auth sufficient pam_permit.so`.
- Helper sends `{"event":"cancel"}` during the fingerprint phase →
  module log **`exit rc=25`** (PAM_IGNORE) and end-to-end `pam_authenticate rc=0`
  (falls through to the rescue stack). Hard-fail would have been rc=7.

**C. No-touch scan timeout is not a failure — 5/5**
- `mock-fail`, `BIOAUTH_MOCK_DELAY=3`, `fingerprint_timeout=1`, helper idles then cancels
  after 3.8 s: daemon log has `scan timeout; not counted` three times, status
  `fail_count=0`, `locked=false` after the run; the helper received
  `未检测到手指，请重试。` as a `fingerprint_result` (i.e. the UI message, not
  `指纹不匹配`), and the module still exited via the cancel fallback (rc=0).

## 3. Config / rescue path checks

- `/etc/pam.d/sudo` is unchanged from the hand-off state and contains **no `debug`
  argument** (`auth [success=done default=die ignore=ignore] pam_bioauth.so`, then
  `@include common-auth`), so `PAM_IGNORE` — including the new cancel path — reaches the
  normal password stack. `debug` is env-only (`BIOAUTH_PAM_DEBUG`), as designed.
- pkexec rescue intact: `/etc/pam.d/polkit-1` still does not exist (pkexec →
  `/etc/pam.d/other` → password). The install script does not touch it.
- `scripts/install-bioauth.sh` syntax OK; it backs up `/etc/pam.d/sudo`,
  module, daemon, helper and config to `/root/bioauth-sudo-backup-*`; installs
  module+daemon+helper+doctor from one build; by default leaves sudo untouched
  (`--enable-sudo` opt-in); runs `bioauth-doctor` live as the desktop user after install;
  `--rollback` restores the backup and disables the daemon.
- Installed-vs-build skew is real and reproduced by the doctor (below). This is the
  user-actionable item; the frozen workspace build is the verified one.

## 4. Doctor — deploy skew reproduced exactly as reported

Actual installed system (`--no-live`, exit 1): 7 failures / 1 warning / 24 checks:

```
FAIL module matches build tree: installed=ce3fd33d7a38 build=fca3974837b3 (installed mtime …12:23)
FAIL daemon matches build tree: installed=3d7ad70cb6d9 build=16c71a63faa9 (…12:17)
FAIL helper matches build tree: installed=58526bd7b74b build=915224c65d66 (…12:15)
FAIL module carries BIOAUTH_PROTO token
FAIL helper emits ready event
WARN helper ready carries proto=2
FAIL daemon has req-scoped cancel (pending_cancels)
FAIL daemon treats scan timeout as non-failure
summary: 7 failure(s), 1 warning(s), 24 checks
  Fix by installing module+daemon+helper from the SAME build: sudo scripts/install-bioauth.sh
```
PASSes include config parsing/keys, systemd unit not overriding limits, sudo references
pam_bioauth, sudo line has no `debug`, daemon status responds. This matches the Lead's
report; it is **expected** and is exactly the gap a root install closes.

Fresh DESTDIR (`make install DESTDIR=/tmp/audit-verify/destdir`, then
`bioauth-doctor --prefix … --no-live`, exit 0): **0 failures / 0 warnings / 23 checks**,
including module/daemon/helper byte-identical to `build/`, proto token present,
helper `ready`/`proto=2` present, daemon `pending_cancels` and non-counting timeout
present.

## 5. Adversarial / hygiene checks

- GUI suite run **twice consecutively**: 10/10 both times, same durations ±0.5 s — no
  flakiness observed; each run left **no** `bioauth-ui`/`bioauthd`/`pam_auth.py`
  processes behind (checked with `pgrep` immediately after; only the shell's own
  pattern match, no real process).
- Pre-run check: no other helper windows up (exclusive GUI session honored).
- Screenshots written by the suite: `tests/screenshots/01-fingerprint-wait.png` …
  `10-auth-timeout.png` (10 per run, ~65–590 KB each; run-2 files timestamped 12:57).
- Hashes recomputed after the second GUI run: identical to §0 (frozen).
- `toolkit-accessibility` is still `true` now (harness requirement); Lead restores it to
  its previous value (`false`) after this run — not a finding.
- Compile hygiene: build/pam_bioauth.so is the frozen artifact built with
  `-O2 -Wall -Wextra -Wformat=2`; unit tests include the two new cancel/timeout tests.

## 6. Residual risks (documented, not blockers)

1. **P7** — TTY raw mode (`ICANON`/`ECHO` off) can still be left on if the PAM process is
   SIGKILLed/crashes while the fingerprint scanner is armed; normal returns restore it.
2. **D2** — a legacy/no-`req` `cancel` can still abort whichever fingerprint scan is
   active (any local user DoS); the new module always sends `req`, so the normal path is
   scoped.
3. **D4** — daemon socket `0666` + thread-per-connection + fingerprint op permitted to
   the owning local user: local DoS/sensor-contention surface, no auth bypass.
4. **Multi-monitor** — the helper is a single fullscreen overlay; multi-monitor
   presentation/stacking is not covered by this suite.
5. **GUI overlay for sudo** — `install-bioauth.sh --enable-sudo` installs the plain
   `pam_bioauth.so` line, so `sudo` in a terminal uses the (verified) TTY keyboard UI
   (P/Q); the GTK overlay is used for no-TTY callers. Using the overlay inside terminal
   sessions requires explicitly adding `prefer=gui` (covered by the private-conf GUI
   suite, but that exact `/etc/pam.d/sudo` variant is not installed by the script).
6. **GDM/pkexec** remain untouched/out of scope. Install itself needs root; this
   verification could not and did not install anything on the system.

## 7. Verdict — safe to install for sudo?

**YES, conditionally.** The frozen build passes the full gate independently
(22/22 tests incl. two GUI runs, 14/14 independent security/behaviour spot checks,
doctor clean on a fresh prefix tree). Proceed as follows:

1. Run `sudo scripts/install-bioauth.sh` (installs module+daemon+helper+doctor from one
   build and runs the live doctor; keep pkexec untouched).
2. Require `bioauth-doctor` on the **actual** system to report `0 failure(s)` before
   enabling for sudo (it currently reports the 7 expected skew failures).
3. Then `sudo scripts/install-bioauth.sh --enable-sudo` (backup is created
   automatically) and do one supervised `sudo -k; sudo true` in a terminal: expect the
   TTY fingerprint prompt, `P` → hidden password, `Q`/cancel → falls through to the
   password stack (no hard fail).
4. Keep `/root/bioauth-sudo-backup-*` and the documented rollback
   (`sudo scripts/install-bioauth.sh --rollback`, or pkexec) until the first real sudo
   session is confirmed. Do **not** touch pkexec/GDM yet (handoff §9 red lines).
5. Run the user-facing `bioauth-doctor` (without `--no-live`) once in the live session
   to confirm the helper's `ready` handshake after install.

No blocking failures were found. Failures/oddities to carry forward are only the
residual items in §6 and the expected install-time deploy-skew report in §4.

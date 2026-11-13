#!/bin/bash
# install-bioauth.sh - install the whole bioauth stack from ONE build (root).
#
# The dangerous failure mode is installing only some of the components: the
# current PAM module requires the UI helper's {"event":"ready"} handshake and
# the req-scoped cancel support in the daemon.  This script always installs
# module + daemon + helper + doctor from the same build, then verifies the
# installation with bioauth-doctor (including the live ready handshake).
#
# By default it does NOT touch /etc/pam.d/sudo (the rescue path stays pure
# password).  Use --enable-sudo once the test service and the GUI have been
# verified.  Use --rollback to restore the backed-up sudo PAM file.
#
# Usage:
#   sudo scripts/install-bioauth.sh                 # install files, keep sudo/pkexec untouched
#   sudo scripts/install-bioauth.sh --enable-sudo   # install + enable for sudo
#   sudo scripts/install-bioauth.sh --enable-pkexec # install + enable for pkexec/polkit/polkit agents
#   sudo scripts/install-bioauth.sh --enable-sudo --enable-pkexec
#   sudo scripts/install-bioauth.sh --rollback      # restore sudo + polkit PAM backups
#   sudo scripts/install-bioauth.sh --uninstall     # remove bioauth (PAM restored first)
#   sudo scripts/install-bioauth.sh --dry-run       # print the plan, change nothing
#   sudo scripts/install-bioauth.sh --skip-build    # use existing build/
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PAM_SUDO=/etc/pam.d/sudo
BACKUP_DIR=/root/bioauth-sudo-backup-$(date +%Y%m%d-%H%M%S)
MULTIARCH="$(cc -print-multiarch 2>/dev/null || echo aarch64-linux-gnu)"
PAMMOD="/lib/${MULTIARCH}/security/pam_bioauth.so"
TARGET_USER="${SUDO_USER:-${USER:-}}"
REQUESTED_USER=""

ENABLE_SUDO=0
ENABLE_PKEXEC=0
DO_BUILD=1
ROLLBACK=0
UNINSTALL=0
DRY_RUN=0
NO_DOCTOR=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --enable-sudo) ENABLE_SUDO=1 ;;
        --enable-pkexec) ENABLE_PKEXEC=1 ;;
        --user=*) REQUESTED_USER="${1#--user=}" ;;
        --user) shift; REQUESTED_USER="${1:-}" ;;
        --no-doctor) NO_DOCTOR=1 ;;
        --skip-build) DO_BUILD=0 ;;
        --rollback) ROLLBACK=1 ;;
        --uninstall) UNINSTALL=1 ;;
        --dry-run) DRY_RUN=1 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

die() { echo "error: $*" >&2; exit 1; }

restore_pam_from_backup() {
    backup="$1"
    if [ -f "$backup/sudo.pam" ]; then
        cp -a "$backup/sudo.pam" "$PAM_SUDO"
        echo "restored $PAM_SUDO from $backup/sudo.pam"
    fi
    if [ -f "$backup/polkit-1.pam" ]; then
        cp -a "$backup/polkit-1.pam" /etc/pam.d/polkit-1
        echo "restored /etc/pam.d/polkit-1 from $backup/polkit-1.pam"
    elif [ -f "$backup/polkit-1.absent" ]; then
        rm -f /etc/pam.d/polkit-1
        echo "removed /etc/pam.d/polkit-1 (it did not exist before install)"
    fi
}

[ -n "$REQUESTED_USER" ] && TARGET_USER="$REQUESTED_USER"
# When invoked via sudo/pkexec from a helper process, SUDO_USER is set; when
# invoked directly as root, fall back to the active desktop session's user.
if [ -z "$TARGET_USER" ] || [ "$TARGET_USER" = "root" ]; then
    TARGET_USER="$(loginctl list-sessions --no-legend 2>/dev/null | awk '$3 != "" {print $3; exit}')"
fi

if [ "$DRY_RUN" -eq 1 ]; then
    if [ "$ROLLBACK" -eq 1 ]; then
        echo "== dry-run: bioauth rollback plan =="
        echo "  restore latest /root/bioauth-sudo-backup-*/sudo.pam -> /etc/pam.d/sudo"
        echo "  restore (or remove) /etc/pam.d/polkit-1 from the same backup"
        echo "  systemctl disable --now bioauthd"
        exit 0
    fi
    if [ "$UNINSTALL" -eq 1 ]; then
        echo "== dry-run: bioauth uninstall plan =="
        echo "  restore sudo + polkit PAM from latest backup, then:"
        echo "  systemctl disable --now bioauthd"
        echo "  rm module /usr/local/libexec/{bioauthd,bioauth-ui} /usr/local/bin/bioauth-doctor"
        echo "  rm /etc/bioauth/config.json /etc/systemd/system/bioauthd.service /etc/pam.d/bioauth-test"
        echo "  systemctl daemon-reload"
        echo "  (shared fprintd-verify queue / fingerprint package files untouched)"
        exit 0
    fi
    echo "== dry-run: bioauth install plan =="
    echo "  root:            $ROOT"
    echo "  target user:     ${TARGET_USER:-<unknown>}"
    echo "  build:           $([ "$DO_BUILD" -eq 1 ] && echo 'make clean all check' || echo 'skip (--skip-build)')"
    echo "  install:         module + daemon + helper + fprintd-verify queue + doctor + config"
    echo "                   -> /lib/<multiarch>/security/pam_bioauth.so, /usr/local/libexec/, /etc/bioauth/"
    echo "  service:         systemctl enable --now bioauthd"
    echo "  sudo PAM:        $([ "$ENABLE_SUDO" -eq 1 ] && echo 'insert bioauth line (backup first)' || echo 'leave untouched')"
    echo "  pkexec/polkit:   $([ "$ENABLE_PKEXEC" -eq 1 ] && echo 'install /etc/pam.d/polkit-1 (backup first)' || echo 'leave untouched')"
    echo "  doctor:          $([ "$NO_DOCTOR" -eq 1 ] && echo 'skipped' || echo 'bioauth-doctor (live helper ready + config checks)')"
    echo "  rollback:        sudo $0 --rollback"
    echo "  uninstall:       sudo $0 --uninstall"
    exit 0
fi

[ "$(id -u)" -eq 0 ] || die "must run as root (sudo $0)"

if [ "$ROLLBACK" -eq 1 ]; then
    backup="$(ls -1d /root/bioauth-sudo-backup-* 2>/dev/null | sort | tail -1)"
    [ -n "$backup" ] || die "no /root/bioauth-sudo-backup-* found"
    restore_pam_from_backup "$backup"
    systemctl disable --now bioauthd 2>/dev/null || true
    echo "rollback complete (pkexec back to pure password if it had no config)"
    exit 0
fi

if [ "$UNINSTALL" -eq 1 ]; then
    echo "== uninstall bioauth =="
    systemctl disable --now bioauthd 2>/dev/null || true
    backup="$(ls -1d /root/bioauth-sudo-backup-* 2>/dev/null | sort | tail -1)"
    if [ -n "$backup" ]; then
        restore_pam_from_backup "$backup"
    else
        warn_msg="no /root/bioauth-sudo-backup-* found; PAM files left unchanged"
        echo "warning: $warn_msg"
    fi
    rm -f /usr/local/libexec/bioauthd /usr/local/libexec/bioauth-ui \
          /usr/local/bin/bioauth-doctor "$PAMMOD" \
          /etc/bioauth/config.json /etc/systemd/system/bioauthd.service \
          /etc/pam.d/bioauth-test
    rmdir /etc/bioauth 2>/dev/null || true
    systemctl daemon-reload
    echo "bioauth removed (shared fprintd-verify queue / fingerprint package untouched)."
    echo "verify: grep -H pam_bioauth /etc/pam.d/sudo /etc/pam.d/polkit-1 2>/dev/null || true"
    exit 0
fi

cd "$ROOT"
if [ "$DO_BUILD" -eq 1 ]; then
    echo "== build =="
    make clean all check
fi
[ -f build/pam_bioauth.so ] || die "build/pam_bioauth.so missing (run: make)"
[ -x gui/bioauth-ui ] || die "gui/bioauth-ui missing"

echo "== backup current state =="
mkdir -p "$BACKUP_DIR"
[ -f "$PAM_SUDO" ] && cp -a "$PAM_SUDO" "$BACKUP_DIR/sudo.pam"
cp -a "$PAMMOD" "$BACKUP_DIR/pam_bioauth.so" 2>/dev/null || true
cp -a /usr/local/libexec/bioauth-ui "$BACKUP_DIR/bioauth-ui" 2>/dev/null || true
cp -a /usr/local/libexec/bioauthd "$BACKUP_DIR/bioauthd" 2>/dev/null || true
cp -a /etc/bioauth/config.json "$BACKUP_DIR/config.json" 2>/dev/null || true
if [ -f /etc/pam.d/polkit-1 ]; then
    cp -a /etc/pam.d/polkit-1 "$BACKUP_DIR/polkit-1.pam"
else
    : > "$BACKUP_DIR/polkit-1.absent"
fi
echo "backup dir: $BACKUP_DIR"

echo "== install (module + daemon + helper + queue + doctor + config) =="
make install

# Preserve an existing user-tuned config (make install ships the default one).
if [ -f "$BACKUP_DIR/config.json" ]; then
    cp -a "$BACKUP_DIR/config.json" /etc/bioauth/config.json
    echo "kept existing /etc/bioauth/config.json"
fi

echo "== restart daemon =="
systemctl daemon-reload
systemctl enable bioauthd
systemctl restart bioauthd
sleep 0.5

if [ "$ENABLE_PKEXEC" -eq 1 ]; then
    echo "== enable bioauth for pkexec / polkit =="
    if [ -f /etc/pam.d/polkit-1 ] && grep -q '^[^#].*pam_bioauth' /etc/pam.d/polkit-1; then
        echo "polkit-1 already references pam_bioauth; left unchanged"
    else
        make install-pkexec
        echo "installed /etc/pam.d/polkit-1 (backup: $BACKUP_DIR)"
    fi
fi

if [ "$ENABLE_SUDO" -eq 1 ]; then
    echo "== enable bioauth for sudo =="
    if grep -q '^[^#].*pam_bioauth' "$PAM_SUDO"; then
        echo "sudo already references pam_bioauth; left unchanged"
    else
        {
            echo "# bioauth: biometric auth first; PAM_IGNORE (no tty/GUI/daemon) falls through"
            echo "# to the normal password stack. Cancel/close also falls through."
            echo "auth     [success=done default=die ignore=ignore]  pam_bioauth.so"
            cat "$BACKUP_DIR/sudo.pam"
        } > "$PAM_SUDO.new"
        chmod 0644 "$PAM_SUDO.new"
        mv "$PAM_SUDO.new" "$PAM_SUDO"
        echo "inserted pam_bioauth into $PAM_SUDO (backup: $BACKUP_DIR/sudo.pam)"
    fi
fi

if [ "$NO_DOCTOR" -eq 1 ]; then
    echo "== skip bioauth-doctor (--no-doctor) =="
    echo "INSTALL_COMPLETE rc=0"
    exit 0
fi

echo "== verify installation (bioauth-doctor) =="
if [ -n "$TARGET_USER" ] && id "$TARGET_USER" >/dev/null 2>&1; then
    su - "$TARGET_USER" -c "XDG_RUNTIME_DIR=/run/user/$(id -u "$TARGET_USER") \
        /usr/local/bin/bioauth-doctor" || {
        echo
        echo "bioauth-doctor reported failures - the stack is NOT verified." >&2
        echo "Keep pkexec as the rescue path; rollback with:" >&2
        echo "  sudo $0 --rollback" >&2
        exit 1
    }
else
    echo "no desktop user to run the live helper probe as; run bioauth-doctor manually"
fi

echo "INSTALL_COMPLETE rc=0"
echo
echo "OK. Rescue paths:"
if [ -f /etc/pam.d/polkit-1 ] && grep -q '^[^#].*pam_bioauth' /etc/pam.d/polkit-1; then
    echo "  pkexec/polkit: bioauth enabled (cancel -> gnome password dialog = rescue)."
else
    echo "  pkexec/polkit: password only (enable with --enable-pkexec)."
fi
echo "  rollback sudo+polkit PAM: sudo $0 --rollback"
echo "  pkexec rescue: if pkexec itself misbehaves, sudo still works; rollback removes polkit-1."
echo "  test service:      pamtester bioauth-test $TARGET_USER authenticate"

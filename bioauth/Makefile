CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wformat=2 -fPIC
PAM_CFLAGS := $(shell pkg-config --cflags pam)
PAM_LIBS := $(shell pkg-config --libs pam)

PREFIX ?= /usr/local
MULTIARCH ?= $(shell $(CC) -print-multiarch 2>/dev/null || echo aarch64-linux-gnu)
PAMMODDIR ?= /lib/$(MULTIARCH)/security
DAEMON_DIR ?= $(PREFIX)/libexec
BIN_DIR ?= $(PREFIX)/bin
SYSTEMD_DIR ?= /etc/systemd/system
PAM_DIR ?= /etc/pam.d
CONFIG_DIR ?= /etc/bioauth

all: build/pam_bioauth.so

build/pam_bioauth.so: pam/pam_bioauth.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(PAM_CFLAGS) -shared -o $@ $< $(PAM_LIBS)

check: build/pam_bioauth.so
	python3 -m py_compile daemon/bioauthd.py gui/bioauth-ui scripts/bioauth-doctor
	python3 -m unittest discover -s tests -v

# End-to-end tests: TTY (always safe) + GUI (needs the live Wayland session).
# Both use a private mock daemon and a private PAM confdir - no root, no
# /etc/pam.d changes, no real fingerprint device.
check-tty: build/pam_bioauth.so
	python3 tests/e2e_tty.py

check-gui: build/pam_bioauth.so
	python3 tests/e2e_gui.py

check-e2e: check-tty check-gui

doctor:
	python3 scripts/bioauth-doctor

# Opt-in: route pkexec / polkit authentication through bioauth (biometric
# first, password fallback).  Kept separate from "install" so upgrading the
# stack never silently changes polkit behaviour.
install-pkexec:
	install -D -m 0644 pam.d/polkit-1 $(DESTDIR)$(PAM_DIR)/polkit-1

install: build/pam_bioauth.so
	install -D -m 0755 daemon/bioauthd.py $(DESTDIR)$(DAEMON_DIR)/bioauthd
	install -D -m 0755 gui/bioauth-ui $(DESTDIR)$(DAEMON_DIR)/bioauth-ui
	install -D -m 0755 scripts/fprintd-verify-queue $(DESTDIR)$(BIN_DIR)/fprintd-verify
	install -D -m 0755 scripts/bioauth-doctor $(DESTDIR)$(BIN_DIR)/bioauth-doctor
	install -d -m 0755 $(DESTDIR)/run/lock
	install -m 0666 /dev/null $(DESTDIR)/run/lock/fprintd-verify.lock
	install -D -m 0644 build/pam_bioauth.so $(DESTDIR)$(PAMMODDIR)/pam_bioauth.so
	install -D -m 0644 config/bioauth.json $(DESTDIR)$(CONFIG_DIR)/config.json
	install -D -m 0644 systemd/bioauthd.service $(DESTDIR)$(SYSTEMD_DIR)/bioauthd.service
	install -D -m 0644 pam.d/bioauth-test $(DESTDIR)$(PAM_DIR)/bioauth-test

clean:
	rm -rf build

.PHONY: all check check-tty check-gui check-e2e doctor install install-pkexec clean

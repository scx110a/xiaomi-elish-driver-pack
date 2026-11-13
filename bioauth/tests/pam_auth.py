#!/usr/bin/env python3
"""Run pam_authenticate for a private PAM config dir (no root needed).

Uses pam_start_confdir(3) from libpam 1.5.  The PAM service file in --confdir
may reference the locally built pam_bioauth.so by absolute path and pass
socket=/helper=/debug arguments, so the full PAM flow can be tested without
touching /etc/pam.d or the system PAM module.

Exit status: 0 = PAM_SUCCESS, otherwise 1 and a PAM_* code is printed.
"""
import argparse
import ctypes
import os
import sys

libpam = ctypes.CDLL("libpam.so.0", use_errno=True)

PAM_SUCCESS = 0
PAM_AUTH_ERR = 7
PAM_CONV_ERR = 19
PAM_IGNORE = 25
PAM_TTY = 3

PAM_NAMES = {0: "PAM_SUCCESS", 7: "PAM_AUTH_ERR", 25: "PAM_IGNORE",
             19: "PAM_CONV_ERR", 6: "PAM_AUTHINFO_UNAVAIL", 9: "PAM_AUTHTOK_ERR",
             28: "PAM_ABORT", 4: "PAM_SYSTEM_ERR"}


class PamMessage(ctypes.Structure):
    _fields_ = [("msg_style", ctypes.c_int), ("msg", ctypes.c_char_p)]


class PamResponse(ctypes.Structure):
    _fields_ = [("resp", ctypes.c_char_p), ("resp_retcode", ctypes.c_int)]


ConvFunc = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_int,
                            ctypes.POINTER(ctypes.POINTER(PamMessage)),
                            ctypes.POINTER(ctypes.POINTER(PamResponse)),
                            ctypes.c_void_p)


class PamConv(ctypes.Structure):
    _fields_ = [("conv", ConvFunc), ("appdata_ptr", ctypes.c_void_p)]


def _conv(n, msgs, resp, data):
    # The module reads keyboard input from PAM_TTY / the GUI helper itself, so
    # the conversation only receives fallback prompts.  Refuse them rather than
    # fabricating input.
    for i in range(n):
        m = msgs[i].contents
        sys.stderr.write("pam_conv: style=%d msg=%r (refused)\n"
                         % (m.msg_style, m.msg))
    return PAM_CONV_ERR


_CONV = ConvFunc(_conv)
_CONV_STRUCT = PamConv(_CONV, None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--confdir", required=True)
    ap.add_argument("--service", default="bioauth-dev")
    ap.add_argument("--user", required=True)
    ap.add_argument("--tty", default=None, help="set PAM_TTY (for TTY-mode tests)")
    args = ap.parse_args()

    os.setsid()  # no controlling terminal => /dev/tty unavailable

    libpam.pam_start_confdir.restype = ctypes.c_int
    libpam.pam_start_confdir.argtypes = [ctypes.c_char_p, ctypes.c_char_p,
                                         ctypes.POINTER(PamConv),
                                         ctypes.c_char_p,
                                         ctypes.POINTER(ctypes.c_void_p)]
    libpam.pam_set_item.restype = ctypes.c_int
    libpam.pam_set_item.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
    libpam.pam_authenticate.restype = ctypes.c_int
    libpam.pam_authenticate.argtypes = [ctypes.c_void_p, ctypes.c_int]
    libpam.pam_end.restype = ctypes.c_int
    libpam.pam_end.argtypes = [ctypes.c_void_p, ctypes.c_int]

    handle = ctypes.c_void_p()
    rc = libpam.pam_start_confdir(args.service.encode(), args.user.encode(),
                                  ctypes.byref(_CONV_STRUCT),
                                  args.confdir.encode(), ctypes.byref(handle))
    if rc != PAM_SUCCESS:
        print(f"pam_start_confdir failed rc={rc}")
        return 2
    if args.tty:
        rc = libpam.pam_set_item(handle, PAM_TTY,
                                 ctypes.cast(ctypes.c_char_p(args.tty.encode()),
                                             ctypes.c_void_p))
        if rc != PAM_SUCCESS:
            print(f"pam_set_item(PAM_TTY) failed rc={rc}")
            libpam.pam_end(handle, rc)
            return 2
    print(f"pam_authenticate starting (service={args.service} user={args.user} "
          f"tty={args.tty})", flush=True)
    rc = libpam.pam_authenticate(handle, 0)
    libpam.pam_end(handle, rc)
    print(f"pam_authenticate rc={rc} ({PAM_NAMES.get(rc, 'PAM_?')})", flush=True)
    # Propagate the PAM code (0..31) as the exit status so tests can assert on
    # PAM_SUCCESS / PAM_AUTH_ERR / PAM_IGNORE directly.
    return rc if 0 <= rc < 200 else 1


if __name__ == "__main__":
    sys.exit(main())

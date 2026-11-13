/*
 * pam_bioauth.so - generic biometric PAM client
 *
 * The module contains no device/vendor code. It talks to bioauthd over a
 * Unix socket; bioauthd owns the fingerprint backend, state and lockout.
 * If no terminal and no usable GUI is available it returns PAM_IGNORE so the
 * normal password stack remains the fallback.
 *
 * Two UI kinds are supported:
 *   UI_TTY - /dev/tty based flow (sudo in a terminal).  While the fingerprint
 *            scanner is armed the terminal is put in non-canonical mode so
 *            "P" switches to the password prompt and "Q" cancels immediately.
 *   UI_GUI - bioauth-ui (single GTK window) run as the target user, connected
 *            with a socketpair on fd 3, newline-delimited JSON in both
 *            directions.  The helper must emit {"event":"ready"} once the
 *            window is actually shown; if it does not become ready the module
 *            kills it and falls back (this is what makes "no popup" visible in
 *            the logs instead of silently authenticating invisibly).
 *
 * Request ids: every fingerprint request carries "session" (random per PAM
 * authentication) and "seq" (increments per attempt).  cancel carries the same
 * id, so a cancel can never hit the next attempt and an early cancel is not
 * lost in the cancel-vs-verify race.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <syslog.h>

#define PAM_SM_AUTH
#include <security/pam_modules.h>
#include <security/pam_ext.h>

#define DEFAULT_SOCKET "/run/bioauth.sock"
#define DEFAULT_HELPER "/usr/local/libexec/bioauth-ui"
#define BIOAUTH_PROTO 2
#define DEFAULT_UI_LOG "/tmp/bioauth-ui.log"
#define DEFAULT_PAM_LOG "/tmp/bioauth-pam.log"
#define BUF_SIZE 65536

/* internal return code from do_fingerprint() */
#define FP_UNAVAILABLE 2

/* internal switch codes */
#define AUTH_SWITCH_FP 3
#define AUTH_SWITCH_PW 4

/* ui_poll_switch() results */
#define SW_NONE 0
#define SW_PASSWORD 1
#define SW_FINGERPRINT 2
#define SW_CANCEL 3
#define SW_UI_DEAD 4

static const char *g_socket = DEFAULT_SOCKET;
static const char *g_helper = DEFAULT_HELPER;
static int g_debug = 0;
static const char *g_log_path = NULL;
static int g_ready_timeout = 5000;   /* ms; 0 disables the handshake */
static int g_prefer_gui = 0;         /* 0 = tty first, 1 = gui first */
static int g_password_tries = 3;
static int g_auth_timeout = 120;
static int g_pw_used = 0;
static unsigned g_pw_attempt = 0;
static char g_pending_line[2048];
static int g_has_pending_line = 0;
static long long g_deadline_ms = 0;
static char g_session[33];
static unsigned g_seq = 0;

static int tty_fd = -1;
static int tty_raw = 0;
static struct termios tty_saved;

enum ui_kind { UI_NONE = 0, UI_TTY, UI_GUI };
static enum ui_kind ui_kind = UI_NONE;

static uid_t gui_uid;
static gid_t gui_gid;
static char gui_name[64];
static char gui_home[PATH_MAX];
static char gui_runtime[64];
static int helper_fd = -1;
static pid_t helper_pid = -1;

static int json_string(const char *buf, const char *key, char *out, size_t outsz);

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int remaining_ms(void)
{
    long long left;
    if (g_auth_timeout <= 0)
        return 300000;
    left = g_deadline_ms - now_ms();
    if (left < 0)
        left = 0;
    if (left > 300000)
        left = 300000;
    return (int)left;
}

/* ------------------------------------------------------------------ */
/* logging (only when debug is enabled; /tmp/bioauth-pam.log by default) */

static const char *
pam_log_path(void)
{
    if (g_log_path)
        return g_log_path;
    /* Prefer the root-owned runtime dir when it exists (created by bioauthd's
     * StateDirectory); never default to a world-writable location when running
     * with the debug flag under sudo. */
    if (access("/run/bioauth", W_OK) == 0)
        return "/run/bioauth/pam.log";
    return DEFAULT_PAM_LOG;
}

static void
plog(const char *fmt, ...)
{
    int fd;
    FILE *fp;
    va_list ap;
    struct timespec ts;
    struct tm tm;

    if (!g_debug)
        return;
    fd = open(pam_log_path(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC,
              0600);
    if (fd < 0)
        return;
    (void)fchmod(fd, 0600);
    fp = fdopen(fd, "a");
    if (!fp) {
        close(fd);
        return;
    }
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    fprintf(fp, "%02d:%02d:%02d.%03ld [%d] ", tm.tm_hour, tm.tm_min, tm.tm_sec,
            ts.tv_nsec / 1000000, (int)getpid());
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

/* Log helper JSON without ever writing values (passwords travel as base64). */
static void
plog_helper_rx(const char *buf)
{
    char ev[64], op[64];

    ev[0] = op[0] = '\0';
    json_string(buf, "event", ev, sizeof(ev));
    if (!ev[0])
        json_string(buf, "op", op, sizeof(op));
    if (ev[0])
        plog("helper<- event=%s", ev);
    else if (op[0])
        plog("helper<- op=%s", op);
    else
        plog("helper<- (unparseable line, redacted)");
}

/* ------------------------------------------------------------------ */
/* small helpers                                                       */

static void
json_escape(const char *src, char *dst, size_t dstsz)
{
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j + 2 < dstsz; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (j + 3 >= dstsz)
                break;
            dst[j++] = '\\';
            dst[j++] = (char)c;
        } else if (c == '\n' || c == '\r' || c == '\t') {
            dst[j++] = ' ';
        } else {
            dst[j++] = (char)c;
        }
    }
    dst[j] = '\0';
}

static const char *
json_find(const char *buf, const char *key)
{
    char pat[64];
    const char *p;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(buf, pat);
    if (!p)
        return NULL;
    p += strlen(pat);
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != ':')
        return NULL;
    p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    return p;
}

static int
json_bool(const char *buf, const char *key, int *out)
{
    const char *p = json_find(buf, key);
    if (!p)
        return -1;
    if (strncmp(p, "true", 4) == 0) {
        *out = 1;
        return 0;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

static int
json_int(const char *buf, const char *key, int *out)
{
    const char *p = json_find(buf, key);
    long v;
    if (!p)
        return -1;
    errno = 0;
    v = strtol(p, NULL, 10);
    if (errno || v < INT_MIN || v > INT_MAX)
        return -1;
    *out = (int)v;
    return 0;
}

static int
json_string(const char *buf, const char *key, char *out, size_t outsz)
{
    const char *p = json_find(buf, key);
    const char *q;
    size_t n;
    if (!p || *p != '"')
        return -1;
    p++;
    q = strchr(p, '"');
    if (!q)
        return -1;
    n = (size_t)(q - p);
    if (n >= outsz)
        n = outsz - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 0;
}

static void
gen_session(void)
{
    unsigned char rnd[16];
    static const char hex[] = "0123456789abcdef";
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    size_t i;

    if (fd < 0 || read(fd, rnd, sizeof(rnd)) != (ssize_t)sizeof(rnd)) {
        uint64_t seed = (uint64_t)time(NULL) ^ ((uint64_t)getpid() << 17) ^
                        (uint64_t)(uintptr_t)&rnd;
        for (i = 0; i < sizeof(rnd); i++) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            rnd[i] = (unsigned char)(seed >> 33);
        }
    }
    if (fd >= 0)
        close(fd);
    for (i = 0; i < sizeof(rnd); i++) {
        g_session[i * 2] = hex[rnd[i] >> 4];
        g_session[i * 2 + 1] = hex[rnd[i] & 0xf];
    }
    g_session[32] = '\0';
}

/* ------------------------------------------------------------------ */
/* base64                                                              */

static char *
b64_encode(const unsigned char *in, size_t len)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t outlen = 4 * ((len + 2) / 3);
    char *out = malloc(outlen + 1);
    size_t i, j = 0;

    if (!out)
        return NULL;
    for (i = 0; i + 2 < len; i += 3) {
        unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) | in[i + 2];
        out[j++] = tbl[(v >> 18) & 63];
        out[j++] = tbl[(v >> 12) & 63];
        out[j++] = tbl[(v >> 6) & 63];
        out[j++] = tbl[v & 63];
    }
    if (i < len) {
        unsigned v = (unsigned)in[i] << 16;
        if (i + 1 < len)
            v |= (unsigned)in[i + 1] << 8;
        out[j++] = tbl[(v >> 18) & 63];
        out[j++] = tbl[(v >> 12) & 63];
        out[j++] = (i + 1 < len) ? tbl[(v >> 6) & 63] : '=';
        out[j++] = '=';
    }
    out[j] = '\0';
    return out;
}

static char *
b64_decode(const char *val)
{
    size_t n = val ? strlen(val) : 0, o = 0;
    char *out = malloc(n + 1);
    int quad[4], qn = 0;

    if (!out)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)val[i];
        int v;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '+') v = 62;
        else if (ch == '/') v = 63;
        else if (ch == '=') continue;
        else continue;
        quad[qn++] = v;
        if (qn == 4) {
            out[o++] = (char)((quad[0] << 2) | (quad[1] >> 4));
            out[o++] = (char)(((quad[1] & 15) << 4) | (quad[2] >> 2));
            out[o++] = (char)(((quad[2] & 3) << 6) | quad[3]);
            qn = 0;
        }
    }
    if (qn == 2)
        out[o++] = (char)((quad[0] << 2) | (quad[1] >> 4));
    else if (qn == 3) {
        out[o++] = (char)((quad[0] << 2) | (quad[1] >> 4));
        out[o++] = (char)(((quad[1] & 15) << 4) | (quad[2] >> 2));
    }
    out[o] = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/* terminal UI                                                         */

static void
tty_msg(const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    size_t len;

    if (tty_fd < 0)
        return;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    len = strlen(msg);
    if (write(tty_fd, msg, len) < 0) {
        /* best effort */
    }
}

static void
tty_raw_off(void)
{
    if (tty_raw && tty_fd >= 0)
        tcsetattr(tty_fd, TCSANOW, &tty_saved);
    tty_raw = 0;
}

static void
tty_raw_on(void)
{
    struct termios t;

    if (tty_fd < 0 || tty_raw)
        return;
    if (tcgetattr(tty_fd, &tty_saved) != 0)
        return;
    t = tty_saved;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(tty_fd, TCSANOW, &t) == 0)
        tty_raw = 1;
}

/* 0 = no key, 1 = switch to password, 2 = cancel */
static int
tty_poll_key(int timeout_ms)
{
    struct pollfd pfd;
    char c;

    if (tty_fd < 0)
        return 0;
    pfd.fd = tty_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, timeout_ms) <= 0)
        return 0;
    if (!(pfd.revents & POLLIN))
        return 0;
    if (read(tty_fd, &c, 1) != 1)
        return 0;
    if (c == 'p' || c == 'P')
        return 1;
    if (c == 'q' || c == 'Q' || c == 3)
        return 2;
    return 0;
}

static int
tty_open(pam_handle_t *pamh)
{
    const void *item = NULL;
    const char *path = "/dev/tty";

    if (pam_get_item(pamh, PAM_TTY, &item) == PAM_SUCCESS && item &&
        *(const char *)item)
        path = (const char *)item;
    tty_fd = open(path, O_RDWR | O_CLOEXEC);
    plog("tty_open path=%s fd=%d errno=%d", path, tty_fd, errno);
    if (tty_fd < 0)
        return -1;
    if (!isatty(tty_fd)) {
        /* PAM_TTY can be "/dev/null" or a non-terminal for pkexec/GDM; that is
         * not an interactive terminal, fall through to the GUI. */
        plog("tty_open %s is not a tty", path);
        close(tty_fd);
        tty_fd = -1;
        return -1;
    }
    return 0;
}

static void
tty_close(void)
{
    tty_raw_off();
    if (tty_fd >= 0) {
        close(tty_fd);
        tty_fd = -1;
    }
}

static char *
tty_read(int echo, const char *prompt)
{
    char *buf;
    size_t len = 0;
    struct termios oldt, newt;
    int have_term = 0;
    char c;

    if (tty_fd < 0)
        return NULL;
    tty_raw_off();
    tty_msg("%s", prompt);
    if (!echo && tcgetattr(tty_fd, &oldt) == 0) {
        newt = oldt;
        newt.c_lflag &= ~(ECHO);
        have_term = tcsetattr(tty_fd, TCSANOW, &newt) == 0;
    }
    buf = malloc(256);
    if (!buf)
        goto out;
    while (read(tty_fd, &c, 1) == 1) {
        if (c == '\n' || c == '\r')
            break;
        if (len < 255)
            buf[len++] = c;
    }
    buf[len] = '\0';
    tty_msg("\n");
out:
    if (have_term)
        tcsetattr(tty_fd, TCSANOW, &oldt);
    return buf;
}

/* ------------------------------------------------------------------ */
/* daemon socket                                                       */

static int
connect_daemon(void)
{
    struct sockaddr_un addr;
    int fd;
    struct timeval tv = { 10, 0 };

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(g_socket) >= sizeof(addr.sun_path))
        return -1;
    strncpy(addr.sun_path, g_socket, sizeof(addr.sun_path) - 1);

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        plog("daemon connect failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int
read_line(int fd, char *buf, size_t size)
{
    size_t off = 0;

    if (!size)
        return -1;
    while (off + 1 < size) {
        ssize_t n = read(fd, buf + off, 1);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        if (buf[off] == '\n')
            break;
        off++;
    }
    buf[off] = '\0';
    return (int)off;
}

static int
call_daemon(const char *request, char *resp, size_t respsz)
{
    int fd = connect_daemon();
    size_t len;
    int n;

    if (fd < 0)
        return -1;
    len = strlen(request);
    if (send(fd, request, len, MSG_NOSIGNAL) != (ssize_t)len) {
        close(fd);
        return -1;
    }
    n = read_line(fd, resp, respsz);
    close(fd);
    return n > 0 ? 0 : -1;
}

/* Read whatever the daemon sends on fd within timeout_ms; used after a cancel
 * so the helper's in-flight verification cannot outlive the PAM decision. */
static void
drain_fd(int fd, int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    char buf[1024];

    if (fd < 0 || poll(&pfd, 1, timeout_ms) <= 0)
        return;
    if (pfd.revents & POLLIN)
        (void)read_line(fd, buf, sizeof(buf));
}

/* ------------------------------------------------------------------ */
/* GUI helper                                                          */

static int
helper_send(const char *fmt, ...)
{
    char msg[4096];
    va_list ap;
    size_t len;
    ssize_t n;

    if (helper_fd < 0)
        return -1;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg) - 2, fmt, ap);
    va_end(ap);
    len = strlen(msg);
    msg[len++] = '\n';
    msg[len] = '\0';
    n = send(helper_fd, msg, len, MSG_NOSIGNAL);
    if (n != (ssize_t)len) {
        plog("helper send failed len=%zu errno=%d", len, errno);
        return -1;
    }
    return 0;
}

/* 0 = one line read, -1 = timeout, -2 = helper gone */
static int
helper_read_line(char *buf, size_t size, int timeout_ms)
{
    struct pollfd pfd;
    size_t off = 0;

    if (helper_fd < 0 || !size)
        return -2;
    buf[0] = '\0';
    for (;;) {
        pfd.fd = helper_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, timeout_ms) <= 0)
            return -1;
        if (pfd.revents & (POLLHUP | POLLERR))
            return -2;
        if (!(pfd.revents & POLLIN))
            continue;
        while (off + 1 < size) {
            ssize_t n = read(helper_fd, buf + off, 1);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                return -2;
            if (buf[off] == '\n')
                break;
            off++;
        }
        buf[off] = '\0';
        if (off > 0 && buf[off - 1] == '\r')
            buf[off - 1] = '\0';
        return 0;
    }
}

static int
helper_next_line(char *buf, size_t size, int timeout_ms)
{
    if (g_has_pending_line) {
        snprintf(buf, size, "%s", g_pending_line);
        g_has_pending_line = 0;
        return 0;
    }
    return helper_read_line(buf, size, timeout_ms);
}

static void
helper_pushback(const char *buf)
{
    if (!g_has_pending_line && strlen(buf) < sizeof(g_pending_line)) {
        snprintf(g_pending_line, sizeof(g_pending_line), "%s", buf);
        g_has_pending_line = 1;
    }
}

static int
helper_value(const char *buf, const char *key, char *out, size_t outsz)
{
    return json_string(buf, key, out, outsz);
}

static int
helper_event_is(const char *buf, const char *value)
{
    char ev[64];
    if (helper_value(buf, "event", ev, sizeof(ev)) != 0)
        return 0;
    return strcmp(ev, value) == 0;
}

static void
detect_display(char *wd, size_t wdsz, char *disp, size_t dispsz, char *xauth,
               size_t xauthsz)
{
    DIR *d;
    struct dirent *de;
    char *env;

    wd[0] = '\0';
    disp[0] = '\0';
    xauth[0] = '\0';
    env = getenv("WAYLAND_DISPLAY");
    if (env && *env)
        snprintf(wd, wdsz, "%s", env);
    env = getenv("DISPLAY");
    if (env && *env)
        snprintf(disp, dispsz, "%s", env);
    env = getenv("XAUTHORITY");
    if (env && *env)
        snprintf(xauth, xauthsz, "%s", env);

    d = opendir(gui_runtime);
    if (d) {
        while ((de = readdir(d)) != NULL) {
            if (strncmp(de->d_name, "wayland-", 8) == 0 && !wd[0] &&
                strlen(de->d_name) < wdsz)
                strcpy(wd, de->d_name);
            if (strncmp(de->d_name, ".mutter-Xwaylandauth", 20) == 0 && !xauth[0]) {
                snprintf(xauth, xauthsz, "%s/%s", gui_runtime, de->d_name);
            }
        }
        closedir(d);
    }
    if (!wd[0] && access("/tmp/.X11-unix/X0", F_OK) == 0 && !disp[0])
        snprintf(disp, dispsz, ":0");
    
}

static void
helper_close_fds(int keep)
{
    long max = sysconf(_SC_OPEN_MAX);
    if (max < 0 || max > 4096)
        max = 4096;
    for (int i = 0; i < (int)max; i++) {
        if (i != keep && i > 2)
            close(i);
    }
}

static int
helper_start(pam_handle_t *pamh, const char *user)
{
    struct passwd *pw = getpwnam(user);
    const void *svc_item = NULL;
    const char *service = "auth";
    int fds[2];
    pid_t pid;
    char wd[256], disp[64], xauth[PATH_MAX];

    if (!pw || !pw->pw_name || !pw->pw_dir)
        return -1;
    if (access(g_helper, X_OK) != 0) {
        plog("helper %s not executable: %s", g_helper, strerror(errno));
        return -1;
    }
    snprintf(gui_name, sizeof(gui_name), "%s", pw->pw_name);
    snprintf(gui_home, sizeof(gui_home), "%s", pw->pw_dir);
    snprintf(gui_runtime, sizeof(gui_runtime), "/run/user/%u", (unsigned)pw->pw_uid);
    gui_uid = pw->pw_uid;
    gui_gid = pw->pw_gid;
    if (access(gui_runtime, F_OK) != 0) {
        plog("runtime dir %s missing", gui_runtime);
        return -1;
    }
    if (pam_get_item(pamh, PAM_SERVICE, &svc_item) == PAM_SUCCESS && svc_item &&
        *(const char *)svc_item)
        service = (const char *)svc_item;

    detect_display(wd, sizeof(wd), disp, sizeof(disp), xauth, sizeof(xauth));
    if (!wd[0] && !disp[0]) {
        plog("no WAYLAND_DISPLAY/DISPLAY for user");
        return -1;
    }

    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) {
        plog("socketpair failed: %s", strerror(errno));
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        /* child: keep only the protocol fd on 3 */
        close(fds[0]);
        helper_close_fds(fds[1]);
        if (geteuid() == 0) {
            if (initgroups(gui_name, gui_gid) != 0 || setgid(gui_gid) != 0 ||
                setuid(gui_uid) != 0)
                _exit(127);
        }
        setenv("HOME", gui_home, 1);
        setenv("USER", gui_name, 1);
        setenv("LOGNAME", gui_name, 1);
        setenv("XDG_RUNTIME_DIR", gui_runtime, 1);
        if (wd[0])
            setenv("WAYLAND_DISPLAY", wd, 1);
        if (disp[0])
            setenv("DISPLAY", disp, 1);
        if (xauth[0])
            setenv("XAUTHORITY", xauth, 1);
        if (!getenv("XDG_SESSION_TYPE"))
            setenv("XDG_SESSION_TYPE", wd[0] ? "wayland" : "x11", 0);
        if (!getenv("XDG_CURRENT_DESKTOP"))
            setenv("XDG_CURRENT_DESKTOP", "GNOME", 0);
        {
            char dbus[96];
            snprintf(dbus, sizeof(dbus), "unix:path=%s/bus", gui_runtime);
            setenv("DBUS_SESSION_BUS_ADDRESS", dbus, 1);
        }
        /* The helper must never write to the inherited stdout: under
         * pkexec/polkit the PAM client (polkit-agent-helper-1) uses stdout as
         * the prompt protocol between PAM and the desktop agent, and a stray
         * GTK/Python message would corrupt it.  Always detach stdout; stderr
         * goes to the debug log when enabled (journal otherwise). */
        {
            int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                if (!g_debug)
                    dup2(devnull, STDERR_FILENO);
                if (devnull > 2)
                    close(devnull);
            }
            if (g_debug) {
                const char *uilog = getenv("BIOAUTH_UI_LOG");
                int lf = open(uilog && *uilog ? uilog : DEFAULT_UI_LOG,
                              O_WRONLY | O_CREAT | O_APPEND, 0600);
                if (lf >= 0) {
                    dup2(lf, STDERR_FILENO);
                    if (lf > 2)
                        close(lf);
                }
            }
        }
        if (fds[1] == 3) {
            fcntl(fds[1], F_SETFD, 0);
        } else if (dup2(fds[1], 3) < 0) {
            _exit(127);
        } else {
            close(fds[1]);
        }
        execl(g_helper, "bioauth-ui", NULL);
        _exit(127);
    }
    close(fds[1]);
    helper_fd = fds[0];
    helper_pid = pid;
    plog("helper started pid=%d fd=%d helper=%s", (int)pid, helper_fd, g_helper);
    helper_send("{\"op\":\"start\",\"service\":\"%s\"}", service);
    return 0;
}

static int
helper_wait_ready(int timeout_ms)
{
    char buf[1024];
    long long deadline = now_ms() + timeout_ms;

    while (now_ms() < deadline) {
        long long left = deadline - now_ms();
        int r;
        if (left <= 0)
            break;
        r = helper_read_line(buf, sizeof(buf), (int)(left < 200 ? left : 200));
        if (r == -1)
            continue;
        if (r == -2) {
            plog("helper went away before ready");
            return -1;
        }
        {
            int proto = 0;
            json_int(buf, "proto", &proto);
            plog_helper_rx(buf);
            if (helper_event_is(buf, "ready")) {
                plog("helper ready (proto=%d)", proto);
                if (proto < 2)
                    plog("WARNING: helper ready without proto>=2 (old helper?)");
                return 0;
            }
        }
        /* ignore anything else before ready */
    }
    plog("helper not ready within %d ms", timeout_ms);
    return -1;
}

static void
ui_close(void)
{
    int status;
    int waited = 0;

    tty_raw_off();
    if (helper_fd >= 0) {
        close(helper_fd);
        helper_fd = -1;
    }
    if (helper_pid > 0) {
        while (waited < 800) {
            pid_t r = waitpid(helper_pid, &status, WNOHANG);
            if (r == helper_pid || (r < 0 && errno != EINTR)) {
                helper_pid = -1;
                break;
            }
            usleep(20000);
            waited += 20;
        }
        if (helper_pid > 0) {
            plog("helper pid=%d still alive; SIGTERM", (int)helper_pid);
            kill(helper_pid, SIGTERM);
            waited = 0;
            while (waited < 400) {
                pid_t r = waitpid(helper_pid, &status, WNOHANG);
                if (r == helper_pid || (r < 0 && errno != EINTR)) {
                    helper_pid = -1;
                    break;
                }
                usleep(20000);
                waited += 20;
            }
        }
        if (helper_pid > 0) {
            kill(helper_pid, SIGKILL);
            waitpid(helper_pid, &status, 0);
            helper_pid = -1;
        }
    }
    plog("ui closed");
}

/* ------------------------------------------------------------------ */
/* UI state updates                                                    */

static void
ui_fingerprint_wait(const char *text)
{
    char esc[1024];

    if (ui_kind == UI_TTY) {
        tty_msg("%s\n", text);
        tty_msg("  按 P 切换密码，按 Q 取消\n");
        return;
    }
    if (ui_kind != UI_GUI)
        return;
    json_escape(text, esc, sizeof(esc));
    helper_send("{\"op\":\"fingerprint_wait\",\"text\":\"%s\"}", esc);
}

static void
ui_fingerprint_result(int ok, const char *text)
{
    char esc[1024];

    if (ui_kind == UI_TTY) {
        if (text)
            tty_msg("%s\n", text);
        return;
    }
    if (ui_kind != UI_GUI)
        return;
    json_escape(text, esc, sizeof(esc));
    plog("ui: fingerprint_result ok=%d text=%s", ok, text);
    helper_send("{\"op\":\"fingerprint_result\",\"ok\":%s,\"text\":\"%s\"}",
                ok ? "true" : "false", esc);
}

static void
ui_password_result(int ok, const char *text)
{
    char esc[1024];

    if (ui_kind == UI_TTY) {
        if (text)
            tty_msg("%s\n", text);
        return;
    }
    if (ui_kind != UI_GUI)
        return;
    json_escape(text, esc, sizeof(esc));
    plog("ui: password_result ok=%d text=%s", ok, text);
    helper_send("{\"op\":\"password_result\",\"ok\":%s,\"text\":\"%s\"}",
                ok ? "true" : "false", esc);
}

static void
ui_password_mode(const char *text, int allow_fingerprint)
{
    char esc[1024];

    g_pw_attempt++;
    if (ui_kind == UI_TTY) {
        if (text)
            tty_msg("%s\n", text);
        return;
    }
    if (ui_kind != UI_GUI)
        return;
    json_escape(text, esc, sizeof(esc));
    helper_send("{\"op\":\"password\",\"text\":\"%s\",\"allow_fingerprint\":%s,"
                "\"attempt\":%u}",
                esc, allow_fingerprint ? "true" : "false", g_pw_attempt);
}

static void
ui_success(const char *text)
{
    char esc[1024];

    if (ui_kind == UI_TTY) {
        if (text)
            tty_msg("%s\n", text);
        return;
    }
    if (ui_kind != UI_GUI)
        return;
    json_escape(text, esc, sizeof(esc));
    helper_send("{\"op\":\"success\",\"text\":\"%s\"}", esc);
}

/* 0 none, 1 switch->password, 2 switch->fingerprint, 3 cancel, 4 ui dead.
 *
 * Unknown events (e.g. a password submit that raced with the 1.2 s result
 * dwell) are pushed back instead of being thrown away, so they are delivered
 * to the next prompt rather than silently lost.  While an event is pending we
 * still wait out the requested dwell so the red status text stays readable. */
static int
ui_poll_switch(int timeout_ms)
{
    char buf[2048];
    long long deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 0);

    if (ui_kind == UI_TTY) {
        int k = tty_poll_key(timeout_ms);
        if (k == 1)
            return SW_PASSWORD;
        if (k == 2)
            return SW_CANCEL;
        return SW_NONE;
    }
    if (ui_kind != UI_GUI)
        return SW_NONE;
    for (;;) {
        long long left = deadline - now_ms();
        int r;
        if (g_has_pending_line) {
            /* An unusable line is pending, but never let it starve the escape
             * path: poll for a switch/cancel event with priority. */
            r = helper_read_line(buf, sizeof(buf), 0);
            if (r == 0) {
                plog_helper_rx(buf);
                if (helper_event_is(buf, "switch_password"))
                    return SW_PASSWORD;
                if (helper_event_is(buf, "switch_fingerprint"))
                    return SW_FINGERPRINT;
                if (helper_event_is(buf, "cancel"))
                    return SW_CANCEL;
                /* newest unusable line replaces the older one (bounded queue) */
                snprintf(g_pending_line, sizeof(g_pending_line), "%s", buf);
            } else if (r == -2) {
                plog("helper dead in ui_poll_switch");
                return SW_UI_DEAD;
            }
            if (left <= 0)
                return SW_NONE;
            usleep((useconds_t)((left < 50 ? left : 50) * 1000));
            continue;
        }
        r = helper_read_line(buf, sizeof(buf), left > 0 ? (int)left : 0);
        if (r == -1)
            return SW_NONE;
        if (r == -2) {
            plog("helper dead in ui_poll_switch");
            return SW_UI_DEAD;
        }
        plog_helper_rx(buf);
        if (helper_event_is(buf, "switch_password"))
            return SW_PASSWORD;
        if (helper_event_is(buf, "switch_fingerprint"))
            return SW_FINGERPRINT;
        if (helper_event_is(buf, "cancel"))
            return SW_CANCEL;
        helper_pushback(buf);
    }
}

/* ------------------------------------------------------------------ */
/* daemon requests                                                     */

static int
daemon_password(const char *user, const char *password, char *resp, size_t respsz)
{
    char esc[512];
    char *enc;
    char req[2048];
    int rc;

    json_escape(user, esc, sizeof(esc));
    enc = b64_encode((const unsigned char *)password, strlen(password));
    if (!enc)
        return -1;
    snprintf(req, sizeof(req),
             "{\"op\":\"password\",\"user\":\"%s\",\"password_b64\":\"%s\"}\n", esc,
             enc);
    memset(enc, 0, strlen(enc));
    free(enc);
    rc = call_daemon(req, resp, respsz);
    memset(req, 0, sizeof(req));
    return rc;
}

static void
daemon_cancel(const char *user, const char *req_id)
{
    char esc[512], resc[64], req[1024];
    int fd;

    json_escape(user, esc, sizeof(esc));
    json_escape(req_id, resc, sizeof(resc));
    snprintf(req, sizeof(req), "{\"op\":\"cancel\",\"user\":\"%s\",\"req\":\"%s\"}\n",
             esc, resc);
    fd = connect_daemon();
    if (fd >= 0) {
        send(fd, req, strlen(req), MSG_NOSIGNAL);
        close(fd);
        plog("daemon cancel req=%s", req_id);
    }
}

/* ------------------------------------------------------------------ */
/* password flow                                                       */

/* 0 = password received, 1 = cancel/timeout, 2 = switch to fingerprint, 3 = ui dead */
static int
ui_password_wait(const char *text, int allow_fingerprint, char **out, int timeout_ms)
{
    long long deadline;

    *out = NULL;
    ui_password_mode(text, allow_fingerprint);
    if (ui_kind == UI_TTY) {
        tty_raw_off();
        if (timeout_ms > 0 && timeout_ms < 300000) {
            struct pollfd pfd = { .fd = tty_fd, .events = POLLIN };
            if (poll(&pfd, 1, timeout_ms) <= 0)
                return 1;
        }
        *out = tty_read(0, "密码: ");
        return *out ? 0 : 1;
    }
    if (ui_kind != UI_GUI)
        return 1;
    deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 300000);
    for (;;) {
        char buf[2048], val[1024];
        long long left = deadline - now_ms();
        int r;
        if (left <= 0)
            return 1;
        r = helper_next_line(buf, sizeof(buf), (int)(left < 250 ? left : 250));
        if (r == -1)
            continue;
        if (r == -2) {
            plog("helper died while waiting for password");
            return 3;
        }
        plog_helper_rx(buf);
        if (helper_event_is(buf, "password")) {
            int attempt = -1;
            json_int(buf, "attempt", &attempt);
            if (attempt >= 0 && attempt != (int)g_pw_attempt) {
                plog("discarding stale password submit (attempt %d != %u)", attempt,
                     g_pw_attempt);
                continue;
            }
            if (helper_value(buf, "value", val, sizeof(val)) != 0)
                return 1;
            *out = b64_decode(val);
            return *out ? 0 : 1;
        }
        if (helper_event_is(buf, "switch_fingerprint"))
            return 2;
        if (helper_event_is(buf, "cancel"))
            return 1;
    }
}

static int
do_password(pam_handle_t *pamh, const char *user, int allow_fingerprint)
{
    (void)pamh;
    for (;;) {
        int left = remaining_ms();
        char *pw = NULL;
        char resp[BUF_SIZE];
        int rc, ok = 0;
        int ev;

        if (g_password_tries > 0 && g_pw_used >= g_password_tries) {
            ui_password_result(0, "密码尝试次数已用完");
            return PAM_AUTH_ERR;
        }
        if (g_auth_timeout > 0 && left <= 0) {
            ui_password_result(0, "验证超时");
            return PAM_AUTH_ERR;
        }
        ev = ui_password_wait("请输入密码", allow_fingerprint, &pw, left);
        if (ev == 2)
            return AUTH_SWITCH_FP;
        if (ev == 3)
            return PAM_IGNORE;
        if (ev != 0 || !pw) {
            if (g_auth_timeout > 0 && remaining_ms() <= 0) {
                ui_password_result(0, "验证超时");
                return PAM_AUTH_ERR;
            }
            /* no input / user aborted: let the normal password stack try */
            return PAM_IGNORE;
        }
        g_pw_used++;
        rc = daemon_password(user, pw, resp, sizeof(resp));
        memset(pw, 0, strlen(pw));
        free(pw);
        if (rc != 0) {
            ui_password_result(0, "无法联系认证服务，请稍后重试。");
            return PAM_IGNORE;
        }
        if (json_bool(resp, "ok", &ok) == 0 && ok) {
            ui_success("密码验证成功；指纹验证已恢复。");
            usleep(1000 * 1000);
            return PAM_SUCCESS;
        }
        if (g_password_tries > 0 && g_pw_used >= g_password_tries) {
            ui_password_result(0, "密码尝试次数已用完");
            return PAM_AUTH_ERR;
        }
        /* Keep the red "密码错误" visible long enough to be read (R8/R9). */
        ui_password_result(0, "密码错误");
        if (ui_kind == UI_TTY) {
            usleep(1200 * 1000);
        } else {
            int sw = ui_poll_switch(1200);
            if (sw == SW_FINGERPRINT)
                return AUTH_SWITCH_FP;
            if (sw == SW_CANCEL)
                return PAM_IGNORE;
        }
    }
}

/* ------------------------------------------------------------------ */
/* fingerprint flow                                                    */

/* Send one fingerprint request to the daemon.  Returns the daemon response in
 * resp and PAM_SUCCESS / PAM_IGNORE / PAM_AUTH_ERR / AUTH_SWITCH_PW /
 * FP_UNAVAILABLE.  On a match it returns PAM_SUCCESS after the success dwell. */
static int
do_fingerprint(pam_handle_t *pamh, const char *user, int *locked_out, int *fail_out,
               int *max_out)
{
    char esc[512], resc[64];
    char req[1024];
    char resp[BUF_SIZE];
    char req_id[96];
    int fd, ok = 0, locked = 0, fail = 0, max = 0, unavailable = 0, canceled = 0;
    int timed_out = 0;
    unsigned seq = ++g_seq;

    (void)pamh;
    snprintf(req_id, sizeof(req_id), "%s:%u", g_session, seq);
    ui_fingerprint_wait("请把手指放在指纹传感器上…");
    if (ui_kind == UI_TTY)
        tty_raw_on();
    json_escape(user, esc, sizeof(esc));
    json_escape(req_id, resc, sizeof(resc));
    snprintf(req, sizeof(req),
             "{\"op\":\"fingerprint\",\"user\":\"%s\",\"session\":\"%s\",\"seq\":%u,"
             "\"req\":\"%s\"}\n",
             esc, g_session, seq, resc);
    fd = connect_daemon();
    if (fd < 0)
        return PAM_IGNORE;
    if (send(fd, req, strlen(req), MSG_NOSIGNAL) != (ssize_t)strlen(req)) {
        close(fd);
        return PAM_IGNORE;
    }
    plog("fingerprint req=%s", req_id);

    for (;;) {
        int left = remaining_ms();
        struct pollfd pfds[2];
        int nfds = 1;
        int pr;

        if (g_auth_timeout > 0 && left <= 0) {
            daemon_cancel(user, req_id);
            drain_fd(fd, 300);
            close(fd);
            tty_raw_off();
            ui_fingerprint_result(0, "验证超时");
            return PAM_AUTH_ERR;
        }
        pfds[0].fd = fd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        if (ui_kind == UI_GUI && helper_fd >= 0) {
            pfds[1].fd = helper_fd;
            pfds[1].events = POLLIN;
            pfds[1].revents = 0;
            nfds = 2;
        } else if (ui_kind == UI_TTY && tty_fd >= 0) {
            pfds[1].fd = tty_fd;
            pfds[1].events = POLLIN;
            pfds[1].revents = 0;
            nfds = 2;
        }
        pr = poll(pfds, nfds, left < 500 ? (left > 0 ? left : 1) : 500);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            tty_raw_off();
            return PAM_IGNORE;
        }
        if (nfds == 2 && (pfds[1].revents & (POLLHUP | POLLERR))) {
            plog("ui fd gone during fingerprint");
            close(fd);
            tty_raw_off();
            return PAM_IGNORE;
        }
        if (nfds == 2 && (pfds[1].revents & POLLIN)) {
            int sw = ui_poll_switch(0);
            if (sw == SW_PASSWORD) {
                /* If the daemon already answered "match", prefer success. */
                struct pollfd d = { .fd = fd, .events = POLLIN };
                if (poll(&d, 1, 0) > 0 && (d.revents & POLLIN) &&
                    read_line(fd, resp, sizeof(resp)) > 0) {
                    json_bool(resp, "ok", &ok);
                    if (ok) {
                        close(fd);
                        tty_raw_off();
                        ui_success("指纹验证成功。");
                        usleep(1000 * 1000);
                        return PAM_SUCCESS;
                    }
                    close(fd);
                    fd = -1;
                    goto use_response;
                }
                daemon_cancel(user, req_id);
                drain_fd(fd, 300);
                close(fd);
                tty_raw_off();
                return AUTH_SWITCH_PW;
            }
            if (sw == SW_CANCEL) {
                /* User aborted the biometric prompt: fall back to the normal
                 * password stack instead of failing the whole PAM transaction
                 * (this is the sudo rescue path). */
                daemon_cancel(user, req_id);
                drain_fd(fd, 300);
                close(fd);
                tty_raw_off();
                return PAM_IGNORE;
            }
            if (sw == SW_UI_DEAD) {
                close(fd);
                tty_raw_off();
                return PAM_IGNORE;
            }
        }
        if (pfds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            if (!(pfds[0].revents & POLLIN)) {
                plog("daemon fd hung up during fingerprint");
                close(fd);
                tty_raw_off();
                return PAM_IGNORE;
            }
        }
        if (pfds[0].revents & POLLIN) {
            if (read_line(fd, resp, sizeof(resp)) > 0)
                break;
            close(fd);
            tty_raw_off();
            return PAM_IGNORE;
        }
    }
    close(fd);

use_response:
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
    json_bool(resp, "ok", &ok);
    json_bool(resp, "locked", &locked);
    json_bool(resp, "unavailable", &unavailable);
    json_int(resp, "fail_count", &fail);
    json_int(resp, "max_failures", &max);
    json_bool(resp, "canceled", &canceled);
    json_bool(resp, "timeout", &timed_out);
    plog("fingerprint resp ok=%d locked=%d unavailable=%d timeout=%d fail=%d max=%d "
         "canceled=%d", ok, locked, unavailable, timed_out, fail, max, canceled);
    if (canceled) {
        tty_raw_off();
        ui_fingerprint_result(0, "指纹扫描已被取消。");
        return AUTH_SWITCH_PW;
    }
    if (timed_out) {
        /* No finger placed during fingerprint_timeout: this is NOT a mismatch
         * and must not advance the lockout counter (R7).  Auto-retry until the
         * overall auth_timeout decides. */
        tty_raw_off();
        ui_fingerprint_result(0, "未检测到手指，请重试。");
        return PAM_AUTH_ERR;
    }
    if (unavailable) {
        tty_raw_off();
        ui_fingerprint_result(0, "指纹不可用（设备/服务异常）。");
        return FP_UNAVAILABLE;
    }
    *locked_out = locked;
    *fail_out = fail;
    *max_out = max;
    if (ok) {
        tty_raw_off();
        ui_success("指纹验证成功。");
        usleep(1000 * 1000);
        return PAM_SUCCESS;
    }
    {
        char text[160];
        if (locked)
            snprintf(text, sizeof(text), "指纹连续失败 %d 次，已暂时停用；请使用密码。", fail);
        else if (max > 0)
            snprintf(text, sizeof(text), "指纹不匹配（%d/%d）。", fail, max);
        else
            snprintf(text, sizeof(text), "指纹不匹配。");
        ui_fingerprint_result(0, text);
    }
    return PAM_AUTH_ERR;
}

/* ------------------------------------------------------------------ */
/* PAM entry point                                                     */

static void
parse_args(int argc, const char **argv)
{
    for (int i = 0; i < argc; i++) {
        if (strncmp(argv[i], "socket=", 7) == 0) {
            g_socket = argv[i] + 7;
        } else if (strncmp(argv[i], "helper=", 7) == 0) {
            g_helper = argv[i] + 7;
        } else if (strncmp(argv[i], "ready_timeout=", 14) == 0) {
            g_ready_timeout = atoi(argv[i] + 14);
        } else if (strcmp(argv[i], "prefer=gui") == 0) {
            g_prefer_gui = 1;
        } else if (strcmp(argv[i], "prefer=tty") == 0) {
            g_prefer_gui = 0;
        } else if (strcmp(argv[i], "debug") == 0) {
            g_debug = 1;
        }
    }
}

static void
debug_init(void)
{
    const char *dbg = getenv("BIOAUTH_PAM_DEBUG");
    if (!g_debug && dbg && *dbg && strcmp(dbg, "0") != 0)
        g_debug = 1;
    g_log_path = getenv("BIOAUTH_PAM_LOG");
}

PAM_EXTERN int
pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
    const char *user = NULL;
    const void *tty_item = NULL;
    char resp[BUF_SIZE];
    int rc = PAM_IGNORE;
    int locked = 0;

    (void)flags;
    parse_args(argc, argv);
    debug_init();
    g_pw_used = 0;
    g_pw_attempt = 0;
    g_seq = 0;
    g_has_pending_line = 0;
    gen_session();

    if (pam_get_user(pamh, &user, "用户名: ") != PAM_SUCCESS || !user)
        return PAM_AUTH_ERR;
    pam_syslog(pamh, LOG_DEBUG, "bioauth: pam_bioauth proto=%d", BIOAUTH_PROTO);
    if (pam_get_item(pamh, PAM_TTY, &tty_item) == PAM_SUCCESS && tty_item) {
        plog("enter user=%s euid=%d pam_tty=%s prefer_gui=%d sess=%.8s", user,
             (int)geteuid(), (const char *)tty_item, g_prefer_gui, g_session);
    } else {
        plog("enter user=%s euid=%d pam_tty=(none) prefer_gui=%d sess=%.8s", user,
             (int)geteuid(), g_prefer_gui, g_session);
    }

    if (g_prefer_gui) {
        if (helper_start(pamh, user) == 0)
            ui_kind = UI_GUI;
        else if (tty_open(pamh) == 0)
            ui_kind = UI_TTY;
        else
            ui_kind = UI_NONE;
    } else {
        if (tty_open(pamh) == 0)
            ui_kind = UI_TTY;
        else if (helper_start(pamh, user) == 0)
            ui_kind = UI_GUI;
        else
            ui_kind = UI_NONE;
    }
    plog("ui_kind=%d", (int)ui_kind);
    if (ui_kind == UI_NONE)
        return PAM_IGNORE;

    if (ui_kind == UI_GUI && g_ready_timeout > 0) {
        if (helper_wait_ready(g_ready_timeout) != 0) {
            pam_syslog(pamh, LOG_ERR,
                       "bioauth: authentication UI did not become ready; falling back");
            ui_close();
            if (tty_open(pamh) == 0) {
                ui_kind = UI_TTY;
            } else {
                ui_kind = UI_NONE;
                return PAM_IGNORE;
            }
        }
    }

    {
        char esc[512], req[1024];
        json_escape(user, esc, sizeof(esc));
        snprintf(req, sizeof(req), "{\"op\":\"status\",\"user\":\"%s\"}\n", esc);
        if (call_daemon(req, resp, sizeof(resp)) != 0) {
            plog("daemon status failed");
            ui_close();
            tty_close();
            return PAM_IGNORE;
        }
    }
    json_bool(resp, "locked", &locked);
    json_int(resp, "password_tries", &g_password_tries);
    json_int(resp, "auth_timeout", &g_auth_timeout);
    if (g_password_tries < 0)
        g_password_tries = 0;
    if (g_auth_timeout < 0)
        g_auth_timeout = 0;
    g_deadline_ms =
        g_auth_timeout > 0 ? now_ms() + (long long)g_auth_timeout * 1000 : 0;
    plog("status locked=%d password_tries=%d auth_timeout=%d", locked, g_password_tries,
         g_auth_timeout);

    if (locked) {
        plog("locked at start; password only");
        ui_password_mode("指纹已暂时停用（连续失败），请使用密码。密码验证成功后将恢复指纹。",
                         0);
        rc = do_password(pamh, user, 0);
        if (rc == AUTH_SWITCH_FP)
            rc = PAM_AUTH_ERR;
        goto out;
    }

    for (;;) {
        int lk = 0, fail = 0, max = 0;
        int r = do_fingerprint(pamh, user, &lk, &fail, &max);

        if (r == PAM_SUCCESS) {
            rc = PAM_SUCCESS;
            goto out;
        }
        if (r == PAM_IGNORE) {
            rc = PAM_IGNORE;
            goto out;
        }
        if (r == AUTH_SWITCH_PW) {
            ui_password_mode("已切换到密码验证。", 1);
            {
                int pr = do_password(pamh, user, 1);
                if (pr == AUTH_SWITCH_FP)
                    continue;
                rc = pr;
                goto out;
            }
        }
        if (r == FP_UNAVAILABLE) {
            ui_password_mode("指纹不可用（设备/服务异常）；可重试指纹或使用密码。", 1);
            {
                int pr = do_password(pamh, user, 1);
                if (pr == AUTH_SWITCH_FP)
                    continue;
                rc = pr;
                goto out;
            }
        }
        if (lk) {
            ui_password_mode(
                "指纹连续失败，已暂时停用；请使用密码。密码成功后将恢复。", 0);
            {
                int pr = do_password(pamh, user, 0);
                rc = (pr == AUTH_SWITCH_FP) ? PAM_AUTH_ERR : pr;
                goto out;
            }
        }
        /* Mismatch, not locked: keep the red "n/N" result visible for a moment
         * and give the user a chance to switch before the next attempt. */
        {
            int wait_ms = 1200;
            int sw;
            if (g_auth_timeout > 0) {
                int left = remaining_ms();
                if (left <= 0) {
                    ui_fingerprint_result(0, "验证超时");
                    rc = PAM_AUTH_ERR;
                    goto out;
                }
                if (left < wait_ms)
                    wait_ms = left;
            }
            sw = ui_poll_switch(wait_ms);
            if (sw == SW_PASSWORD) {
                ui_password_mode("已切换到密码验证。", 1);
                {
                    int pr = do_password(pamh, user, 1);
                    if (pr == AUTH_SWITCH_FP)
                        continue;
                    rc = pr;
                    goto out;
                }
            }
            if (sw == SW_CANCEL) {
                rc = PAM_IGNORE;
                goto out;
            }
            if (sw == SW_UI_DEAD) {
                rc = PAM_IGNORE;
                goto out;
            }
        }
        /* default: automatically start the next fingerprint attempt */
    }

out:
    ui_close();
    tty_close();
    plog("exit rc=%d", rc);
    return rc;
}

PAM_EXTERN int
pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
    (void)pamh;
    (void)flags;
    (void)argc;
    (void)argv;
    return PAM_SUCCESS;
}

PAM_EXTERN int
pam_sm_chauthtok(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
    (void)pamh;
    (void)flags;
    (void)argc;
    (void)argv;
    return PAM_IGNORE;
}

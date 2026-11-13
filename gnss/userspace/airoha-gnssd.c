// SPDX-License-Identifier: GPL-2.0
/*
 * airoha-gnssd - userspace host driver for the Airoha AG3335 GNSS chip
 *                on Xiaomi Pad 5 Pro (elish, SM8250).
 *
 * The AG3335 has no mainline Linux driver.  On Android the vendor daemon
 * `airoha.hardware.anld` powers the chip through /dev/airoha_gps, then
 * talks the Airoha "PAIR" ASCII protocol over /dev/ttyHS1 at 921600 baud.
 *
 * This daemon reproduces the minimal part needed for positioning:
 *   1. power the chip on  (write "OPEN" + "DF" to /dev/airoha_gps)
 *   2. open /dev/ttyHS1 at 921600 8N1 raw
 *   3. immediately send $PAIR002 (open DSP) - the firmware only listens
 *      for ~1 s after power-on (the observed window is < 1.0 s), then it
 *      stops answering until reset
 *   4. wait for NMEA, then republish the NMEA stream on a pty so gpsd
 *      (or any other NMEA consumer) can read it
 *   5. on shutdown / on hang, power-cycle the chip and re-initialise
 *
 * Why a userspace daemon instead of a kernel driver: the whole boot
 * protocol is a userspace UART conversation (same architecture as
 * Android's anld), the kernel has no GNSS input subsystem, and keeping
 * it out of kernel space lets us handle the 1-second init window with
 * normal timers and retries.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define VERSION "0.3"

#define DEFAULT_TTY   "/dev/ttyHS1"
#define DEFAULT_CTL   "/dev/airoha_gps"
#define DEFAULT_LINK  "/dev/ttyGNSS0"
#define DEFAULT_BAUD  B921600

/* Chip power state files written to /dev/airoha_gps (see airoha_gps_driver.c) */
#define CTL_POWER_ON   "OPEN"   /* GPS LDO on        */
#define CTL_POWER_OFF  "CLOSE"  /* GPS LDO off       */
#define CTL_PINS_ACT   "DF"     /* active pinctrl    */
#define CTL_PINS_IDLE  "DI"     /* suspend pinctrl   */

/* PAIR protocol: $PAIR002 = open DSP / start navigation.  The reply is
 * $PAIR001,002,0*39 (PAIR001 = ACK, 002 = command, 0 = success). */
#define PAIR_OPEN_DSP        "$PAIR002*38\r\n"
/* Runtime bring-up after DSP open.  $PAIR002 alone only opens the DSP;
 * the chip then needs constellation/NMEA configuration and a hot start
 * before it starts producing fixes.  All are runtime-only (no flash write)
 * and were verified ACKed on this elish unit. */
#define PAIR_QUERY_VERSION   "$PAIR021*39\r\n"
#define PAIR_GNSS_ALL        "$PAIR066,1,1,1,1,0,0*3A\r\n"
#define PAIR_NMEA_GGA_ON     "$PAIR062,0,1*3F\r\n"
#define PAIR_NMEA_GLL_ON     "$PAIR062,1,1*3E\r\n"
#define PAIR_NMEA_GSA_ON     "$PAIR062,2,1*3D\r\n"
#define PAIR_NMEA_GSV_ON     "$PAIR062,3,1*3C\r\n"
#define PAIR_NMEA_RMC_ON     "$PAIR062,4,1*3B\r\n"
#define PAIR_NMEA_VTG_ON     "$PAIR062,5,1*3A\r\n"
#define PAIR_NMEA_ZDA_ON     "$PAIR062,6,1*39\r\n"
#define PAIR_DEBUG_OFF       "$PAIR086,0*28\r\n"
#define PAIR_HOT_START       "$PAIR004*3E\r\n"

/* Init window: the firmware only answers commands sent < ~1 s after power-on. */
#define INIT_WINDOW_MS        900
#define PROBE_INTERVAL_MS      60
#define BOOT_LOG_MS           250
#define FIRST_NMEA_TIMEOUT_MS 4000
#define HEALTH_TIMEOUT_MS    15000
#define MAX_INIT_RETRIES         4

#define RXBUF_SZ (1 << 16)
#define TXBUF_SZ (1 << 16)

struct daemon {
	const char *tty_path;
	const char *ctl_path;
	const char *link_path;
	const char *log_path;
	const char *nmea_log_path;
	speed_t baud;
	bool verbose;
	int ctl_fd;
	int tty_fd;
	int pty_master;
	char pty_slave[128];
	FILE *log_fp;
	FILE *nmea_fp;
	/* rx assembly */
	char rx[RXBUF_SZ];
	size_t rx_len;
	/* tx (pty) assembly */
	char tx[TXBUF_SZ];
	size_t tx_len;
	unsigned long nmea_total;
	unsigned long nmea_dropped;
	unsigned long debug_lines;
	time_t last_nmea;
	time_t started;
	volatile sig_atomic_t stop;
	bool saw_ack;
	bool test_mode;
};

static struct daemon g;

/* ------------------------------------------------------------------ */
/* logging                                                             */

static void logmsg(const char *fmt, ...)
{
	va_list ap;
	char ts[32];
	time_t now = time(NULL);
	struct tm tm;

	localtime_r(&now, &tm);
	strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

	va_start(ap, fmt);
	fprintf(stderr, "[%s] ", ts);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);

	if (g.log_fp) {
		va_start(ap, fmt);
		fprintf(g.log_fp, "[%s] ", ts);
		vfprintf(g.log_fp, fmt, ap);
		fputc('\n', g.log_fp);
		va_end(ap);
		fflush(g.log_fp);
	}
}

/* ------------------------------------------------------------------ */
/* sd_notify without linking libsystemd                                */

static void sd_notify(const char *msg)
{
	const char *sock = getenv("NOTIFY_SOCKET");
	struct sockaddr_un sa;
	socklen_t len;
	int fd;

	if (!sock || !*sock)
		return;
	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	if (sock[0] == '@') {           /* abstract namespace */
		sa.sun_path[0] = '\0';
		strncpy(sa.sun_path + 1, sock + 1, sizeof(sa.sun_path) - 2);
		len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 +
				  strlen(sock + 1));
	} else {
		strncpy(sa.sun_path, sock, sizeof(sa.sun_path) - 1);
		len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
				  strlen(sa.sun_path) + 1);
	}
	sendto(fd, msg, strlen(msg), MSG_NOSIGNAL, (struct sockaddr *)&sa, len);
	close(fd);
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */

static int ms_since(const struct timespec *t0)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int)((now.tv_sec - t0->tv_sec) * 1000 +
		     (now.tv_nsec - t0->tv_nsec) / 1000000);
}

static void sleep_ms(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
}

/* Write a command to /dev/airoha_gps.  The driver matches by substring. */
static int ctl_write(const char *cmd)
{
	ssize_t n;

	if (g.ctl_fd < 0)
		return -1;
	n = write(g.ctl_fd, cmd, strlen(cmd));
	if (n < 0) {
		logmsg("control write '%s' failed: %s", cmd, strerror(errno));
		return -1;
	}
	return 0;
}

static void chip_power_off(void)
{
	if (g.ctl_fd < 0)
		return;
	ctl_write(CTL_PINS_IDLE);
	ctl_write(CTL_POWER_OFF);
}

static void chip_power_on(void)
{
	ctl_write(CTL_POWER_ON);
	ctl_write(CTL_PINS_ACT);
}

static int open_ctl(const char *path)
{
	int fd = open(path, O_RDWR | O_CLOEXEC);

	if (fd < 0) {
		logmsg("cannot open %s: %s", path, strerror(errno));
		logmsg("is the airoha_gps module loaded?  (modprobe airoha_gps)");
		return -1;
	}
	return fd;
}

static int tty_set_raw(int fd, speed_t baud)
{
	struct termios t;

	if (tcgetattr(fd, &t) < 0)
		return -1;
	cfmakeraw(&t);
	t.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
	t.c_cflag |= CS8 | CREAD | CLOCAL;
	t.c_iflag &= ~(IXON | IXOFF | IXANY);
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	cfsetispeed(&t, baud);
	cfsetospeed(&t, baud);
	if (tcsetattr(fd, TCSANOW, &t) < 0)
		return -1;
	tcflush(fd, TCIOFLUSH);
	return 0;
}

static int open_tty(const char *path, speed_t baud)
{
	int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);

	if (fd < 0) {
		logmsg("cannot open %s: %s", path, strerror(errno));
		return -1;
	}
	if (tty_set_raw(fd, baud) < 0) {
		logmsg("cannot configure %s (%s baud): %s", path,
		       baud == B921600 ? "921600" : "?", strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

/* Append bytes to the pty buffer; drop oldest data if a reader is not
 * keeping up (gpsd normally keeps up trivially). */
static void pty_queue(const char *data, size_t len)
{
	if (g.pty_master < 0)
		return;
	if (g.tx_len + len > sizeof(g.tx)) {
		size_t drop = g.tx_len + len - sizeof(g.tx);
		if (drop > g.tx_len)
			drop = g.tx_len;
		memmove(g.tx, g.tx + drop, g.tx_len - drop);
		g.tx_len -= drop;
	}
	memcpy(g.tx + g.tx_len, data, len);
	g.tx_len += len;
}

static void pty_flush(void)
{
	size_t off = 0;

	while (off < g.tx_len && g.pty_master >= 0) {
		ssize_t n = write(g.pty_master, g.tx + off, g.tx_len - off);
		if (n > 0) {
			off += (size_t)n;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			break;
		if (n < 0 && errno == EINTR)
			continue;
		break;
	}
	if (off) {
		memmove(g.tx, g.tx + off, g.tx_len - off);
		g.tx_len -= off;
	}
}

static bool nmea_checksum_ok(const char *line, size_t len)
{
	unsigned csum = 0;
	const char *p = line, *star = NULL;

	for (p = line + 1; p < line + len; p++) {
		if (*p == '*') {
			star = p;
			break;
		}
		csum ^= (unsigned char)*p;
	}
	if (!star || star + 2 >= line + len)
		return false;
	{
		unsigned got = 0;
		char hex[3] = { star[1], star[2], 0 };
		char *end = NULL;
		got = (unsigned)strtoul(hex, &end, 16);
		if (!end || *end)
			return false;
		return got == csum;
	}
}

/* A line is a real GNSS sentence if it starts with '$G' followed by a
 * two-letter talker (GN, GP, GL, GA, GB, GQ, GI, GS, BD is not $G but
 * we accept only $G* here because the chip's debug log uses $DA/$TC/
 * $CF/$fl/$te/$fr/$AF/$do/$TMS). */
static bool is_gnss_line(const char *line, size_t len)
{
	if (len < 7)
		return false;
	if (line[0] != '$' || line[1] != 'G')
		return false;
	if (!(line[2] >= 'A' && line[2] <= 'Z'))
		return false;
	/* sentence name: 3 uppercase letters, then ',' or '*' */
	if (!((line[3] >= 'A' && line[3] <= 'Z') &&
	      (line[4] >= 'A' && line[4] <= 'Z') &&
	      (line[5] >= 'A' && line[5] <= 'Z')))
		return false;
	return nmea_checksum_ok(line, len);
}

static void handle_line(char *line, size_t len)
{
	if (len == 0)
		return;

	if (is_gnss_line(line, len)) {
		char out[600];
		int n;
		if (len > sizeof(out) - 3)
			return;
		memcpy(out, line, len);
		out[len] = '\r';
		out[len + 1] = '\n';
		n = (int)len + 2;
		pty_queue(out, (size_t)n);
		g.nmea_total++;
		g.last_nmea = time(NULL);
		if (g.nmea_fp) {
			fwrite(out, 1, (size_t)n, g.nmea_fp);
			fflush(g.nmea_fp);
		}
		return;
	}

	/* Everything else is the chip's diagnostic stream.  It is only
	 * shown in verbose mode; it must never reach gpsd. */
	g.debug_lines++;
	if (len >= 14 && memcmp(line, "$PAIR001,002,0", 14) == 0)
		g.saw_ack = true;
	if (g.verbose && len > 3) {
		char tmp[256];
		size_t n = len < sizeof(tmp) - 1 ? len : sizeof(tmp) - 1;
		memcpy(tmp, line, n);
		tmp[n] = '\0';
		logmsg("chip: %s", tmp);
	}
}

/* Consume whatever bytes are available on the tty. */
static void read_tty(int timeout_ms)
{
	struct pollfd pfd = { .fd = g.tty_fd, .events = POLLIN };
	ssize_t n;
	int r;

	r = poll(&pfd, 1, timeout_ms);
	if (r <= 0)
		return;
	if (!(pfd.revents & (POLLIN | POLLHUP | POLLERR)))
		return;

	if (g.rx_len >= sizeof(g.rx) - 1)
		g.rx_len = 0;
	{
		size_t want = sizeof(g.rx) - g.rx_len - 1;
		if (want > 4096)
			want = 4096;
		n = read(g.tty_fd, g.rx + g.rx_len, want);
	}
	if (n <= 0)
		return;

	/* split into lines */
	{
		size_t start = 0, i;
		size_t end = g.rx_len + (size_t)n;
		for (i = 0; i < end; i++) {
			if (g.rx[i] == '\n') {
				size_t len = i - start;
				if (len && g.rx[start + len - 1] == '\r')
					len--;
				handle_line(g.rx + start, len);
				start = i + 1;
			}
		}
		if (start) {
			memmove(g.rx, g.rx + start, end - start);
			end -= start;
		}
		g.rx_len = end;
		if (g.rx_len == sizeof(g.rx)) {
			/* pathological unterminated stream: drop it */
			g.rx_len = 0;
		}
	}
	if (!g.test_mode)
		pty_flush();
}

/* ------------------------------------------------------------------ */
/* chip bring-up                                                       */

/* Write a PAIR command to the tty, tolerating a non-blocking fd. */
static void pair_send(const char *cmd)
{
	size_t off = 0, len = strlen(cmd);

	while (off < len) {
		ssize_t w = write(g.tty_fd, cmd + off, len - off);

		if (w < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				sleep_ms(5);
				continue;
			}
			logmsg("PAIR write failed: %s", strerror(errno));
			return;
		}
		off += (size_t)w;
	}
}

struct probe_ctx {
	bool ack;
	bool nmea;
};

/*
 * Bring the chip up: power on and send $PAIR002 inside the init window.
 * Retries with a power cycle if the firmware did not answer.
 */
static int chip_bringup(struct probe_ctx *ctx)
{
	int attempt;

	for (attempt = 1; attempt <= MAX_INIT_RETRIES; attempt++) {
		struct timespec t0;
		bool acked = false;

		memset(ctx, 0, sizeof(*ctx));
		g.rx_len = 0;
		g.saw_ack = false;
		g.nmea_total = 0;
		g.last_nmea = 0;

		chip_power_off();
		sleep_ms(150);

		/* TTY must be opened *before* power-on so the early probe
		 * bytes land in the init window. */
		if (g.tty_fd < 0) {
			g.tty_fd = open_tty(g.tty_path, g.baud);
			if (g.tty_fd < 0)
				return -1;
		} else {
			tty_set_raw(g.tty_fd, g.baud);
		}

		clock_gettime(CLOCK_MONOTONIC, &t0);
		chip_power_on();

		/* Keep poking $PAIR002 through the whole init window and into
		 * the first second of firmware life; the chip only listens in
		 * that window, and an early reply may be a failure ("...002,1")
		 * before the DSP is ready.  Success = first valid NMEA line. */
		while (ms_since(&t0) < INIT_WINDOW_MS + FIRST_NMEA_TIMEOUT_MS) {
			ssize_t w;

			read_tty(15);
			if (g.nmea_total > 0)
				break;
			if (ms_since(&t0) < INIT_WINDOW_MS + 400) {
				w = write(g.tty_fd, PAIR_OPEN_DSP,
					  strlen(PAIR_OPEN_DSP));
				if (w < 0 && errno != EAGAIN)
					logmsg("probe write failed: %s",
					       strerror(errno));
			}
			sleep_ms(PROBE_INTERVAL_MS);
			acked = g.saw_ack;
		}
		ctx->ack = acked;
		if (g.nmea_total == 0) {
			logmsg("init attempt %d/%d: no NMEA (PAIR002 ack=%s), retrying",
			       attempt, MAX_INIT_RETRIES,
			       acked ? "yes" : "no");
			sleep_ms(200);
			continue;
		}
		ctx->nmea = true;
		if (acked)
			logmsg("chip ACKed $PAIR002 and streams NMEA (attempt %d, %d ms)",
			       attempt, ms_since(&t0));
		else
			logmsg("chip streams NMEA (attempt %d, %d ms; no explicit ack seen)",
			       attempt, ms_since(&t0));

		/* PAIR002 only opens the DSP; configure constellations and NMEA,
		 * then hot-start navigation.  Verified accepted by the AG3335 on
		 * this unit (runtime-only; never writes flash). */
		pair_send(PAIR_QUERY_VERSION);
		sleep_ms(50);
		pair_send(PAIR_GNSS_ALL);
		sleep_ms(50);
		pair_send(PAIR_NMEA_GGA_ON);
		pair_send(PAIR_NMEA_GLL_ON);
		pair_send(PAIR_NMEA_GSA_ON);
		pair_send(PAIR_NMEA_GSV_ON);
		pair_send(PAIR_NMEA_RMC_ON);
		pair_send(PAIR_NMEA_VTG_ON);
		pair_send(PAIR_NMEA_ZDA_ON);
		sleep_ms(50);
		pair_send(PAIR_DEBUG_OFF);
		sleep_ms(50);
		pair_send(PAIR_HOT_START);
		logmsg("runtime init sent: PAIR021/066/062/086/004 (runtime-only)");
		return 0;
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* pty + symlink                                                       */

static int setup_pty(const char *link_path)
{
	struct termios t;

	g.pty_master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (g.pty_master < 0) {
		logmsg("posix_openpt: %s", strerror(errno));
		return -1;
	}
	if (grantpt(g.pty_master) < 0 || unlockpt(g.pty_master) < 0) {
		logmsg("grantpt/unlockpt: %s", strerror(errno));
		return -1;
	}
	if (ptsname_r(g.pty_master, g.pty_slave, sizeof(g.pty_slave)) != 0) {
		logmsg("ptsname_r failed");
		return -1;
	}

	/* Put the slave in raw mode so it does not echo back into us. */
	{
		int slave = open(g.pty_slave, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
		if (slave >= 0) {
			cfmakeraw(&t);
			tcsetattr(slave, TCSANOW, &t);
			close(slave);
		}
	}

	if (link_path && *link_path) {
		unlink(link_path);
		if (symlink(g.pty_slave, link_path) < 0) {
			logmsg("symlink %s -> %s: %s", link_path, g.pty_slave,
			       strerror(errno));
			return -1;
		}
	}
	logmsg("NMEA pty ready: %s%s%s", g.pty_slave,
	       link_path && *link_path ? " (link " : "",
	       link_path && *link_path ? link_path : "");
	if (link_path && *link_path)
		logmsg("point gpsd at it: DEVICES=\"%s\"", link_path);
	return 0;
}

static void teardown_pty(void)
{
	if (g.pty_master >= 0) {
		close(g.pty_master);
		g.pty_master = -1;
	}
	if (g.link_path && *g.link_path)
		unlink(g.link_path);
}

/* ------------------------------------------------------------------ */
/* signals                                                             */

static void on_signal(int sig)
{
	(void)sig;
	g.stop = 1;
}

static void install_signals(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0; /* no SA_RESTART: poll() should return EINTR */
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
}

/* ------------------------------------------------------------------ */
/* main loop                                                           */

static void write_status(void)
{
	FILE *f = fopen("/run/airoha-gnssd.state", "w");

	if (!f)
		return;
	fprintf(f,
		"pid=%d\nstate=%s\nnmea_total=%lu\ndebug_lines=%lu\n"
		"pty=%s\nlink=%s\nuptime=%ld\n",
		(int)getpid(), g.nmea_total ? "streaming" : "starting",
		g.nmea_total, g.debug_lines, g.pty_slave,
		g.link_path ? g.link_path : "", (long)(time(NULL) - g.started));
	fclose(f);
}

static int run_loop(void)
{
	int next_status = 0;

	while (!g.stop) {
		struct pollfd pfd[2];
		nfds_t nfds = 1;
		int r;

		pfd[0].fd = g.tty_fd;
		pfd[0].events = POLLIN;
		pfd[1].fd = g.pty_master;
		pfd[1].events = POLLOUT;

		if (g.tx_len)
			nfds = 2;

		r = poll(pfd, nfds, 500);
		if (r < 0 && errno == EINTR)
			continue;
		if (r > 0) {
			if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR))
				read_tty(0);
			if (nfds == 2 && (pfd[1].revents & POLLOUT))
				pty_flush();
		}

		/* health check: NMEA should arrive at least once per second */
		if (g.last_nmea && time(NULL) - g.last_nmea > HEALTH_TIMEOUT_MS / 1000) {
			logmsg("no NMEA for %d s, restarting chip",
			       HEALTH_TIMEOUT_MS / 1000);
			if (chip_bringup(&(struct probe_ctx){0}) < 0) {
				logmsg("restart failed, retrying in 2 s");
				sleep_ms(2000);
			}
		}

		if (time(NULL) >= next_status) {
			write_status();
			next_status = (int)time(NULL) + 2;
			if (g.verbose)
				logmsg("stats: nmea=%lu debug=%lu",
				       g.nmea_total, g.debug_lines);
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  -d, --device PATH    GNSS UART (default %s)\n"
		"  -c, --control PATH   power control node (default %s)\n"
		"  -l, --link PATH      symlink to expose NMEA pty (default %s,\n"
		"                       empty string = no symlink)\n"
		"  -b, --baud RATE      baud rate (default 921600)\n"
		"  -f, --log-file PATH  append daemon log to PATH\n"
		"  -n, --nmea-log PATH  append raw NMEA to PATH\n"
		"  -t, --test SECONDS   run init, stream NMEA to stdout for SECONDS,\n"
		"                       power off and exit (for validation)\n"
		"  -v, --verbose        log chip diagnostic lines too\n"
		"  -V, --version\n"
		"  -h, --help\n", argv0, DEFAULT_TTY, DEFAULT_CTL, DEFAULT_LINK);
}

static int parse_baud(const char *s)
{
	switch (atoi(s)) {
	case 9600: return B9600;
	case 19200: return B19200;
	case 38400: return B38400;
	case 57600: return B57600;
	case 115200: return B115200;
	case 230400: return B230400;
	case 460800: return B460800;
	case 921600: return B921600;
	default: return -1;
	}
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "device",    required_argument, NULL, 'd' },
		{ "control",   required_argument, NULL, 'c' },
		{ "link",      required_argument, NULL, 'l' },
		{ "baud",      required_argument, NULL, 'b' },
		{ "log-file",  required_argument, NULL, 'f' },
		{ "nmea-log",  required_argument, NULL, 'n' },
		{ "test",      required_argument, NULL, 't' },
		{ "verbose",   no_argument,       NULL, 'v' },
		{ "version",   no_argument,       NULL, 'V' },
		{ "help",      no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	int test_seconds = 0;
	int c;

	memset(&g, 0, sizeof(g));
	g.tty_path = DEFAULT_TTY;
	g.ctl_path = DEFAULT_CTL;
	g.link_path = DEFAULT_LINK;
	g.baud = DEFAULT_BAUD;
	g.tty_fd = -1;
	g.ctl_fd = -1;
	g.pty_master = -1;

	while ((c = getopt_long(argc, argv, "d:c:l:b:f:n:t:vVh", opts, NULL)) != -1) {
		switch (c) {
		case 'd': g.tty_path = optarg; break;
		case 'c': g.ctl_path = optarg; break;
		case 'l': g.link_path = optarg; break;
		case 'b':
			g.baud = (speed_t)parse_baud(optarg);
			if ((int)g.baud < 0) {
				fprintf(stderr, "bad baud: %s\n", optarg);
				return 2;
			}
			break;
		case 'f': g.log_path = optarg; break;
		case 'n': g.nmea_log_path = optarg; break;
		case 't': test_seconds = atoi(optarg); g.test_mode = true; break;
		case 'v': g.verbose = true; break;
		case 'V': printf("airoha-gnssd %s\n", VERSION); return 0;
		case 'h': usage(argv[0]); return 0;
		default: usage(argv[0]); return 2;
		}
	}

	g.started = time(NULL);

	if (g.log_path) {
		g.log_fp = fopen(g.log_path, "a");
		if (!g.log_fp)
			logmsg("warning: cannot open log %s: %s", g.log_path,
			       strerror(errno));
	}
	if (g.nmea_log_path) {
		g.nmea_fp = fopen(g.nmea_log_path, "a");
		if (!g.nmea_fp)
			logmsg("warning: cannot open nmea log %s: %s",
			       g.nmea_log_path, strerror(errno));
	}

	install_signals();

	g.ctl_fd = open_ctl(g.ctl_path);
	if (g.ctl_fd < 0)
		return 1;

	/* make sure we start from a known state */
	chip_power_off();
	sleep_ms(200);

	if (setup_pty(test_seconds ? "" : g.link_path) < 0) {
		chip_power_off();
		return 1;
	}

	/* open the tty before power-on: the init window is < 1 s */
	g.tty_fd = open_tty(g.tty_path, g.baud);
	if (g.tty_fd < 0) {
		teardown_pty();
		chip_power_off();
		return 1;
	}

	{
		struct probe_ctx ctx;
		if (chip_bringup(&ctx) < 0) {
			(void)0;
			logmsg("failed to bring up the GNSS chip");
			if (g.tty_fd >= 0) close(g.tty_fd);
			teardown_pty();
			chip_power_off();
			return 1;
		}
	}

	if (test_seconds > 0) {
		/* Validation mode: copy NMEA to stdout, then power down. */
		time_t end = time(NULL) + test_seconds;
		while (time(NULL) < end && !g.stop) {
			read_tty(200);
			while (g.tx_len) {
				ssize_t w = write(STDOUT_FILENO, g.tx, g.tx_len);
				if (w <= 0)
					break;
				memmove(g.tx, g.tx + w, g.tx_len - (size_t)w);
				g.tx_len -= (size_t)w;
			}
		}
		logmsg("test done: %lu NMEA sentences in %d s",
		       g.nmea_total, test_seconds);
		chip_power_off();
		if (g.tty_fd >= 0) close(g.tty_fd);
		teardown_pty();
		return g.nmea_total > 0 ? 0 : 1;
	}

	logmsg("airoha-gnssd %s running (device=%s baud=921600)", VERSION,
	       g.tty_path);
	write_status();
	sd_notify("READY=1\nSTATUS=streaming NMEA to " DEFAULT_LINK);
	{
		char st[160];
		snprintf(st, sizeof(st), "STATUS=streaming, nmea_total=%lu",
			 g.nmea_total);
		sd_notify(st);
	}
	run_loop();

	logmsg("shutting down");
	chip_power_off();
	if (g.tty_fd >= 0)
		close(g.tty_fd);
	teardown_pty();
	if (g.log_fp)
		fclose(g.log_fp);
	if (g.nmea_fp)
		fclose(g.nmea_fp);
	return 0;
}

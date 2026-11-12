// SPDX-License-Identifier: GPL-2.0
/*
 * gps-nmea-share - bridge gpsd NMEA stream to GeoClue2's "network NMEA" source.
 *
 * GeoClue2's network-nmea source (nmea-socket=...) connects to a Unix stream
 * socket and reads plain NMEA lines (GGA/RMC are used), see gclue-nmea-source.c.
 * gpsd already owns the GNSS tty, so this daemon subscribes to gpsd's TCP
 * interface with JSON WATCH {"nmea":true} and republishes the NMEA lines on a
 * Unix socket for GeoClue.
 *
 * Usage: gps-nmea-share [-s /run/gps-share.sock] [-H 127.0.0.1] [-p 2947] [-v]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define MAX_CLIENTS 16
#define LINE_MAX_LEN 1024
#define GPSD_WATCH "?WATCH={\"enable\":true,\"nmea\":true}\n"

static volatile sig_atomic_t g_stop;
static bool g_verbose;

static void on_sig(int sig) { (void)sig; g_stop = 1; }

static void logmsg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "gps-nmea-share: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static void msleep(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
	while (nanosleep(&ts, &ts) < 0 && errno == EINTR && !g_stop)
		;
}

static int connect_gpsd(const char *host, int port)
{
	struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port) };
	int fd;

	if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
		logmsg("bad gpsd address %s", host);
		return -1;
	}
	fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		close(fd);
		return -1;
	}
	if (write(fd, GPSD_WATCH, strlen(GPSD_WATCH)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int open_listener(const char *path)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	struct sockaddr_un tmp;
	socklen_t tlen = sizeof(tmp);
	int fd;

	if (strlen(path) >= sizeof(sa.sun_path)) {
		logmsg("socket path too long");
		return -1;
	}
	/* remove stale socket if nothing is listening on it */
	if (access(path, F_OK) == 0) {
		int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (probe >= 0) {
			memset(&tmp, 0, sizeof(tmp));
			tmp.sun_family = AF_UNIX;
			strncpy(tmp.sun_path, path, sizeof(tmp.sun_path) - 1);
			if (connect(probe, (struct sockaddr *)&tmp, tlen) < 0)
				unlink(path);
			close(probe);
		}
	}
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
	unlink(path);
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		logmsg("bind %s: %s", path, strerror(errno));
		close(fd);
		return -1;
	}
	chmod(path, 0666);   /* geoclue runs as user 'geoclue' */
	if (listen(fd, 8) < 0) {
		logmsg("listen: %s", strerror(errno));
		close(fd);
		unlink(path);
		return -1;
	}
	return fd;
}

static void forward_line(const char *line, size_t len, int *clients, int *nclients)
{
	char out[LINE_MAX_LEN + 2];
	size_t out_len = len + 2;
	int i;

	if (len < 6 || len > LINE_MAX_LEN || line[0] != '$')
		return;
	memcpy(out, line, len);
	out[len] = '\r';
	out[len + 1] = '\n';
	for (i = 0; i < *nclients; ) {
		ssize_t n = send(clients[i], out, out_len, MSG_NOSIGNAL);
		if (n == (ssize_t)out_len) {
			i++;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			i++;              /* slow client; drop this line only */
			continue;
		}
		close(clients[i]);
		clients[i] = clients[--(*nclients)];
	}
}

int main(int argc, char **argv)
{
	const char *sock_path = "/run/gps-share.sock";
	const char *gpsd_host = "127.0.0.1";
	int gpsd_port = 2947;
	int listen_fd, gpsd_fd = -1;
	int clients[MAX_CLIENTS];
	int nclients = 0;
	char buf[8192];
	size_t buf_len = 0;
	struct sigaction sa;
	int c;

	static const struct option long_opts[] = {
		{ "socket",    required_argument, NULL, 's' },
		{ "gpsd-host", required_argument, NULL, 'H' },
		{ "port",      required_argument, NULL, 'p' },
		{ "verbose",   no_argument,       NULL, 'v' },
		{ "help",      no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};

	while ((c = getopt_long(argc, argv, "s:H:p:vh", long_opts, NULL)) != -1) {
		switch (c) {
		case 's': sock_path = optarg; break;
		case 'H': gpsd_host = optarg; break;
		case 'p': gpsd_port = atoi(optarg); break;
		case 'v': g_verbose = true; break;
		case 'h':
			printf("Usage: %s [-s socket] [-H gpsd-host] [-p port] [-v]\n",
			       argv[0]);
			return 0;
		default:
			return 2;
		}
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_sig;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	listen_fd = open_listener(sock_path);
	if (listen_fd < 0)
		return 1;
	logmsg("listening on %s -> gpsd %s:%d", sock_path, gpsd_host, gpsd_port);

	while (!g_stop) {
		struct pollfd pfd[2 + MAX_CLIENTS];
		nfds_t nfds = 0, base;
		int i, r;

		if (gpsd_fd < 0) {
			gpsd_fd = connect_gpsd(gpsd_host, gpsd_port);
			if (gpsd_fd < 0) {
				if (g_verbose)
					logmsg("waiting for gpsd at %s:%d",
					       gpsd_host, gpsd_port);
				msleep(1000);
				continue;
			}
			logmsg("connected to gpsd %s:%d", gpsd_host, gpsd_port);
			buf_len = 0;
		}

		pfd[0].fd = listen_fd;
		pfd[0].events = POLLIN;
		pfd[1].fd = gpsd_fd;
		pfd[1].events = POLLIN;
		nfds = 2;
		base = 2;
		for (i = 0; i < nclients; i++) {
			pfd[base + i].fd = clients[i];
			pfd[base + i].events = POLLIN;
			nfds++;
		}

		r = poll(pfd, nfds, 1000);
		if (r < 0 && errno == EINTR)
			continue;

		/* new GeoClue client */
		if (r > 0 && (pfd[0].revents & POLLIN)) {
			int cfd = accept4(listen_fd, NULL, NULL,
					  SOCK_NONBLOCK | SOCK_CLOEXEC);
			if (cfd >= 0) {
				if (nclients < MAX_CLIENTS) {
					clients[nclients++] = cfd;
					logmsg("client connected (%d total)",
					       nclients);
				} else {
					close(cfd);
				}
			}
		}

		/* gpsd data */
		if (r > 0 && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
			ssize_t n = read(gpsd_fd, buf + buf_len,
					 sizeof(buf) - buf_len);
			if (n <= 0) {
				logmsg("gpsd connection lost");
				close(gpsd_fd);
				gpsd_fd = -1;
				buf_len = 0;
				continue;
			}
			buf_len += (size_t)n;
			{
				size_t start = 0, j;
				for (j = 0; j < buf_len; j++) {
					if (buf[j] == '\n') {
						size_t len = j - start;
						if (len && buf[start + len - 1] == '\r')
							len--;
						forward_line(buf + start, len,
							     clients, &nclients);
						start = j + 1;
					}
				}
				if (start) {
					memmove(buf, buf + start, buf_len - start);
					buf_len -= start;
				}
				if (buf_len >= sizeof(buf))
					buf_len = 0;
			}
		}

		/* client hangup (we never expect data from clients) */
		if (r > 0) {
			for (i = 0; i < nclients; ) {
				int ci = base + i;
				if (pfd[ci].revents & (POLLHUP | POLLERR | POLLNVAL)) {
					close(clients[i]);
					clients[i] = clients[--nclients];
					continue;
				}
				if (pfd[ci].revents & POLLIN) {
					char junk[64];
					if (read(clients[i], junk, sizeof(junk)) <= 0) {
						close(clients[i]);
						clients[i] = clients[--nclients];
						continue;
					}
				}
				i++;
			}
		}
	}

	if (gpsd_fd >= 0)
		close(gpsd_fd);
	for (int k = 0; k < nclients; k++)
		close(clients[k]);
	close(listen_fd);
	unlink(sock_path);
	logmsg("stopped");
	return 0;
}

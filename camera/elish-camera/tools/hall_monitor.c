/*
 * hall_monitor.c - print the state of every kernel switch (SW_*) and log
 * changes with timestamps.  Used to compare the magnetic cover / keyboard
 * states of the elish tablet.
 *
 * Build: gcc -O2 -o hall_monitor hall_monitor.c
 * Run:   hall_monitor [seconds]   (0 = forever)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#define MAX_DEV 32

struct swdev {
    int fd;
    char path[32];
    char name[128];
    unsigned long sw_bits;
};

static volatile sig_atomic_t running = 1;
static void on_signal(int sig) { (void)sig; running = 0; }

static const char *sw_name(int code)
{
    switch (code) {
    case SW_LID: return "SW_LID";
    case SW_TABLET_MODE: return "SW_TABLET_MODE";
    case SW_HEADPHONE_INSERT: return "SW_HEADPHONE_INSERT";
    case SW_MICROPHONE_INSERT: return "SW_MICROPHONE_INSERT";
    case SW_JACK_PHYSICAL_INSERT: return "SW_JACK_PHYSICAL_INSERT";
    case SW_VIDEOOUT_INSERT: return "SW_VIDEOOUT_INSERT";
    case SW_LINEIN_INSERT: return "SW_LINEIN_INSERT";
    case SW_CAMERA_LENS_COVER: return "SW_CAMERA_LENS_COVER";
    default: return "SW_?";
    }
}

static void print_state(struct swdev *d)
{
    unsigned long sw = 0;
    if (ioctl(d->fd, EVIOCGSW(sizeof(sw)), &sw) < 0)
        return;
    printf("STATE %s (%s): sw=0x%lx", d->path, d->name, sw);
    for (int b = 0; b < 16; b++)
        if (sw & (1ul << b))
            printf(" %s=1", sw_name(b));
    printf("\n");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int seconds = argc > 1 ? atoi(argv[1]) : 0;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    setvbuf(stdout, NULL, _IOLBF, 0);

    struct swdev devs[MAX_DEV];
    int n = 0;
    for (int i = 0; i < 64 && n < MAX_DEV; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;
        char name[128] = {0};
        unsigned long swbits = 0;
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0 ||
            ioctl(fd, EVIOCGBIT(EV_SW, sizeof(swbits)), &swbits) < 0 ||
            swbits == 0) {
            close(fd);
            continue;
        }
        struct swdev *d = &devs[n++];
        d->fd = fd;
        snprintf(d->path, sizeof(d->path), "%s", path);
        snprintf(d->name, sizeof(d->name), "%s", name);
        d->sw_bits = swbits;
        printf("DEV %s (%s): switch caps=0x%lx\n", d->path, d->name, swbits);
        print_state(d);
    }
    if (n == 0) {
        printf("no switch devices found\n");
        return 1;
    }

    uint64_t start = 0;
    struct timespec ts0;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    start = (uint64_t)ts0.tv_sec;

    while (running) {
        fd_set rfds;
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        FD_ZERO(&rfds);
        int maxfd = -1;
        for (int i = 0; i < n; i++) {
            FD_SET(devs[i].fd, &rfds);
            if (devs[i].fd > maxfd)
                maxfd = devs[i].fd;
        }
        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (r > 0) {
            for (int i = 0; i < n; i++) {
                if (!FD_ISSET(devs[i].fd, &rfds))
                    continue;
                struct input_event ev;
                while (read(devs[i].fd, &ev, sizeof(ev)) == sizeof(ev)) {
                    if (ev.type != EV_SW)
                        continue;
                    struct timespec now;
                    clock_gettime(CLOCK_MONOTONIC, &now);
                    printf("EVENT t=%lu.%03lu %s (%s) %s -> %d\n",
                           (unsigned long)(now.tv_sec - (time_t)start),
                           (unsigned long)(now.tv_nsec / 1000000),
                           devs[i].path, devs[i].name, sw_name(ev.code), ev.value);
                    if (ev.code == SW_LID || ev.code == SW_TABLET_MODE)
                        print_state(&devs[i]);
                }
            }
        }
        if (seconds > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if ((uint64_t)(now.tv_sec - (time_t)start) >= (uint64_t)seconds)
                break;
        }
    }
    for (int i = 0; i < n; i++)
        close(devs[i].fd);
    return 0;
}

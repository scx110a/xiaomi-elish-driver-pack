/*
 * tablet_mode_switch.c - control the "tablet mode" state used by mutter's
 * automatic screen rotation.
 *
 * On this tablet the magnetic cover's hall sensor is exposed as
 * SW_TABLET_MODE on the gpio-keys input device, and mutter only enables
 * auto-rotate when it believes the device is in tablet mode
 * (panel_orientation_managed = touch_mode && has_accelerometer && laptop_panel).
 *
 * There is no reliable keybaord-detect signal in userspace (the pogo USB
 * keyboard interface is always enumerated), so the behaviour is configurable:
 *
 *   /etc/elish-orientation.conf:
 *     tablet_mode = on      always tablet mode  -> auto-rotate always on
 *                 = off     never tablet mode  -> auto-rotate always off
 *                 = auto    mirror the real hall switch (default: on)
 *
 * The real SW_LID is never touched, so cover-close suspend via logind keeps
 * working; this helper only publishes a virtual SW_TABLET_MODE.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CONFIG_PATH "/etc/elish-orientation.conf"

static volatile sig_atomic_t running = 1;
static void on_signal(int sig) { (void)sig; running = 0; }

static int find_gpio_keys_event(char *path, size_t path_len)
{
    for (int i = 0; i < 64; i++) {
        char p[64];
        snprintf(p, sizeof(p), "/dev/input/event%d", i);
        int fd = open(p, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;
        char name[256] = {0};
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 &&
            strstr(name, "gpio-keys")) {
            close(fd);
            snprintf(path, path_len, "%s", p);
            return 0;
        }
        close(fd);
    }
    return -1;
}

static int read_tablet_mode(void)
{
    FILE *f = fopen(CONFIG_PATH, "r");
    int mode = 1; /* default: on */
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char *p = strchr(line, '=');
            if (!p)
                continue;
            *p++ = 0;
            while (*p == ' ' || *p == '\t')
                p++;
            char *end = p + strlen(p);
            while (end > p && (end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\r'))
                *--end = 0;
            if (!strcmp(line, "tablet_mode")) {
                if (!strcmp(p, "on"))
                    mode = 1;
                else if (!strcmp(p, "off"))
                    mode = 0;
                else
                    mode = -1; /* auto */
            }
        }
        fclose(f);
    }
    return mode; /* 1 on, 0 off, -1 auto */
}

static int real_switch_state(int gfd)
{
    unsigned long sw = 0;
    if (gfd < 0 || ioctl(gfd, EVIOCGSW(sizeof(sw)), &sw) < 0)
        return 0;
    return (sw >> SW_TABLET_MODE) & 1;
}

static int desired_state(int mode, int gfd)
{
    if (mode == 1)
        return 1;
    if (mode == 0)
        return 0;
    return real_switch_state(gfd); /* auto: mirror the hall switch */
}

static void emit_switch(int fd, int value, int *current)
{
    if (value == *current)
        return;
    struct input_event ev[2];
    memset(ev, 0, sizeof(ev));
    ev[0].type = EV_SW;
    ev[0].code = SW_TABLET_MODE;
    ev[0].value = value;
    ev[1].type = EV_SYN;
    ev[1].code = SYN_REPORT;
    if (write(fd, ev, sizeof(ev)) < 0)
        return;
    *current = value;
    fprintf(stderr, "tablet-mode-switch: SW_TABLET_MODE=%d\n", value);
}

int main(void)
{
    struct uinput_setup setup;
    int ufd, gfd = -1;
    char gpath[64] = {0};

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (ufd < 0) {
        fprintf(stderr, "tablet-mode-switch: open /dev/uinput: %s\n", strerror(errno));
        return 1;
    }
    if (ioctl(ufd, UI_SET_EVBIT, EV_SW) < 0 ||
        ioctl(ufd, UI_SET_SWBIT, SW_TABLET_MODE) < 0) {
        fprintf(stderr, "tablet-mode-switch: uinput setup: %s\n", strerror(errno));
        return 1;
    }
    memset(&setup, 0, sizeof(setup));
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor = 0x1234;
    setup.id.product = 0x0002;
    snprintf(setup.name, sizeof(setup.name), "elish-tablet-mode");
    if (ioctl(ufd, UI_DEV_SETUP, &setup) < 0 || ioctl(ufd, UI_DEV_CREATE) < 0) {
        fprintf(stderr, "tablet-mode-switch: create device: %s\n", strerror(errno));
        return 1;
    }
    usleep(150000);

    if (find_gpio_keys_event(gpath, sizeof(gpath)) == 0)
        gfd = open(gpath, O_RDONLY | O_NONBLOCK);
    if (gfd < 0)
        fprintf(stderr, "tablet-mode-switch: gpio-keys not found, using config only\n");

    int current = -1;
    int last_mode = -2;
    struct timespec last_poll = {0};
    clock_gettime(CLOCK_MONOTONIC, &last_poll);

    while (running) {
        int mode = read_tablet_mode();
        if (mode != last_mode) {
            const char *s = mode == 1 ? "on" : mode == 0 ? "off" : "auto";
            fprintf(stderr, "tablet-mode-switch: config tablet_mode=%s\n", s);
            last_mode = mode;
            /* force re-assert so a mode change is applied immediately */
            current = -1;
        }
        int want = desired_state(mode, gfd);
        emit_switch(ufd, want, &current);

        fd_set rfds;
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (gfd >= 0) {
            FD_SET(gfd, &rfds);
            maxfd = gfd;
        }
        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (r > 0 && gfd >= 0 && FD_ISSET(gfd, &rfds)) {
            struct input_event ev;
            while (read(gfd, &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type == EV_SW && ev.code == SW_TABLET_MODE) {
                    want = desired_state(mode, gfd);
                    emit_switch(ufd, want, &current);
                }
            }
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - last_poll.tv_sec) * 1000L +
                          (now.tv_nsec - last_poll.tv_nsec) / 1000000L;
        if (elapsed_ms >= 1000) {
            last_poll = now;
            if (mode == -1) {
                /* keep mirroring the hall switch */
                emit_switch(ufd, real_switch_state(gfd), &current);
            } else if (current != mode) {
                emit_switch(ufd, mode, &current);
            }
        }
    }

    ioctl(ufd, UI_DEV_DESTROY);
    if (gfd >= 0)
        close(gfd);
    close(ufd);
    return 0;
}

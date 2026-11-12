CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/sbin

all: airoha-gnssd gps-nmea-share

airoha-gnssd: airoha-gnssd.c
	$(CC) $(CFLAGS) -o $@ $<

gps-nmea-share: gps-nmea-share.c
	$(CC) $(CFLAGS) -o $@ $<

install: all
	install -D -m 755 airoha-gnssd $(DESTDIR)$(BINDIR)/airoha-gnssd
	install -D -m 755 gps-nmea-share $(DESTDIR)$(BINDIR)/gps-nmea-share

clean:
	rm -f airoha-gnssd gps-nmea-share

.PHONY: all install clean

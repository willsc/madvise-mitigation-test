# lru-isolate - userspace mitigation for Ubuntu LP#2165410
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS_PTHREAD = -lpthread
PREFIX  ?= /usr/local
BINDIR   = $(PREFIX)/bin
UNITDIR ?= /lib/systemd/system

BINS = lru-isolate lru-verify premigrate rt-spinner

all: $(BINS)

lru-isolate: src/lru-isolate.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS_PTHREAD)

lru-verify: src/lru-verify.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS_PTHREAD)

# test load only - not installed by default, see TESTING.md
rt-spinner: src/rt-spinner.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS_PTHREAD)

premigrate: src/premigrate.c
	$(CC) $(CFLAGS) -o $@ $<

check: lru-isolate
	./lru-isolate check

# causal proof: baseline vs mitigated
verify: lru-verify
	./lru-verify

# Controller regression tests: no FIFO spinners, NUMA changes or tracing.
test: lru-isolate tests/test-drainer tests/test-check tests/fail-madvise.so
	./tests/test-drainer
	./tests/test-check
	python3 tests/test-cli.py ./lru-isolate ./tests/fail-madvise.so
	PYTHONDONTWRITEBYTECODE=1 python3 tests/test-capture.py

tests/test-drainer: tests/test-drainer.c src/lru-isolate.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS_PTHREAD)

tests/test-check: tests/test-check.c src/lru-isolate.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS_PTHREAD)

tests/fail-madvise.so: tests/fail-madvise.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $<

# is the drained state stable, or is guard just losing the race slower?
decay: lru-verify
	./lru-verify --decay

# rt-spinner is a test load that can starve per-CPU kworkers; it is built but
# deliberately not installed. Copy it by hand onto a disposable test box.
INSTALL_BINS = lru-isolate lru-verify premigrate

install: $(INSTALL_BINS)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(INSTALL_BINS) $(DESTDIR)$(BINDIR)
	install -d $(DESTDIR)$(UNITDIR)
	install -m 0644 systemd/*.service $(DESTDIR)$(UNITDIR)

uninstall:
	rm -f $(addprefix $(DESTDIR)$(BINDIR)/,$(INSTALL_BINS))
	rm -f $(DESTDIR)$(UNITDIR)/lru-isolate-guard.service

clean:
	rm -f $(BINS) lru-probe
	rm -f tests/test-drainer tests/test-check tests/fail-madvise.so

.PHONY: all check verify test decay install uninstall clean

// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * lru-verify - causal proof + re-dirty measurement for LP#2165410
 *
 * Replaces the earlier lru-probe + verify-mitigation.sh pair.  Being a single
 * process is not cosmetic: the shell version forked grep/seq/subshells inside
 * the measurement window, and every fork faults pages and dirties LRU batches
 * on whatever CPU it lands on - noise in exactly the signal being measured.
 * Nothing here forks.
 *
 * WHAT IT MEASURES
 * ----------------
 * compare (default)
 *     baseline : dirty CPU N's LRU batch, trigger lru_cache_disable()
 *                -> drain work is queued to CPU N
 *     mitigated: same, but madvise(MADV_WILLNEED) on CPU N first
 *                -> no work is queued to CPU N
 *
 *     On the affected kernels that queued work is what PID 1 blocks on in
 *     flush_work() when CPU N is a FIFO-saturated isolated CPU.
 *
 * decay (--decay)
 *     drain CPU N, wait, trigger.  Did anything re-dirty the batch in the
 *     meantime?  Swept over several delays this gives the re-dirty rate, which
 *     is the number that decides whether `lru-isolate guard' is holding a
 *     stable state or just losing a race more slowly.  See the README section
 *     "Operating model".
 *
 * HOW IT OBSERVES
 * ---------------
 * A kprobe on lru_add_drain_per_cpu (the per-CPU work function), plus the
 * per-CPU ring buffer: after a trigger, /sys/kernel/tracing/per_cpu/cpuN/stats
 * reports "entries: > 0" iff the probe fired on CPU N.  No trace-text parsing
 * and no matching on the "[015]" CPU column.
 *
 * lru_cache_disable() is reached with move_pages(2): do_pages_move() opens with
 * the same unconditional lru_cache_disable() that the cpuset path makes via
 * cpuset_migrate_mm() -> do_migrate_pages().  So this needs no SCHED_FIFO
 * spinner, no NUMA and no cpuset write, and is safe on a live machine.
 *
 * ISOLATED-CORE DISCIPLINE
 * ------------------------
 * The measurement is only honest if the harness does not itself dirty the CPU
 * it is testing.  Same discipline as lru-isolate:
 *
 *  - The dirty and drain workers are persistent and parked on a futex, so no
 *    clone()/exit() runs on the target CPU inside a trial.  A per-trial drain
 *    thread would run its pthread_exit() path on the target CPU *after* the
 *    madvise and could re-dirty the batch it just emptied.
 *
 *  - mlockall(MCL_CURRENT) locks what is already mapped.  Deliberately NOT
 *    MCL_FUTURE: the dirty region must stay unpopulated so it faults fresh on
 *    the target CPU, which is the whole point of the baseline.
 *
 *  - The drain worker runs on an explicitly pre-faulted, mlocked stack
 *    (pthread_attr_setstack), so it cannot fault on the target CPU at all.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef MPOL_MF_MOVE
#define MPOL_MF_MOVE (1 << 1)
#endif
#ifndef MADV_NOHUGEPAGE
#define MADV_NOHUGEPAGE 15
#endif

#define PROBE_NAME "lruverify"
#define DRAIN_STACK (512 * 1024)

static char tracefs[192] = "/sys/kernel/tracing";
static int probe_added, probe_enabled, saved_tracing_on = -1;
static size_t pagesz;
static int verbose;

/* ------------------------------------------------------------------ tracefs */

static void tf_path(char *out, size_t n, const char *rel)
{
	snprintf(out, n, "%s/%s", tracefs, rel);
}

static int tf_put(const char *rel, const char *val, int append)
{
	char path[320];
	int fd, flags = O_WRONLY | (append ? O_APPEND : O_TRUNC);
	ssize_t w;

	tf_path(path, sizeof(path), rel);
	fd = open(path, flags);
	if (fd < 0)
		return -1;
	w = write(fd, val, strlen(val));
	close(fd);
	return w < 0 ? -1 : 0;
}

static int tf_get(const char *rel, char *buf, size_t n)
{
	char path[320];
	int fd;
	ssize_t r;

	tf_path(path, sizeof(path), rel);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	r = read(fd, buf, n - 1);
	close(fd);
	if (r < 0)
		return -1;
	buf[r] = 0;
	return 0;
}

/* clear the whole ring buffer */
static void tf_clear(void)
{
	char path[320];
	int fd;

	tf_path(path, sizeof(path), "trace");
	fd = open(path, O_WRONLY | O_TRUNC);
	if (fd >= 0)
		close(fd);
}

/* events currently sitting in CPU `cpu's per-CPU ring buffer */
static long tf_entries(int cpu)
{
	char rel[64], buf[512], *p;

	snprintf(rel, sizeof(rel), "per_cpu/cpu%d/stats", cpu);
	if (tf_get(rel, buf, sizeof(buf)) != 0)
		return -1;
	p = strstr(buf, "entries:");
	if (!p)
		return -1;
	return strtol(p + strlen("entries:"), NULL, 10);
}

/* async-signal-safe enough for a handler: open/write/close only */
static void probe_teardown(void)
{
	if (probe_enabled) {
		tf_put("events/kprobes/" PROBE_NAME "/enable", "0\n", 0);
		probe_enabled = 0;
	}
	if (probe_added) {
		tf_put("kprobe_events", "-:" PROBE_NAME "\n", 1);
		probe_added = 0;
	}
	if (saved_tracing_on >= 0) {
		tf_put("tracing_on", saved_tracing_on ? "1\n" : "0\n", 0);
		saved_tracing_on = -1;
	}
}

static void on_signal(int sig)
{
	probe_teardown();
	_exit(128 + sig);
}

static int probe_setup(void)
{
	char buf[64];

	if (tf_get("tracing_on", buf, sizeof(buf)) == 0)
		saved_tracing_on = atoi(buf);

	/* append, so a probe someone else installed is left alone */
	if (tf_put("kprobe_events", "p:" PROBE_NAME " lru_add_drain_per_cpu\n", 1) != 0) {
		fprintf(stderr,
			"cannot add kprobe on lru_add_drain_per_cpu: %s\n"
			"  (need root, CONFIG_KPROBES, and tracefs at %s)\n",
			strerror(errno), tracefs);
		return -1;
	}
	probe_added = 1;

	if (tf_put("events/kprobes/" PROBE_NAME "/enable", "1\n", 0) != 0) {
		fprintf(stderr, "cannot enable kprobe: %s\n", strerror(errno));
		return -1;
	}
	probe_enabled = 1;
	return 0;
}

/* ------------------------------------------------------------------- workers */

static int futex_wait(atomic_int *a, int v)
{
	return (int)syscall(SYS_futex, (void *)a, FUTEX_WAIT_PRIVATE, v,
			    NULL, NULL, 0);
}

static int futex_wake(atomic_int *a)
{
	return (int)syscall(SYS_futex, (void *)a, FUTEX_WAKE_PRIVATE, 1,
			    NULL, NULL, 0);
}

enum { W_DIRTY, W_DRAIN };

struct worker {
	int kind;
	int cpu;
	pthread_t th;
	int live;
	atomic_int req, done, ready;
	void *mem;		/* dirty: region to fault. drain: scratch page */
	int npages;
	int ran_on;
	int err;
};

static void *worker_fn(void *arg)
{
	struct worker *w = arg;
	int seen = 0;

	w->ran_on = sched_getcpu();
	atomic_store(&w->ready, 1);
	futex_wake(&w->ready);

	for (;;) {
		int req;

		for (;;) {
			req = atomic_load(&w->req);
			if (req != seen)
				break;
			futex_wait(&w->req, seen);
		}
		if (req < 0)
			break;
		seen = req;

		if (w->kind == W_DIRTY) {
			/* fault pages in here, so folio_add_lru_vma() puts them
			 * in THIS CPU's lru_add batch */
			for (int i = 0; i < w->npages; i++)
				((volatile char *)w->mem)[(size_t)i * pagesz] = 1;
		} else {
			/* untouched anon page: the walk moves nothing, then
			 * lru_add_drain() runs on this CPU */
			if (madvise(w->mem, pagesz, MADV_WILLNEED) != 0)
				w->err = errno;
		}

		atomic_store(&w->done, seen);
		futex_wake(&w->done);
	}
	return NULL;
}

static int worker_start(struct worker *w, int kind, int cpu, void *mem,
			int npages, void *stack, size_t stacksz)
{
	pthread_attr_t attr;
	cpu_set_t set;
	int rc, v;

	w->kind = kind;
	w->cpu = cpu;
	w->mem = mem;
	w->npages = npages;
	w->ran_on = -1;
	atomic_init(&w->req, 0);
	atomic_init(&w->done, 0);
	atomic_init(&w->ready, 0);

	pthread_attr_init(&attr);
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	pthread_attr_setaffinity_np(&attr, sizeof(set), &set);
	if (stack)
		pthread_attr_setstack(&attr, stack, stacksz);
	rc = pthread_create(&w->th, &attr, worker_fn, w);
	pthread_attr_destroy(&attr);
	if (rc) {
		fprintf(stderr, "pthread_create on cpu%d: %s\n", cpu,
			strerror(rc));
		return -1;
	}
	w->live = 1;

	while ((v = atomic_load(&w->ready)) == 0)
		futex_wait(&w->ready, v);
	if (w->ran_on != cpu)
		fprintf(stderr, "warning: worker wanted cpu%d, landed on cpu%d\n",
			cpu, w->ran_on);
	return 0;
}

static void worker_run(struct worker *w, int gen)
{
	int v;

	if (!w->live)
		return;
	atomic_store(&w->req, gen);
	futex_wake(&w->req);
	while ((v = atomic_load(&w->done)) != gen)
		futex_wait(&w->done, v);
}

static void worker_stop(struct worker *w)
{
	if (!w->live)
		return;
	atomic_store(&w->req, -1);
	futex_wake(&w->req);
	pthread_join(w->th, NULL);
	w->live = 0;
}

/* --------------------------------------------------------------------- main */

static void *map_anon(size_t len)
{
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	return p == MAP_FAILED ? NULL : p;
}

static int pin_self(int cpu)
{
	cpu_set_t s;

	CPU_ZERO(&s);
	CPU_SET(cpu, &s);
	return sched_setaffinity(0, sizeof(s), &s);
}

/* do_pages_move() -> lru_cache_disable() -> __lru_add_drain_all(true) */
static void trigger(void *page)
{
	void *pages[1] = { page };
	int nodes[1] = { 0 }, status[1] = { 0 };
	long rc;

	rc = syscall(SYS_move_pages, 0, 1UL, pages, nodes, status, MPOL_MF_MOVE);
	if (rc < 0 && errno != ENOENT && verbose)
		fprintf(stderr, "move_pages: %s\n", strerror(errno));
}

struct ctx {
	int cpu, trigger_cpu, npages;
	struct worker dirty, drain;
	void *dirty_mem, *drain_page, *probe_page;
	int gen;
};

/* one trial; returns 1 if drain work was queued to the target CPU */
static int trial(struct ctx *c, int do_dirty, int do_drain, long sleep_ms)
{
	long entries;

	/* make the dirty region faultable again */
	if (do_dirty)
		madvise(c->dirty_mem, pagesz * (size_t)c->npages, MADV_DONTNEED);

	tf_put("tracing_on", "0\n", 0);
	tf_clear();
	tf_put("tracing_on", "1\n", 0);

	c->gen++;
	if (do_dirty)
		worker_run(&c->dirty, c->gen);
	if (do_drain)
		worker_run(&c->drain, c->gen);

	if (sleep_ms > 0) {
		struct timespec ts = {
			.tv_sec = sleep_ms / 1000,
			.tv_nsec = (sleep_ms % 1000) * 1000000L,
		};
		nanosleep(&ts, NULL);
	}

	trigger(c->probe_page);

	tf_put("tracing_on", "0\n", 0);
	entries = tf_entries(c->cpu);
	return entries > 0;
}

static void usage(void)
{
	fprintf(stderr,
"lru-verify - causal proof + re-dirty measurement for LP#2165410\n"
"\n"
"usage: lru-verify [options]\n"
"\n"
"  --cpu N            target CPU (default: highest CPU that is not --trigger-cpu)\n"
"  --trigger-cpu M    CPU that calls move_pages(2) (default 0)\n"
"  --trials K         trials per condition (default 20)\n"
"  --pages P          pages to fault when dirtying (default 4; keep below the\n"
"                     folio_batch capacity so the batch is not auto-drained)\n"
"  --decay            measure how long a drained CPU stays clean, instead of\n"
"                     comparing baseline against mitigated\n"
"  --decay-ms LIST    delays to sweep in decay mode (default 0,1,10,100,1000)\n"
"  --tracefs PATH     default /sys/kernel/tracing\n"
"  -v                 verbose\n"
"\n"
"Needs root. Installs a kprobe on lru_add_drain_per_cpu and removes it on exit.\n");
}

int main(int argc, char **argv)
{
	struct ctx c = { .cpu = -1, .trigger_cpu = 0, .npages = 4 };
	int trials = 20, decay = 0, rc = 0;
	long decay_ms[16] = { 0, 1, 10, 100, 1000 };
	int ndecay = 5;
	void *drain_stack;
	int ncpu;

	pagesz = (size_t)sysconf(_SC_PAGESIZE);
	ncpu = (int)sysconf(_SC_NPROCESSORS_CONF);

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--cpu") && i + 1 < argc)
			c.cpu = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--trigger-cpu") && i + 1 < argc)
			c.trigger_cpu = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--trials") && i + 1 < argc)
			trials = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--pages") && i + 1 < argc)
			c.npages = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--decay"))
			decay = 1;
		else if (!strcmp(argv[i], "--decay-ms") && i + 1 < argc) {
			char *s = argv[++i], *tok, *save;

			ndecay = 0;
			for (tok = strtok_r(s, ",", &save);
			     tok && ndecay < 16;
			     tok = strtok_r(NULL, ",", &save))
				decay_ms[ndecay++] = strtol(tok, NULL, 10);
		} else if (!strcmp(argv[i], "--tracefs") && i + 1 < argc)
			snprintf(tracefs, sizeof(tracefs), "%s", argv[++i]);
		else if (!strcmp(argv[i], "-v"))
			verbose = 1;
		else {
			usage();
			return 2;
		}
	}

	if (geteuid() != 0) {
		fprintf(stderr, "must run as root (needs %s)\n", tracefs);
		return 1;
	}
	if (c.cpu < 0) {
		c.cpu = ncpu - 1;
		if (c.cpu == c.trigger_cpu)
			c.cpu--;
	}
	if (c.cpu < 0 || c.cpu >= ncpu || c.cpu == c.trigger_cpu) {
		fprintf(stderr,
			"target and trigger CPU must differ and be < %d.\n"
			"  (__lru_add_drain_all() drains the calling CPU itself "
			"before queueing,\n   so a shared CPU would always look "
			"clean.)\n", ncpu);
		return 2;
	}

	/*
	 * MCL_CURRENT only. MCL_FUTURE would populate the dirty region at mmap
	 * time on this CPU, and then there would be nothing left to fault on
	 * the target CPU - the baseline would silently measure nothing.
	 */
	if (mlockall(MCL_CURRENT) != 0 && verbose)
		fprintf(stderr, "warning: mlockall: %s\n", strerror(errno));

	c.dirty_mem = map_anon(pagesz * (size_t)c.npages);
	c.drain_page = map_anon(pagesz);
	c.probe_page = map_anon(pagesz);
	drain_stack = map_anon(DRAIN_STACK);
	if (!c.dirty_mem || !c.drain_page || !c.probe_page || !drain_stack) {
		perror("mmap");
		return 1;
	}
	/* keep 4K folios so a THP does not collapse the batch to one entry */
	madvise(c.dirty_mem, pagesz * (size_t)c.npages, MADV_NOHUGEPAGE);
	*(volatile char *)c.probe_page = 1;

	/* pre-fault and pin the drain worker's stack, so that worker can never
	 * fault on the target CPU and re-dirty what it just drained */
	memset(drain_stack, 0, DRAIN_STACK);
	if (mlock(drain_stack, DRAIN_STACK) != 0 && verbose)
		fprintf(stderr, "warning: mlock(drain stack): %s\n",
			strerror(errno));

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);

	if (probe_setup() != 0) {
		probe_teardown();
		return 1;
	}

	if (pin_self(c.trigger_cpu) != 0)
		perror("pin trigger cpu");

	if (worker_start(&c.dirty, W_DIRTY, c.cpu, c.dirty_mem, c.npages,
			 NULL, 0) != 0 ||
	    worker_start(&c.drain, W_DRAIN, c.cpu, c.drain_page, 1,
			 drain_stack, DRAIN_STACK) != 0) {
		probe_teardown();
		return 1;
	}

	printf("target cpu=%d  trigger cpu=%d  trials=%d\n\n",
	       c.cpu, c.trigger_cpu, trials);

	if (decay) {
		printf("re-dirty rate: how often cpu%d is back in the flush set\n"
		       "after a drain, as a function of the delay before the trigger.\n\n",
		       c.cpu);
		for (int d = 0; d < ndecay; d++) {
			int hits = 0;

			for (int t = 0; t < trials; t++)
				hits += trial(&c, 0, 1, decay_ms[d]);
			printf("  after %5ld ms   re-dirtied in %2d/%d trials  (%3d%%)\n",
			       decay_ms[d], hits, trials, hits * 100 / trials);
		}
		printf("\nA rate that stays near 0%% as the delay grows means the drained\n"
		       "state is stable and `lru-isolate guard' is holding it rather than\n"
		       "racing. A rate that climbs means a refill source is still live -\n"
		       "see \"Operating model\" in the README.\n");
	} else {
		int base = 0, mit = 0;

		for (int t = 0; t < trials; t++)
			base += trial(&c, 1, 0, 0);
		printf("  %-44s cpu%d queued drain work in %2d/%d trials\n",
		       "BASELINE  (no mitigation)", c.cpu, base, trials);

		for (int t = 0; t < trials; t++)
			mit += trial(&c, 1, 1, 0);
		printf("  %-44s cpu%d queued drain work in %2d/%d trials\n",
		       "MITIGATED (madvise drain)", c.cpu, mit, trials);

		printf("\n");
		if (base == 0) {
			printf("INCONCLUSIVE: baseline never queued work to cpu%d.\n"
			       "  Something drained it first. Retry, or raise --pages.\n",
			       c.cpu);
			rc = 2;
		} else if (mit <= base / 10) {
			printf("PASS: the drain removed cpu%d from the flush set (%d/%d -> %d/%d).\n",
			       c.cpu, base, trials, mit, trials);
			if (mit)
				printf("  (%d residual: something re-dirtied the batch before the\n"
				       "   trigger. Expected on a non-isolated CPU; run --decay.)\n",
				       mit);
		} else {
			printf("FAIL: cpu%d still received drain work in %d/%d trials.\n"
			       "  If cpu%d is a busy general-purpose CPU this may be re-dirtying;\n"
			       "  run --decay, or retest against an idle or isolated CPU.\n",
			       c.cpu, mit, trials, c.cpu);
			rc = 1;
		}
	}

	worker_stop(&c.dirty);
	worker_stop(&c.drain);
	probe_teardown();
	return rc;
}

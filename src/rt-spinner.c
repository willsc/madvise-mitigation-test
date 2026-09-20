// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * rt-spinner - the SCHED_FIFO busy-poll load that LP#2165410 needs, built to
 * self-terminate.
 *
 * You cannot reproduce the bug without a FIFO thread saturating an isolated
 * CPU, and a hand-rolled one is how people wedge a machine: with
 * CONFIG_RT_GROUP_SCHED=n (the case on linux-aws 7.0 AND on 6.17 generic) there
 * is no RT bandwidth throttling, so an unbounded FIFO spinner starves the
 * per-CPU kworker forever and nothing short of a power cycle gets it back.
 *
 * So: --seconds is mandatory and capped, each thread holds its own
 * CLOCK_MONOTONIC deadline and checks it inside the spin loop (no reliance on
 * signal delivery or on the main thread getting scheduled), priority above 90
 * needs --force-prio, and it refuses to occupy every online CPU.
 *
 * WHY --clean MATTERS
 * -------------------
 * Two things must both be true for the stall:
 *   1. a FIFO thread saturates CPU N, so the per-CPU kworker cannot run;
 *   2. cpu_needs_drain(N) is true, so __lru_add_drain_all() queues work there
 *      at all.
 *
 * By default this tool faults its pages ON the pinned CPU at startup, which is
 * what a real application does and which satisfies (2) - the batch is dirtied
 * once and then never drained, because nothing else ever runs there.
 *
 * With --clean it pre-faults and mlocks from the housekeeping CPU before
 * pinning, so the isolated CPU's batches stay empty and (2) is false. That is
 * the "eliminate the source" half of the mitigation, and a --clean spinner
 * should NOT stall a cpuset write even with lru-isolate switched off. Running
 * it both ways is the cleanest demonstration that the batch state - not the
 * FIFO saturation on its own - is what decides.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MAX_SECONDS 600
#define DIRTY_PAGES 4

static size_t pagesz;

struct spinner {
	int cpu;
	int prio;
	int clean;
	void *mem;
	struct timespec deadline;
	pthread_t th;
	int live;
	int ran_on;
};

static int expired(const struct timespec *deadline)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	if (now.tv_sec != deadline->tv_sec)
		return now.tv_sec > deadline->tv_sec;
	return now.tv_nsec >= deadline->tv_nsec;
}

static void *spin_fn(void *arg)
{
	struct spinner *s = arg;
	volatile unsigned long sink = 0;

	s->ran_on = sched_getcpu();

	if (!s->clean) {
		/*
		 * Fault our pages in here, on the isolated CPU, so
		 * folio_add_lru_vma() leaves them in THIS CPU's lru_add batch.
		 * Nothing will ever drain it again - that is the bug.
		 */
		for (int i = 0; i < DIRTY_PAGES; i++)
			((volatile char *)s->mem)[(size_t)i * pagesz] = 1;
	}

	while (!expired(&s->deadline))
		for (int i = 0; i < 20000; i++)
			sink += (unsigned long)i;
	(void)sink;
	return NULL;
}

static int parse_cpulist(const char *str, char *mask, int ncpu, int *count)
{
	const char *p = str;

	*count = 0;
	while (*p) {
		char *end;
		long lo, hi;

		while (*p == ' ' || *p == ',')
			p++;
		if (!*p)
			break;
		lo = strtol(p, &end, 10);
		if (end == p)
			return -1;
		p = end;
		hi = lo;
		if (*p == '-') {
			p++;
			hi = strtol(p, &end, 10);
			if (end == p)
				return -1;
			p = end;
		}
		if (lo < 0 || hi < lo || hi >= ncpu)
			return -1;
		for (long c = lo; c <= hi; c++)
			if (!mask[c]) {
				mask[c] = 1;
				(*count)++;
			}
	}
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
"rt-spinner - self-terminating SCHED_FIFO load for reproducing LP#2165410\n"
"\n"
"usage: rt-spinner --cpus LIST --seconds N [--prio P] [--clean] [--force-prio]\n"
"\n"
"  --cpus LIST     CPUs to saturate, e.g. 2-3 or 18-95,108-178\n"
"  --seconds N     REQUIRED lifetime, 1..%d. Each thread holds its own\n"
"                  CLOCK_MONOTONIC deadline, so it exits even if nothing else\n"
"                  on the box can be scheduled.\n"
"  --prio P        SCHED_FIFO priority (default 80). Above 90 needs\n"
"                  --force-prio: migration/N and the per-CPU RCU kthreads sit\n"
"                  at 99, and preempting those can wedge the machine.\n"
"  --clean         pre-fault and mlock from the housekeeping CPU before\n"
"                  pinning, so the isolated CPU's LRU batches stay empty.\n"
"                  Use this to show that FIFO saturation alone does not stall\n"
"                  a cpuset write - the dirty batch is what does.\n"
"\n"
"WARNING: with CONFIG_RT_GROUP_SCHED=n there is no RT throttling. While this\n"
"runs, the per-CPU kworkers on the named CPUs cannot run at all. Keep --seconds\n"
"short, and do not run it on a machine you cannot afford to lose.\n", MAX_SECONDS);
}

int main(int argc, char **argv)
{
	char *mask;
	int ncpu, count = 0, seconds = 0, prio = 80, clean = 0, force = 0;
	int have_cpus = 0, started = 0;
	struct spinner *sp;
	struct timespec deadline;
	void *region;

	pagesz = (size_t)sysconf(_SC_PAGESIZE);
	ncpu = (int)sysconf(_SC_NPROCESSORS_CONF);
	mask = calloc((size_t)ncpu, 1);
	if (!mask)
		return 1;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--cpus") && i + 1 < argc) {
			if (parse_cpulist(argv[++i], mask, ncpu, &count) != 0) {
				fprintf(stderr, "bad cpu list: %s\n", argv[i]);
				return 2;
			}
			have_cpus = 1;
		} else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
			seconds = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--prio") && i + 1 < argc)
			prio = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--clean"))
			clean = 1;
		else if (!strcmp(argv[i], "--force-prio"))
			force = 1;
		else {
			usage();
			return 2;
		}
	}

	if (!have_cpus || !count) {
		fprintf(stderr, "--cpus is required\n\n");
		usage();
		return 2;
	}
	if (seconds < 1 || seconds > MAX_SECONDS) {
		fprintf(stderr,
			"--seconds is required and must be 1..%d.\n"
			"There is no RT throttling to save you; an unbounded FIFO\n"
			"spinner starves the per-CPU kworker until power cycle.\n",
			MAX_SECONDS);
		return 2;
	}
	if (count >= ncpu) {
		fprintf(stderr,
			"refusing to saturate all %d CPUs - leave housekeeping CPUs free\n",
			ncpu);
		return 2;
	}
	if (prio < 1 || prio > 99) {
		fprintf(stderr, "--prio must be 1..99\n");
		return 2;
	}
	if (prio > 90 && !force) {
		fprintf(stderr,
			"--prio %d needs --force-prio: migration/N and the per-CPU RCU\n"
			"kthreads run at SCHED_FIFO 99 and preempting them can wedge\n"
			"the machine.\n", prio);
		return 2;
	}

	region = mmap(NULL, pagesz * DIRTY_PAGES * (size_t)count,
		      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (region == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	sp = calloc((size_t)count, sizeof(*sp));
	if (!sp)
		return 1;

	clock_gettime(CLOCK_MONOTONIC, &deadline);
	deadline.tv_sec += seconds;

	if (clean) {
		/* populate and pin here, on the housekeeping CPU, so the
		 * isolated CPUs never fault and their batches stay empty */
		memset(region, 0, pagesz * DIRTY_PAGES * (size_t)count);
		if (mlock(region, pagesz * DIRTY_PAGES * (size_t)count) != 0)
			fprintf(stderr, "warning: mlock: %s\n", strerror(errno));
	}

	printf("rt-spinner: %d cpu(s), SCHED_FIFO/%d, %d s, %s\n",
	       count, prio, seconds,
	       clean ? "clean (batches left empty)" : "dirtying batches on-cpu");
	fflush(stdout);

	int i = 0;
	for (int c = 0; c < ncpu; c++) {
		pthread_attr_t attr;
		struct sched_param par = { .sched_priority = prio };
		cpu_set_t set;
		int rc;

		if (!mask[c])
			continue;
		sp[i].cpu = c;
		sp[i].prio = prio;
		sp[i].clean = clean;
		sp[i].deadline = deadline;
		sp[i].mem = (char *)region + (size_t)i * pagesz * DIRTY_PAGES;
		sp[i].ran_on = -1;

		pthread_attr_init(&attr);
		CPU_ZERO(&set);
		CPU_SET(c, &set);
		pthread_attr_setaffinity_np(&attr, sizeof(set), &set);
		pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
		pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
		pthread_attr_setschedparam(&attr, &par);
		rc = pthread_create(&sp[i].th, &attr, spin_fn, &sp[i]);
		pthread_attr_destroy(&attr);

		if (rc) {
			fprintf(stderr, "cpu%d: pthread_create: %s%s\n", c,
				strerror(rc),
				rc == EPERM ? " (need CAP_SYS_NICE)" : "");
		} else {
			sp[i].live = 1;
			started++;
		}
		i++;
	}

	if (!started) {
		fprintf(stderr, "no spinners started\n");
		return 1;
	}

	for (int k = 0; k < count; k++)
		if (sp[k].live)
			pthread_join(sp[k].th, NULL);

	printf("rt-spinner: all %d spinner(s) exited on deadline\n", started);
	return 0;
}

// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * lru-isolate - userspace mitigation for Ubuntu LP#2165410
 *
 *   "systemd D-state during cpuset migration on nohz_full CPUs"
 *
 * WHAT THIS FIXES
 * ---------------
 * A cpuset.mems write (e.g. systemd AllowedMemoryNodes=) reaches:
 *
 *     cpuset_migrate_mm() -> do_migrate_pages() -> lru_cache_disable()
 *         -> synchronize_rcu_expedited()          [cause 1 - NOT fixed here]
 *         -> __lru_add_drain_all(true)            [cause 2 - race reduced]
 *
 * __lru_add_drain_all() queues a per-CPU work item to every CPU for which
 * cpu_needs_drain(cpu) is true, then flush_work()s each one.  On a CPU that
 * is saturated by a SCHED_FIFO busy-poll thread the per-CPU kworker never
 * runs, so flush_work() blocks - and PID 1 sits in D state.
 *
 * THE WRITE IS NOT WHERE THE MIGRATION HAPPENS
 * --------------------------------------------
 * do_migrate_pages() does NOT run in the context of the cpuset.mems write.
 * update_tasks_nodemask() walks the cpuset and calls cpuset_migrate_mm() once
 * per task; each call queues a work item. The writer then flushes that queue
 * from task_work before returning to userspace:
 *
 *     kernel/cgroup/cpuset.c:2641  while ((task = css_task_iter_next(&it)))
 *     kernel/cgroup/cpuset.c:2556      queue_work(cpuset_migrate_mm_wq, ...)
 *     kernel/cgroup/cpuset.c:4010  alloc_ordered_workqueue("cpuset_migrate_mm", 0)
 *
 * That workqueue is *ordered* - max_active=1 - so N tasks in the cpuset means
 * N lru_cache_disable() calls run strictly serially on an unbound kworker,
 * for as long as the migration takes (the bug reports 60 s - 10 min).  Each
 * one re-reads cpu_needs_drain() for every CPU.
 *
 * So a drain that happens once, before the write, covers none of them.  `arm'
 * therefore forks the command and keeps draining until the migration has
 * drained out - see cmd_arm(). A one-shot drain cannot cover all the later
 * queueing decisions, whether or not the command has returned.
 *
 * HOW WELL THIS SCALES - READ BEFORE DEPLOYING
 * --------------------------------------------
 * The css_task_iter_start() above takes flags = 0.  CSS_TASK_ITER_PROCS is
 * "walk only threadgroup leaders" (include/linux/cgroup.h:46) and is NOT set,
 * so the walk covers every *thread* and queues a work item for each, even
 * though threads share an mm.  N threads in the cpuset means N serialized
 * lru_cache_disable() calls, each re-reading cpu_needs_drain().
 *
 * More calls provide more opportunities to encounter a dirty CPU, but those
 * observations need not be independent. Desktop verification results cannot
 * predict the failure probability of the continuously refaulting reproducer.
 * Once work has been queued, madvise does not complete that work item: the
 * kernel worker still has to run. See the README for the residual race.
 *
 * Verified against linux-aws 7.0 (mm/swap.c):
 *
 *     for_each_online_cpu(cpu) {
 *             if (cpu_needs_drain(cpu)) {
 *                     queue_work_on(cpu, mm_percpu_wq, work);
 *                     __cpumask_set_cpu(cpu, &has_work);
 *             }
 *     }
 *     for_each_cpu(cpu, &has_work)
 *             flush_work(&per_cpu(lru_add_drain_work, cpu));
 *
 * Note that `force_all_cpus' does NOT appear in the queueing condition - it
 * only forces the drain *pass* past the generation check.  So a CPU whose
 * per-CPU batches are empty is never sent work and is never flushed.
 *
 * THE LEVER
 * ---------
 * cpu_needs_drain(cpu) tests six folio batches + need_mlock_drain(cpu) +
 * has_bh_in_lru(cpu).  lru_add_drain() clears all of those except the
 * buffer-head LRU, and it is reachable from userspace:
 *
 *     mm/madvise.c:madvise_willneed()
 *         if (!file) {
 *                 walk_page_range_vma(vma, start, end, &swapin_walk_ops, vma);
 *                 lru_add_drain();        <-- local CPU drain
 *                 return 0;
 *         }
 *
 * madvise(MADV_WILLNEED) on a *never-touched anonymous* page, issued by a
 * thread pinned to CPU N, empties CPU N's batches.  MADV_WILLNEED is not gated
 * by can_madv_lru_vma(), so unlike MADV_COLD/MADV_PAGEOUT it still works under
 * mlockall(), which RT daemons routinely call.
 *
 * ISOLATED-CORE DISCIPLINE
 * ------------------------
 * The tool runs *on* latency-critical cores, so it must not perturb them
 * beyond the drain itself, and it must not dirty the batch it came to empty.
 *
 *  - Drain threads are created ONCE and parked on a futex.  A pass wakes them
 *    and they go straight back to sleep.  No clone()/exit() per pass on an
 *    isolated core - just two context switches and one madvise().
 *
 *  - mlockall(MCL_CURRENT|MCL_FUTURE) runs BEFORE any drain thread is created,
 *    so glibc's thread stacks are populated at mmap time on the housekeeping
 *    CPU that calls pthread_create().  Without this the new thread's first
 *    stack write faults on the isolated CPU, and handle_mm_fault() ->
 *    folio_add_lru_vma() adds to the very lru_add batch we are here to drain.
 *    Anything faulting *after* the madvise would re-dirty it outright.
 *
 *  - The main loop is pinned to housekeeping CPUs (the complement of the
 *    target set), so only the pinned drain threads ever touch an isolated CPU.
 *
 *  - Target CPUs are validated against this process's own affinity mask first,
 *    so a cpuset/AllowedCPUs= restriction is reported as such instead of
 *    surfacing as a bare EINVAL from pthread_create().
 *
 *  - cpu_set_t is allocated with CPU_ALLOC.  The fixed cpu_set_t holds 1024
 *    CPUs and CPU_SET() past that corrupts memory; the bug's own hardware is
 *    192 vCPUs but the CPU list is user-supplied.
 *
 * WHAT THIS DOES NOT FIX
 * ----------------------
 * lru_cache_disable() calls synchronize_rcu_expedited() *before* the drain,
 * and do_migrate_pages() calls lru_cache_disable() unconditionally.  No
 * madvise() can avoid that.  Cause 1 needs one of:
 *   - rcupdate.rcu_normal=1        (boot param; module_param is 0444, so a
 *                                   reboot is required - not runtime writable)
 *   - isolcpus=domain,nohz,<list>  (upstream's supported configuration)
 *   - the kthread-affinity kernel patch from the bug
 * `lru-isolate check' reports which of these you are missing.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef BLKFLSBUF
#define BLKFLSBUF _IO(0x12, 97)
#endif

#define PF_KTHREAD 0x00200000

static int verbose;
static int g_ncpu;		/* configured CPU count */
static size_t g_setsz;		/* CPU_ALLOC_SIZE(g_ncpu) */

static void vrb(const char *fmt, ...)
{
	va_list ap;

	if (!verbose)
		return;
	va_start(ap, fmt);
	fprintf(stderr, "[lru-isolate] ");
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static double delta_us(struct timespec a, struct timespec b)
{
	return (b.tv_sec - a.tv_sec) * 1e6 + (b.tv_nsec - a.tv_nsec) / 1e3;
}

static int futex_wait(atomic_int *addr, int val)
{
	return (int)syscall(SYS_futex, (void *)addr, FUTEX_WAIT_PRIVATE, val,
			    NULL, NULL, 0);
}

static int futex_wake(atomic_int *addr, int n)
{
	return (int)syscall(SYS_futex, (void *)addr, FUTEX_WAKE_PRIVATE, n,
			    NULL, NULL, 0);
}

/* ------------------------------------------------------------------ cpulist */

static int parse_cpulist(const char *s, char *mask)
{
	const char *p = s;

	while (*p) {
		char *end;
		long lo, hi;

		while (*p == ' ' || *p == ',' || *p == '\n')
			p++;
		if (!*p)
			break;
		if (!isdigit((unsigned char)*p))
			return -1;
		lo = strtol(p, &end, 10);
		p = end;
		hi = lo;
		if (*p == '-') {
			p++;
			hi = strtol(p, &end, 10);
			p = end;
		}
		if (lo < 0 || hi < lo)
			return -1;
		if (hi >= g_ncpu) {
			fprintf(stderr,
				"cpu %ld is beyond the %d configured CPUs\n",
				hi, g_ncpu);
			return -1;
		}
		for (long c = lo; c <= hi; c++)
			mask[c] = 1;
	}
	return 0;
}

static int read_first_line(const char *path, char *buf, size_t n)
{
	FILE *f = fopen(path, "r");

	if (!f)
		return -1;
	if (!fgets(buf, (int)n, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	buf[strcspn(buf, "\n")] = 0;
	return 0;
}

/* Union of nohz_full and isolated, as the kernel reports them. */
static int detect_isolated(char *mask, char *why, size_t whyn)
{
	static const char *paths[] = {
		"/sys/devices/system/cpu/nohz_full",
		"/sys/devices/system/cpu/isolated",
	};
	char buf[4096];
	int found = 0;

	why[0] = 0;
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		if (read_first_line(paths[i], buf, sizeof(buf)) != 0)
			continue;
		if (!buf[0] || !strcmp(buf, "(null)"))
			continue;
		if (parse_cpulist(buf, mask) == 0) {
			size_t used = strlen(why);

			found = 1;
			snprintf(why + used, whyn - used, "%s%s=%.120s",
				 why[0] ? ", " : "",
				 strrchr(paths[i], '/') + 1, buf);
		}
	}
	return found ? 0 : -1;
}

static int count_mask(const char *mask)
{
	int n = 0;

	for (int c = 0; c < g_ncpu; c++)
		if (mask[c])
			n++;
	return n;
}

/* ---------------------------------------------------------------- rt probing */

/*
 * Highest SCHED_FIFO/RR priority of any *userspace* task per CPU.
 *
 * /proc/<pid>/stat fields: 9 = flags, 39 = processor, 40 = rt_priority,
 * 41 = policy.  comm (field 2) may contain spaces and parens, so scan from
 * the last ')'.
 *
 * Things that matter here:
 *  - iterate /proc/<pid>/task/<tid>, not just <pid>.  A busy-poll spinner is
 *    usually one thread of a larger process, and the thread-group leader is
 *    typically not the pinned RT thread.
 *  - skip kernel threads (PF_KTHREAD).  migration/N and the per-CPU RCU
 *    kthreads sit at SCHED_FIFO 99 on every CPU, and treating them as the
 *    thing to preempt would push every drain to priority 99 for no reason.
 *  - skip our own process during rescans; use each task's allowed CPUs, since
 *    its last reported CPU does not constrain where it can run next.
 */
static void scan_rt_prios(int *percpu_max)
{
	DIR *proc;
	struct dirent *pe;
	cpu_set_t *affinity = CPU_ALLOC((size_t)g_ncpu);

	for (int c = 0; c < g_ncpu; c++)
		percpu_max[c] = 0;

	proc = opendir("/proc");
	if (!proc) {
		CPU_FREE(affinity);
		return;
	}

	while ((pe = readdir(proc))) {
		char tdir[320];
		DIR *tasks;
		struct dirent *te;

		if (!isdigit((unsigned char)pe->d_name[0]))
			continue;
		/* A rescan must not count our own drainers and boost itself. */
		if (strtol(pe->d_name, NULL, 10) == (long)getpid())
			continue;
		snprintf(tdir, sizeof(tdir), "/proc/%s/task", pe->d_name);
		tasks = opendir(tdir);
		if (!tasks)
			continue;

		while ((te = readdir(tasks))) {
			char path[768], buf[1024], *rp, *tok, *save;
			int fd, n, f;
			long flags = 0;
			int processor = -1, rtprio = 0, policy = 0;

			if (!isdigit((unsigned char)te->d_name[0]))
				continue;
			snprintf(path, sizeof(path), "%s/%s/stat", tdir,
				 te->d_name);
			fd = open(path, O_RDONLY);
			if (fd < 0)
				continue;
			n = (int)read(fd, buf, sizeof(buf) - 1);
			close(fd);
			if (n <= 0)
				continue;
			buf[n] = 0;
			rp = strrchr(buf, ')');
			if (!rp || !rp[1])
				continue;

			f = 3;	/* rp + 2 is field 3 (state) */
			for (tok = strtok_r(rp + 2, " ", &save); tok;
			     tok = strtok_r(NULL, " ", &save), f++) {
				if (f == 9)
					flags = strtol(tok, NULL, 10);
				else if (f == 39)
					processor = atoi(tok);
				else if (f == 40)
					rtprio = atoi(tok);
				else if (f == 41) {
					policy = atoi(tok);
					break;
				}
			}
			if (flags & PF_KTHREAD)
				continue;
			if (policy != SCHED_FIFO && policy != SCHED_RR)
				continue;
			/* A runnable RT task can move after the last-CPU sample. */
			if (affinity && sched_getaffinity((pid_t)strtol(te->d_name,
					NULL, 10), g_setsz, affinity) == 0) {
				for (int c = 0; c < g_ncpu; c++)
					if (CPU_ISSET_S((size_t)c, g_setsz, affinity) &&
					    rtprio > percpu_max[c])
						percpu_max[c] = rtprio;
			} else if (processor >= 0 && processor < g_ncpu &&
				   rtprio > percpu_max[processor]) {
				percpu_max[processor] = rtprio;
			}
		}
		closedir(tasks);
	}
	closedir(proc);
	CPU_FREE(affinity);
}

/* ------------------------------------------------------------------- drainers */

/*
 * One persistent thread per target CPU, parked on a futex between passes.
 *
 * Created once, so the clone()/exit() cost and the stack page faults land on
 * whoever calls drainers_start() - which is the main loop, pinned to a
 * housekeeping CPU - and never on the isolated core.
 */
struct drainer {
	int cpu;
	int rtprio;
	pthread_t th;
	int live;		/* thread was created */

	atomic_int req;		/* pass generation requested */
	atomic_int done;	/* pass generation completed */
	atomic_int ready;	/* thread reached its CPU */

	void *scratch;
	size_t pagesz;

	struct timespec t_req;	/* written by main before bumping req */
	double sched_us;	/* dispatch latency on the isolated core */
	double drain_us;	/* madvise() duration */
	int err;
	int ran_on;
};

static struct drainer *g_dr;
static int g_ndr;
static int g_gen;
static void *g_region;
static size_t g_region_len;
static int *g_rtmax;

/* Controller waits are bounded even when a later FIFO task starves a drainer.
 * Keep the workers' idle waits unbounded: they should sleep between passes. */
#define DRAIN_RECHECK_MS 100
#define DRAIN_TIMEOUT_MS 5000

static int refresh_rt_prios(void)
{
	if (!g_rtmax)
		return 0; /* explicit priority: diagnose a timeout, do not override it */
	scan_rt_prios(g_rtmax);
	for (int k = 0; k < g_ndr; k++) {
		struct drainer *d = &g_dr[k];
		int m = g_rtmax[d->cpu], rc;
		struct sched_param sp = { .sched_priority = m + 1 };

		if (!d->live || !m)
			continue;
		if (m >= 99) {
			fprintf(stderr, "cpu%d: userspace RT priority 99 leaves no "
				"higher FIFO priority for the drainer\n", d->cpu);
			return -1;
		}
		if (sp.sched_priority <= d->rtprio)
			continue;
		rc = pthread_setschedparam(d->th, SCHED_FIFO, &sp);
		if (rc) {
			fprintf(stderr, "cpu%d: cannot raise drainer to FIFO/%d: %s "
				"(CAP_SYS_NICE required)\n", d->cpu,
				sp.sched_priority, strerror(rc));
			return -1;
		}
		fprintf(stderr, "cpu%d: raising drainer priority %d -> FIFO/%d "
			"after a delayed dispatch\n", d->cpu, d->rtprio,
			sp.sched_priority);
		d->rtprio = sp.sched_priority;
	}
	return 0;
}

static int wait_for_drainer(struct drainer *d, atomic_int *counter, int wanted,
			    const char *phase)
{
	struct timespec start, now;
	const struct timespec poll = { .tv_nsec = DRAIN_RECHECK_MS * 1000000L };
	int v;

	clock_gettime(CLOCK_MONOTONIC, &start);
	while ((v = atomic_load(counter)) != wanted) {
		syscall(SYS_futex, (void *)counter, FUTEX_WAIT_PRIVATE, v,
			&poll, NULL, 0);
		if (atomic_load(counter) == wanted)
			return 0;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (delta_us(start, now) / 1000.0 >= DRAIN_TIMEOUT_MS) {
			fprintf(stderr, "cpu%d: drainer %s timed out after %d ms "
				"(priority %d); mitigation is not progressing. "
				"Check RT priorities, affinity and blocked-task stacks.\n",
				d->cpu, phase, DRAIN_TIMEOUT_MS, d->rtprio);
			return -1;
		}
		if (refresh_rt_prios() != 0)
			return -1;
	}
	return 0;
}

static void *drain_loop(void *arg)
{
	struct drainer *d = arg;
	int seen = 0;

	d->ran_on = sched_getcpu();
	atomic_store(&d->ready, 1);
	futex_wake(&d->ready, 1);

	for (;;) {
		struct timespec t1, t2;
		int req;

		for (;;) {
			req = atomic_load(&d->req);
			if (req != seen)
				break;
			futex_wait(&d->req, seen);
		}
		if (req < 0)
			break;			/* shutdown */
		seen = req;

		clock_gettime(CLOCK_MONOTONIC, &t1);
		/*
		 * Untouched (or already-resident, under MCL_FUTURE) anonymous
		 * page: the walk moves no pages, then lru_add_drain() runs on
		 * this CPU.  Only mmap_read_lock is taken, so every target CPU
		 * can do this in the same instant.
		 */
		if (madvise(d->scratch, d->pagesz, MADV_WILLNEED) != 0)
			d->err = errno;
		clock_gettime(CLOCK_MONOTONIC, &t2);

		d->sched_us = delta_us(d->t_req, t1);
		d->drain_us = delta_us(t1, t2);

		atomic_store(&d->done, seen);
		futex_wake(&d->done, 1);
	}
	return NULL;
}

/* Is `cpu' reachable at all from this process? */
static int cpu_permitted(int cpu, cpu_set_t *allowed)
{
	return CPU_ISSET_S((size_t)cpu, g_setsz, allowed);
}

/*
 * Pin the caller to the housekeeping CPUs - every CPU this process may use
 * that is NOT a drain target - so the main loop never runs on an isolated
 * core.  Best effort: if the intersection is empty we leave affinity alone
 * and say so.
 */
static void pin_to_housekeeping(const char *mask, cpu_set_t *allowed)
{
	cpu_set_t *hk = CPU_ALLOC((size_t)g_ncpu);
	int n = 0;

	if (!hk)
		return;
	CPU_ZERO_S(g_setsz, hk);
	for (int c = 0; c < g_ncpu; c++) {
		if (mask[c])
			continue;
		if (!CPU_ISSET_S((size_t)c, g_setsz, allowed))
			continue;
		CPU_SET_S((size_t)c, g_setsz, hk);
		n++;
	}
	if (!n) {
		fprintf(stderr,
			"warning: no housekeeping CPU available; the main loop "
			"will share the isolated cores\n");
	} else if (sched_setaffinity(0, g_setsz, hk) != 0) {
		fprintf(stderr, "warning: cannot pin to housekeeping CPUs: %s\n",
			strerror(errno));
	} else {
		vrb("main loop pinned to %d housekeeping CPU(s)\n", n);
	}
	CPU_FREE(hk);
}

static int drainers_start(const char *mask, int rtprio_opt)
{
	cpu_set_t *allowed, *one;
	size_t pagesz = (size_t)sysconf(_SC_PAGESIZE);
	int n = count_mask(mask), i = 0, unreachable = 0;

	if (!n) {
		fprintf(stderr, "no target CPUs\n");
		return -1;
	}

	allowed = CPU_ALLOC((size_t)g_ncpu);
	one = CPU_ALLOC((size_t)g_ncpu);
	if (!allowed || !one) {
		fprintf(stderr, "CPU_ALLOC failed\n");
		return -1;
	}
	CPU_ZERO_S(g_setsz, allowed);
	if (sched_getaffinity(0, g_setsz, allowed) != 0) {
		perror("sched_getaffinity");
		return -1;
	}

	/*
	 * Target CPUs missing from our affinity mask come from one of two
	 * places, and only one of them is fatal:
	 *
	 *   CPUAffinity= (in the unit, in /etc/systemd/system.conf, or a
	 *   taskset wrapper) is a plain sched_setaffinity() restriction.  It
	 *   is not a ceiling - a task can widen its own mask straight back
	 *   out of it, no capability required.  On a tuned low-latency host a
	 *   manager-wide CPUAffinity= in system.conf is normal, and it is
	 *   inherited by every unit including this one.
	 *
	 *   AllowedCPUs= is a cpuset, which IS a hard ceiling.
	 *
	 * Try to widen, then look again to see which one we are in.  Without
	 * this, the guard unit on an isolated box fails at startup with every
	 * target CPU unreachable - the isolation that makes the mitigation
	 * necessary is the same thing that locked it out.
	 */
	for (int c = 0; c < g_ncpu; c++)
		if (mask[c] && !cpu_permitted(c, allowed))
			unreachable++;

	if (unreachable) {
		cpu_set_t *wide = CPU_ALLOC((size_t)g_ncpu);

		if (wide) {
			CPU_ZERO_S(g_setsz, wide);
			for (int c = 0; c < g_ncpu; c++)
				if (mask[c] || cpu_permitted(c, allowed))
					CPU_SET_S((size_t)c, g_setsz, wide);

			if (sched_setaffinity(0, g_setsz, wide) == 0 &&
			    sched_getaffinity(0, g_setsz, allowed) == 0) {
				int still = 0;

				for (int c = 0; c < g_ncpu; c++)
					if (mask[c] && !cpu_permitted(c, allowed))
						still++;
				if (!still)
					vrb("widened affinity mask to reach %d "
					    "target CPU(s) held off by a "
					    "CPUAffinity=/taskset restriction\n",
					    unreachable);
				unreachable = still;
			}
			CPU_FREE(wide);
		}
	}

	if (unreachable) {
		for (int c = 0; c < g_ncpu; c++)
			if (mask[c] && !cpu_permitted(c, allowed))
				fprintf(stderr,
					"error: cpu%d is outside this process's "
					"affinity mask\n", c);
		fprintf(stderr,
			"       %d target CPU(s) unreachable, and widening the "
			"mask did not help - so this is a cpuset ceiling, not a\n"
			"       CPUAffinity= setting. Check AllowedCPUs= on this "
			"unit and on its parent slice.\n",
			unreachable);
		CPU_FREE(allowed);
		CPU_FREE(one);
		return -1;
	}

	pin_to_housekeeping(mask, allowed);

	/*
	 * Lock and populate everything BEFORE creating the drain threads, so
	 * their stacks are faulted in here (on a housekeeping CPU) rather than
	 * on the isolated core on first touch.  See ISOLATED-CORE DISCIPLINE.
	 */
	if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
		fprintf(stderr,
			"warning: mlockall: %s - drain threads may page-fault "
			"on the isolated cores\n", strerror(errno));

	g_region_len = pagesz * (size_t)n;
	g_region = mmap(NULL, g_region_len, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (g_region == MAP_FAILED) {
		perror("mmap scratch");
		CPU_FREE(allowed);
		CPU_FREE(one);
		return -1;
	}

	g_dr = calloc((size_t)n, sizeof(*g_dr));
	if (!g_dr) {
		fprintf(stderr, "out of memory\n");
		return -1;
	}
	g_ndr = n;

	if (rtprio_opt < 0) {
		g_rtmax = calloc((size_t)g_ncpu, sizeof(int));
		if (!g_rtmax) {
			fprintf(stderr, "out of memory scanning RT priorities\n");
			return -1;
		}
		scan_rt_prios(g_rtmax);
	}

	for (int c = 0; c < g_ncpu; c++) {
		struct drainer *d;
		pthread_attr_t attr;
		int rc;

		if (!mask[c])
			continue;
		d = &g_dr[i];
		d->cpu = c;
		d->ran_on = -1;
		d->scratch = (char *)g_region + (size_t)i * pagesz;
		d->pagesz = pagesz;
		atomic_init(&d->req, 0);
		atomic_init(&d->done, 0);
		atomic_init(&d->ready, 0);

		d->rtprio = rtprio_opt;
		if (rtprio_opt < 0) {
			int m = g_rtmax[c];

			if (m >= 99) {
				fprintf(stderr, "cpu%d: userspace RT priority 99 leaves "
					"no higher FIFO priority for the drainer\n", c);
				return -1;
			}
			d->rtprio = m ? m + 1 : 0;
			vrb("cpu%d: highest userspace RT prio = %d, drain prio = %d\n",
			    c, m, d->rtprio);
		}

		CPU_ZERO_S(g_setsz, one);
		CPU_SET_S((size_t)c, g_setsz, one);

		pthread_attr_init(&attr);
		pthread_attr_setaffinity_np(&attr, g_setsz, one);
		if (d->rtprio > 0) {
			struct sched_param sp = { .sched_priority = d->rtprio };

			pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
			pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
			pthread_attr_setschedparam(&attr, &sp);
		}
		rc = pthread_create(&d->th, &attr, drain_loop, d);
		pthread_attr_destroy(&attr);

		if (rc) {
			fprintf(stderr, "cpu%d: pthread_create: %s\n", c,
				strerror(rc));
			d->err = rc;
			return -1;
		} else {
			d->live = 1;
		}
		i++;
	}
	CPU_FREE(allowed);
	CPU_FREE(one);

	/* wait for each thread to land on its CPU before the first pass */
	for (int k = 0; k < g_ndr; k++) {
		struct drainer *d = &g_dr[k];

		if (!d->live)
			continue;
		if (wait_for_drainer(d, &d->ready, 1, "startup") != 0)
			return -1;
		if (d->ran_on != d->cpu)
			fprintf(stderr,
				"warning: cpu%d drainer landed on cpu%d - "
				"affinity not honoured\n", d->cpu, d->ran_on);
	}
	return 0;
}

static void drainers_stop(void)
{
	for (int k = 0; k < g_ndr; k++) {
		struct drainer *d = &g_dr[k];

		if (!d->live)
			continue;
		atomic_store(&d->req, -1);
		futex_wake(&d->req, 1);
		pthread_join(d->th, NULL);
	}
	free(g_dr);
	g_dr = NULL;
	g_ndr = 0;
	free(g_rtmax);
	g_rtmax = NULL;
	if (g_region)
		munmap(g_region, g_region_len);
	g_region = NULL;
}

/* has_bh_in_lru() is the one cpu_needs_drain() term lru_add_drain() misses.
 * invalidate_bh_lrus() is on_each_cpu_cond() - an IPI - so it reaches a
 * FIFO-saturated CPU without needing to be scheduled there. */
static void bh_flush(const char *dev)
{
	int fd = open(dev, O_RDONLY);

	if (fd < 0) {
		fprintf(stderr, "bh-flush: open %s: %s\n", dev, strerror(errno));
		return;
	}
	if (ioctl(fd, BLKFLSBUF, 0) != 0)
		fprintf(stderr, "bh-flush: BLKFLSBUF %s: %s\n", dev,
			strerror(errno));
	else
		vrb("bh_lru invalidated via BLKFLSBUF on %s\n", dev);
	close(fd);
}

/*
 * Is a cpuset memory migration still running?
 *
 * cpuset_migrate_mm_wq is unbound, so only a kworker/u* can be running
 * cpuset_migrate_mm_workfn().  Checking comm before reading the stack keeps
 * this from unwinding every kthread on a 192-CPU box.
 *
 * Returns 1 = in flight, 0 = idle, -1 = cannot tell (no CONFIG_STACKTRACE,
 * or not privileged enough to read /proc/<pid>/stack).
 */
static int migration_in_flight(void)
{
	DIR *proc = opendir("/proc");
	struct dirent *pe;
	int readable = 0, found = 0;

	if (!proc)
		return -1;

	while ((pe = readdir(proc))) {
		char path[320], buf[8192];
		int fd, n;

		if (!isdigit((unsigned char)pe->d_name[0]))
			continue;

		snprintf(path, sizeof(path), "/proc/%s/comm", pe->d_name);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		n = (int)read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n <= 0)
			continue;
		buf[n] = 0;
		if (strncmp(buf, "kworker/u", 9) != 0)
			continue;

		snprintf(path, sizeof(path), "/proc/%s/stack", pe->d_name);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		n = (int)read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (n <= 0)
			continue;
		buf[n] = 0;
		readable = 1;
		if (strstr(buf, "cpuset_migrate_mm_workfn")) {
			found = 1;
			break;
		}
	}
	closedir(proc);

	if (found)
		return 1;
	return readable ? 0 : -1;
}

struct opts {
	char *mask;
	int rtprio;		/* >0 fixed, 0 none, -1 auto */
	int serial;
	int json;
	int interval_ms;
	int settle_ms;		/* arm: cap on draining after the command exits */
	const char *bhdev;
};

static int drain_pass(struct opts *o, int report)
{
	struct timespec p0, p1;
	double worst_sched = 0, total = 0;
	int failed = 0, live = 0;

	g_gen++;

	clock_gettime(CLOCK_MONOTONIC, &p0);
	for (int k = 0; k < g_ndr; k++) {
		struct drainer *d = &g_dr[k];

		if (!d->live)
			continue;
		clock_gettime(CLOCK_MONOTONIC, &d->t_req);
		atomic_store(&d->req, g_gen);
		futex_wake(&d->req, 1);

		if (o->serial) {
			if (wait_for_drainer(d, &d->done, g_gen, "pass") != 0)
				return 1;
		}
	}
	if (!o->serial) {
		for (int k = 0; k < g_ndr; k++) {
			struct drainer *d = &g_dr[k];

			if (!d->live)
				continue;
			if (wait_for_drainer(d, &d->done, g_gen, "pass") != 0)
				return 1;
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &p1);

	if (o->bhdev)
		bh_flush(o->bhdev);

	for (int k = 0; k < g_ndr; k++) {
		struct drainer *d = &g_dr[k];

		if (!d->live) {
			failed++;
			continue;
		}
		live++;
		if (d->err) {
			failed++;
			fprintf(stderr, "cpu%d: madvise drain failed: %s\n",
				d->cpu, strerror(d->err));
		}
		if (d->sched_us > worst_sched)
			worst_sched = d->sched_us;
		total += d->drain_us;
	}

	if (!report)
		return failed ? 1 : 0;

	if (o->json) {
		printf("{\"cpus\":%d,\"failed\":%d,\"rt_denied\":%d,"
		       "\"pass_us\":%.1f,\"worst_sched_us\":%.1f,\"drains\":[",
		       live, failed, 0, delta_us(p0, p1), worst_sched);
		for (int k = 0; k < g_ndr; k++) {
			struct drainer *d = &g_dr[k];

			printf("%s{\"cpu\":%d,\"ran_on\":%d,\"prio\":%d,"
			       "\"sched_us\":%.1f,\"drain_us\":%.1f,\"err\":%d}",
			       k ? "," : "", d->cpu, d->ran_on, d->rtprio,
			       d->sched_us, d->drain_us, d->err);
		}
		printf("]}\n");
	} else {
		printf("drained %d CPU(s) in %.1f us (worst dispatch %.1f us, "
		       "drain cost %.1f us total)\n",
		       live - failed, delta_us(p0, p1), worst_sched, total);
		if (verbose)
			for (int k = 0; k < g_ndr; k++) {
				struct drainer *d = &g_dr[k];

				printf("  cpu%-4d ran_on=%-4d prio=%-3d "
				       "dispatch=%8.1fus drain=%7.1fus%s%s\n",
				       d->cpu, d->ran_on, d->rtprio,
				       d->sched_us, d->drain_us,
				       d->err ? " ERR=" : "",
				       d->err ? strerror(d->err) : "");
			}
	}
	return failed ? 1 : 0;
}

/* ----------------------------------------------------------------------- arm */

#define ARM_QUIET_MS	2000	/* idle this long => the migration is done */
#define ARM_POLL_MS	100	/* how often to look for the migration worker */

static volatile sig_atomic_t g_fwd_sig;

static void arm_sighandler(int s)
{
	g_fwd_sig = s;
}

/*
 * Drain, run the command, and keep draining until the migration it kicks off
 * has actually finished.
 *
 * The window that matters is not "drain -> write".  It is "drain -> every
 * cpu_needs_drain() read made by every queued cpuset_migrate_mm work item",
 * and those run on an unbound kworker while the writer waits for completion.
 * See "THE WRITE IS NOT WHERE THE MIGRATION HAPPENS" at the top of this file.
 */
static int cmd_arm(struct opts *o, char **argv, int execpos)
{
	struct timespec ts, t_start, t_exit, t_quiet, t_poll = { 0, 0 }, now;
	struct sigaction sa;
	unsigned long passes = 0;
	pid_t pid;
	int status = 0, child_done = 0, rc;
	int undetectable = 0, capped = 0, inflight = 0;

	if (drainers_start(o->mask, o->rtprio) != 0)
		return 1;

	/* one drain before the write, as before */
	rc = drain_pass(o, 1);
	if (rc) {
		/* Exiting terminates all drainers; joining a starved one can hang. */
		return rc;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = arm_sighandler;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	clock_gettime(CLOCK_MONOTONIC, &t_start);
	t_exit = t_quiet = t_start;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		drainers_stop();
		return 1;
	}
	if (pid == 0) {
		/* between fork() and exec() only async-signal-safe calls */
		execvp(argv[execpos], &argv[execpos]);
		perror("execvp");
		_exit(127);
	}

	ts.tv_sec = o->interval_ms / 1000;
	ts.tv_nsec = (long)(o->interval_ms % 1000) * 1000000L;

	for (;;) {
		if (drain_pass(o, 0) != 0) {
			fprintf(stderr, "arm: drain failed; command PID %ld may still "
				"be running and migration completion is unknown\n",
				(long)pid);
			return 1;
		}
		passes++;

		if (g_fwd_sig) {
			int sig = g_fwd_sig;

			g_fwd_sig = 0;
			kill(pid, sig);
		}

		if (!child_done && waitpid(pid, &status, WNOHANG) == pid) {
			child_done = 1;
			clock_gettime(CLOCK_MONOTONIC, &t_exit);
			t_quiet = t_exit;
			vrb("command exited; draining until cpuset_migrate_mm_wq settles\n");
		}

		clock_gettime(CLOCK_MONOTONIC, &now);

		if (delta_us(t_poll, now) / 1000.0 >= ARM_POLL_MS) {
			t_poll = now;
			inflight = migration_in_flight();
			if (inflight > 0)
				t_quiet = now;
			else if (inflight < 0)
				undetectable = 1;
		}

		if (child_done) {
			double since_exit = delta_us(t_exit, now) / 1000.0;
			double quiet = delta_us(t_quiet, now) / 1000.0;

			if (o->settle_ms > 0 && since_exit >= o->settle_ms) {
				capped = 1;
				break;
			}
			/*
			 * Without readable kworker stacks there is no signal
			 * to stop on, so drain the whole settle window rather
			 * than guess that the migration is over.
			 */
			if (!undetectable && quiet >= ARM_QUIET_MS)
				break;
			if (undetectable && o->settle_ms <= 0)
				break;
		}

		nanosleep(&ts, NULL);
	}

	if (!child_done)
		waitpid(pid, &status, 0);

	clock_gettime(CLOCK_MONOTONIC, &now);
	drainers_stop();

	if (!o->json)
		printf("armed: %lu drain passes over %.1f s (interval %d ms)\n",
		       passes, delta_us(t_start, now) / 1e6, o->interval_ms);
	if (undetectable)
		fprintf(stderr,
			"note: cannot read kworker stacks (need root and "
			"CONFIG_STACKTRACE); drained for the full --settle "
			"window instead of watching for the migration.\n");
	if (capped && inflight > 0)
		fprintf(stderr,
			"warning: cpuset_migrate_mm_wq still busy at the "
			"--settle cap of %d ms - the migration outlived the "
			"drain. Raise --settle or run `lru-isolate guard'.\n",
			o->settle_ms);

	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return 0;
}

/* --------------------------------------------------------------------- check */

static int cmdline_has_isolcpus_domain(char *isolcpus, size_t n)
{
	char buf[4096];
	char *p;

	isolcpus[0] = 0;
	if (read_first_line("/proc/cmdline", buf, sizeof(buf)) != 0)
		return -1;
	p = strstr(buf, "isolcpus=");
	if (!p)
		return -1;
	p += strlen("isolcpus=");
	snprintf(isolcpus, n, "%.*s", (int)strcspn(p, " "), p);
	/* flags precede the cpulist: isolcpus=[domain,][managed_irq,][nohz,]<list> */
	return strstr(isolcpus, "domain") ? 1 : 0;
}

static int cmd_check(void)
{
	char *mask = calloc((size_t)g_ncpu, 1);
	char why[256], isolcpus[256], val[64];
	int iso, dom, exposed1 = 0, exposed2 = 0, unreachable = 0;
	cpu_set_t *allowed;
	struct utsname u;

	if (!mask)
		return 1;
	uname(&u);
	printf("kernel        : %s\n", u.release);
	printf("cpus          : %d\n", g_ncpu);

	iso = detect_isolated(mask, why, sizeof(why));
	if (iso != 0) {
		printf("isolated CPUs : none\n");
		printf("\nVERDICT: not exposed. LP#2165410 needs nohz_full/isolated CPUs\n"
		       "         occupied by SCHED_FIFO threads.\n");
		free(mask);
		return 0;
	}
	printf("isolated CPUs : %s\n", why);

	dom = cmdline_has_isolcpus_domain(isolcpus, sizeof(isolcpus));
	if (dom < 0)
		printf("isolcpus      : (not on cmdline)\n");
	else
		printf("isolcpus      : %s  [domain flag: %s]\n", isolcpus,
		       dom ? "present" : "MISSING");

	if (read_first_line("/sys/module/rcupdate/parameters/rcu_normal",
			    val, sizeof(val)) == 0)
		printf("rcu_normal    : %s\n", val);
	else
		strcpy(val, "?");

	/* can we even reach the isolated cores from here? */
	allowed = CPU_ALLOC((size_t)g_ncpu);
	if (allowed) {
		CPU_ZERO_S(g_setsz, allowed);
		if (sched_getaffinity(0, g_setsz, allowed) == 0)
			for (int c = 0; c < g_ncpu; c++)
				if (mask[c] &&
				    !CPU_ISSET_S((size_t)c, g_setsz, allowed))
					unreachable++;
		CPU_FREE(allowed);
	}
	printf("reachable     : %d of %d isolated CPU(s)\n",
	       count_mask(mask) - unreachable, count_mask(mask));

	if (strcmp(val, "1") != 0 && dom != 1)
		exposed1 = 1;
	{
		int *rtmax = calloc((size_t)g_ncpu, sizeof(int));

		if (rtmax) {
			scan_rt_prios(rtmax);
			for (int c = 0; c < g_ncpu; c++)
				if (mask[c] && rtmax[c] > 0)
					exposed2++;
			free(rtmax);
		}
	}

	printf("\n");
	printf("cause 1  synchronize_rcu_expedited() stall : %s\n",
	       exposed1 ? "EXPOSED" : "mitigated");
	if (exposed1)
		printf("         fix: boot with rcupdate.rcu_normal=1 (the module_param\n"
		       "              is 0444 - not runtime writable, reboot required),\n"
		       "              or use isolcpus=domain,nohz,<list>.\n"
		       "         lru-isolate CANNOT mitigate this one.\n");
	printf("cause 2  __lru_add_drain_all() flush stall  : %s\n",
	       exposed2 ? "EXPOSED" : "mitigated");
	if (exposed2)
		printf("         %d isolated CPU(s) currently carry SCHED_FIFO/RR tasks.\n"
		       "         fix: lru-isolate arm -- <your cpuset write>\n",
		       exposed2);
	if (unreachable)
		printf("\nWARNING: %d isolated CPU(s) are outside this process's affinity\n"
		       "         mask, so lru-isolate cannot drain them. Check the unit's\n"
		       "         AllowedCPUs=/CPUAffinity= or any taskset wrapper.\n",
		       unreachable);

	printf("\nVERDICT: %s\n",
	       (exposed1 || exposed2) ? "exposed - see above"
				      : "both causes mitigated");
	free(mask);
	return (exposed1 || exposed2) ? 1 : 0;
}

/* ---------------------------------------------------------------------- main */

static void usage(void)
{
	fprintf(stderr,
"lru-isolate - userspace mitigation for LP#2165410 (cpuset migration D-state)\n"
"\n"
"usage: lru-isolate <command> [options]\n"
"\n"
"commands:\n"
"  check                 report exposure to both causes and what is missing\n"
"  drain                 one drain pass over the target CPUs\n"
"  guard                 drain every --interval ms until killed\n"
"  arm -- <cmd...>       run <cmd> while draining continuously, and keep\n"
"                        draining until the migration it triggers finishes\n"
"\n"
"options:\n"
"  -c, --cpus LIST       target CPUs (default: nohz_full + isolated)\n"
"  -p, --rtprio N|auto|none\n"
"                        SCHED_FIFO priority for the drain threads.\n"
"                        'auto' (default) picks the highest userspace RT\n"
"                        priority seen on each target CPU + 1, so it can\n"
"                        preempt the busy-poll thread.\n"
"                        Rechecks after 100 ms without a response and raises\n"
"                        priority for RT tasks started later. A drainer wait\n"
"                        fails after 5 s; RT permission failures are fatal.\n"
"  -i, --interval MS     drain interval: guard default 1000, arm default 10\n"
"      --settle MS       arm: cap on how long to keep draining after the\n"
"                        command exits (default 300000, 0 = no cap). arm\n"
"                        normally stops ~2 s after cpuset_migrate_mm_wq\n"
"                        goes idle; this is only the backstop.\n"
"      --serial          drain one CPU at a time instead of all at once\n"
"      --bh-flush DEV    also invalidate the buffer-head LRU via BLKFLSBUF\n"
"                        on DEV (IPI-based; clears the one cpu_needs_drain()\n"
"                        term madvise cannot reach). Drops DEV's page cache.\n"
"  -j, --json            machine-readable output\n"
"  -v, --verbose         per-CPU detail\n"
"\n"
"why arm does not just exec:\n"
"  A cpuset.mems write does not migrate anything itself. It queues one work\n"
"  item per task in the cpuset onto cpuset_migrate_mm_wq - an ordered, unbound\n"
"  workqueue. The writer flushes that queue from task_work before returning\n"
"  to userspace. Those items call lru_cache_disable() serially on a kworker/u*.\n"
"  Draining once cannot cover the whole migration.\n"
"\n"
"isolated cores:\n"
"  Drain threads are created once and parked on a futex, so a pass costs two\n"
"  context switches and one madvise() on the isolated core - no clone()/exit()\n"
"  and no page faults. mlockall() runs before they are created so their stacks\n"
"  are populated on a housekeeping CPU. The main loop pins itself to the\n"
"  complement of the target set. Use -v to see measured dispatch and drain\n"
"  times per core for your latency budget.\n"
"\n"
"examples:\n"
"  lru-isolate check\n"
"  lru-isolate arm -- systemctl set-property my.slice AllowedMemoryNodes=0\n"
"  lru-isolate guard -i 1000 -c 18-95,108-178\n");
}

int main(int argc, char **argv)
{
	struct opts o = { .rtprio = -1, .interval_ms = 1000, .settle_ms = 300000 };
	const char *cmd;
	char why[256];
	int explicit_cpus = 0, explicit_interval = 0, execpos = 0, rc;

	/* guard runs as a systemd unit writing to the journal, where a
	 * block-buffered stdout emits nothing until 4K has piled up. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	g_ncpu = (int)sysconf(_SC_NPROCESSORS_CONF);
	if (g_ncpu < 1)
		g_ncpu = 1;
	g_setsz = CPU_ALLOC_SIZE((size_t)g_ncpu);

	if (argc < 2) {
		usage();
		return 2;
	}
	cmd = argv[1];
	if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help")) {
		usage();
		return 0;
	}

	o.mask = calloc((size_t)g_ncpu, 1);
	if (!o.mask) {
		fprintf(stderr, "out of memory\n");
		return 1;
	}

	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--")) {
			execpos = i + 1;
			break;
		} else if ((!strcmp(argv[i], "-c") || !strcmp(argv[i], "--cpus")) &&
			   i + 1 < argc) {
			if (parse_cpulist(argv[++i], o.mask) != 0) {
				fprintf(stderr, "bad cpu list: %s\n", argv[i]);
				return 2;
			}
			explicit_cpus = 1;
		} else if ((!strcmp(argv[i], "-p") || !strcmp(argv[i], "--rtprio")) &&
			   i + 1 < argc) {
			i++;
			if (!strcmp(argv[i], "auto"))
				o.rtprio = -1;
			else if (!strcmp(argv[i], "none") || !strcmp(argv[i], "0"))
				o.rtprio = 0;
			else
				o.rtprio = atoi(argv[i]);
		} else if ((!strcmp(argv[i], "-i") || !strcmp(argv[i], "--interval")) &&
			   i + 1 < argc) {
			o.interval_ms = atoi(argv[++i]);
			explicit_interval = 1;
		} else if (!strcmp(argv[i], "--settle") && i + 1 < argc) {
			o.settle_ms = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--serial")) {
			o.serial = 1;
		} else if (!strcmp(argv[i], "--bh-flush") && i + 1 < argc) {
			o.bhdev = argv[++i];
		} else if (!strcmp(argv[i], "-j") || !strcmp(argv[i], "--json")) {
			o.json = 1;
		} else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
			verbose = 1;
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage();
			return 0;
		} else {
			fprintf(stderr, "unknown option: %s\n", argv[i]);
			return 2;
		}
	}

	if (!strcmp(cmd, "check"))
		return cmd_check();

	if (!explicit_cpus && detect_isolated(o.mask, why, sizeof(why)) != 0) {
		fprintf(stderr,
			"no nohz_full/isolated CPUs found; pass --cpus explicitly.\n");
		return 2;
	}

	if (!strcmp(cmd, "drain")) {
		if (drainers_start(o.mask, o.rtprio) != 0)
			return 1;
		rc = drain_pass(&o, 1);
		if (!rc)
			drainers_stop();
		return rc;
	}

	if (!strcmp(cmd, "guard")) {
		struct timespec ts = {
			.tv_sec = o.interval_ms / 1000,
			.tv_nsec = (long)(o.interval_ms % 1000) * 1000000L,
		};

		if (drainers_start(o.mask, o.rtprio) != 0)
			return 1;
		for (;;) {
			if (drain_pass(&o, 1) != 0)
				return 1;
			nanosleep(&ts, NULL);
		}
	}

	if (!strcmp(cmd, "arm")) {
		if (!execpos || execpos >= argc) {
			fprintf(stderr, "arm needs: -- <command...>\n");
			return 2;
		}
		/*
		 * The migration runs serially on a worker while the writer can
		 * block, so arm drains continuously rather than once.
		 * A 1 s guard interval would leave most of that uncovered.
		 */
		if (!explicit_interval)
			o.interval_ms = 10;
		return cmd_arm(&o, argv, execpos);
	}

	usage();
	return 2;
}

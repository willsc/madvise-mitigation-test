// SPDX-License-Identifier: GPL-2.0-or-later
/* Exercise controller recovery with a synthetic /proc and scheduler. No RT
 * policy changes, CPU saturation, cpuset writes or kernel tracing are used. */
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static char fixture[256];
static int fail_priority, priority_calls, acknowledge_startup;
static DIR *fixture_opendir(const char *path);
static int fixture_open(const char *path, int flags, ...);
static int fixture_affinity(pid_t tid, size_t size, cpu_set_t *mask);
static int fixture_priority(pthread_t thread, int policy,
			    const struct sched_param *param);

#define opendir fixture_opendir
#define open fixture_open
#define sched_getaffinity fixture_affinity
#define pthread_setschedparam fixture_priority
#define main lru_isolate_main
#include "../src/lru-isolate.c"
#undef main
#undef opendir
#undef open
#undef sched_getaffinity
#undef pthread_setschedparam

static DIR *fixture_opendir(const char *path)
{
	char translated[1024];
	assert(!strncmp(path, "/proc", 5));
	snprintf(translated, sizeof(translated), "%s%s", fixture, path + 5);
	return opendir(translated);
}

static int fixture_open(const char *path, int flags, ...)
{
	char translated[1024];
	assert(!strncmp(path, "/proc", 5));
	snprintf(translated, sizeof(translated), "%s%s", fixture, path + 5);
	return open(translated, flags);
}

static int fixture_affinity(pid_t tid, size_t size, cpu_set_t *mask)
{
	(void)tid;
	CPU_ZERO_S(size, mask);
	CPU_SET_S(1, size, mask); /* last CPU in stat is 0; allowed CPU is 1 */
	return 0;
}

static int fixture_priority(pthread_t thread, int policy,
			    const struct sched_param *param)
{
	(void)thread;
	assert(policy == SCHED_FIFO);
	assert(param->sched_priority > g_dr[0].rtprio);
	priority_calls++;
	if (fail_priority)
		return EPERM;
	/* Model the previously starved thread running once it can preempt. */
	atomic_store(acknowledge_startup ? &g_dr[0].ready : &g_dr[0].done,
		     acknowledge_startup ? 1 : atomic_load(&g_dr[0].req));
	return 0;
}

static void write_task(long pid, int priority, long flags)
{
	char path[512];
	FILE *f;
	snprintf(path, sizeof(path), "%s/%ld", fixture, pid);
	assert(mkdir(path, 0700) == 0 || errno == EEXIST);
	snprintf(path, sizeof(path), "%s/%ld/task", fixture, pid);
	assert(mkdir(path, 0700) == 0 || errno == EEXIST);
	snprintf(path, sizeof(path), "%s/%ld/task/%ld", fixture, pid, pid);
	assert(mkdir(path, 0700) == 0 || errno == EEXIST);
	snprintf(path, sizeof(path), "%s/%ld/task/%ld/stat", fixture, pid, pid);
	f = fopen(path, "w");
	assert(f);
	fprintf(f, "%ld (test worker) R", pid);
	for (int field = 4; field <= 41; field++)
		fprintf(f, " %ld", field == 9 ? flags : field == 40 ? priority :
			field == 41 && priority ? (long)SCHED_FIFO : 0L);
	fputc('\n', f);
	assert(fclose(f) == 0);
}

static void remove_task(long pid)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%ld/task/%ld/stat", fixture, pid, pid);
	assert(unlink(path) == 0);
	snprintf(path, sizeof(path), "%s/%ld/task/%ld", fixture, pid, pid);
	assert(rmdir(path) == 0);
	snprintf(path, sizeof(path), "%s/%ld/task", fixture, pid);
	assert(rmdir(path) == 0);
	snprintf(path, sizeof(path), "%s/%ld", fixture, pid);
	assert(rmdir(path) == 0);
}

int main(void)
{
	struct drainer dr = { .cpu = 1, .live = 1 };
	struct opts opts = { .rtprio = -1 };
	int maxima[2];
	long workload = (long)getpid() + 1000000;
	long kthread = workload + 1;
	struct timespec start, end;

	snprintf(fixture, sizeof(fixture), "/tmp/lru-drainer-test-XXXXXX");
	assert(mkdtemp(fixture));
	g_ncpu = 2;
	g_setsz = CPU_ALLOC_SIZE(2);
	g_dr = &dr;
	g_ndr = 1;
	g_rtmax = maxima;
	write_task(getpid(), 99, 0); /* self must not cause priority escalation */
	write_task(kthread, 99, PF_KTHREAD);
	write_task(workload, 0, 0);
	scan_rt_prios(maxima);
	assert(maxima[0] == 0 && maxima[1] == 0);

	write_task(workload, 80, 0); /* guard was started before the workload */
	assert(drain_pass(&opts, 0) == 0);
	assert(dr.rtprio == 81 && priority_calls == 1 && maxima[0] == 0);
	assert(refresh_rt_prios() == 0);
	assert(priority_calls == 1); /* rescans do not count the guard itself */

	write_task(workload, 90, 0); /* priority changed while guard was running */
	opts.serial = 1;
	assert(drain_pass(&opts, 0) == 0);
	assert(dr.rtprio == 91 && priority_calls == 2);

	write_task(workload, 99, 0);
	assert(drain_pass(&opts, 0) == 1);
	assert(priority_calls == 2); /* cannot preempt a FIFO/99 peer */

	write_task(workload, 95, 0);
	fail_priority = 1;
	assert(drain_pass(&opts, 0) == 1);
	assert(dr.rtprio == 91 && priority_calls == 3);
	fail_priority = 0;
	acknowledge_startup = 1;
	assert(wait_for_drainer(&dr, &dr.ready, 1, "startup") == 0);
	assert(dr.rtprio == 96 && priority_calls == 4);

	g_rtmax = NULL; /* fixed priorities must not be changed automatically */
	clock_gettime(CLOCK_MONOTONIC, &start);
	assert(drain_pass(&opts, 0) == 1);
	clock_gettime(CLOCK_MONOTONIC, &end);
	assert(delta_us(start, end) >= DRAIN_TIMEOUT_MS * 1000.0);
	assert(priority_calls == 4);

	remove_task(getpid());
	remove_task(workload);
	remove_task(kthread);
	assert(rmdir(fixture) == 0);
	puts("PASS: late RT load, changed priority, affinity, self/kernel exclusion, "
	     "priority ceiling, permission failure, startup recovery and timeout");
	return 0;
}

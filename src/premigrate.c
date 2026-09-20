// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * premigrate - shrink the work a cpuset.mems migration has to do (LP#2165410)
 *
 * Secondary half of the mitigation.  lru-isolate removes the *stall*; this
 * reduces the *duration* of the migration that follows, by reclaiming
 * anonymous memory out of the target tasks before the cpuset write.
 *
 *   do_migrate_pages() -> migrate_to_node() -> queue_pages_range() walks the
 *   whole address space and migrate_pages() moves every resident page.  Fewer
 *   resident pages, less work, shorter window.
 *
 * Honest scoping: MADV_PAGEOUT only actually lowers RSS if the pages can go
 * somewhere - i.e. swap is configured for anonymous memory.  With no swap it
 * degrades to roughly what MADV_COLD does (deactivation), which makes the
 * pages reclaim candidates but does not itself free them.  Check `free -h'
 * before expecting much from this on a swapless host.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#ifndef MADV_COLD
#define MADV_COLD 20
#endif
#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif
#ifndef __NR_pidfd_open
#define __NR_pidfd_open 434
#endif
#ifndef __NR_process_madvise
#define __NR_process_madvise 440
#endif

#define BATCH 1024	/* UIO_MAXIOV */

static int pidfd_open(pid_t pid)
{
	return (int)syscall(__NR_pidfd_open, pid, 0U);
}

static ssize_t process_madvise_(int pidfd, const struct iovec *iov, size_t n,
				int advice)
{
	return syscall(__NR_process_madvise, pidfd, iov, n, advice, 0U);
}

int main(int argc, char **argv)
{
	int advice = MADV_PAGEOUT, dry = 0, pidfd;
	unsigned long minkb = 4096;	/* ignore VMAs under 4 MiB */
	pid_t pid = 0;
	char path[64], line[512];
	FILE *f;
	struct iovec iov[BATCH];
	size_t n = 0;
	unsigned long long total = 0, done = 0;
	int vmas = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--pid") && i + 1 < argc)
			pid = (pid_t)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--cold"))
			advice = MADV_COLD;
		else if (!strcmp(argv[i], "--pageout"))
			advice = MADV_PAGEOUT;
		else if (!strcmp(argv[i], "--min-kb") && i + 1 < argc)
			minkb = strtoul(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "--dry-run"))
			dry = 1;
		else {
			fprintf(stderr,
"usage: premigrate --pid PID [--pageout|--cold] [--min-kb K] [--dry-run]\n"
"\n"
"Applies process_madvise(2) to PID's large private anonymous VMAs so that a\n"
"following cpuset.mems / AllowedMemoryNodes= change has fewer resident pages\n"
"to migrate.  Needs CAP_SYS_NICE and ptrace read access to PID.\n"
"\n"
"  --pageout   reclaim now (default; needs swap to lower RSS)\n"
"  --cold      deactivate only, no I/O\n"
"  --min-kb K  skip VMAs smaller than K KiB (default 4096)\n");
			return 2;
		}
	}
	if (!pid) {
		fprintf(stderr, "--pid is required\n");
		return 2;
	}

	snprintf(path, sizeof(path), "/proc/%d/maps", pid);
	f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "open %s: %s\n", path, strerror(errno));
		return 1;
	}
	pidfd = pidfd_open(pid);
	if (pidfd < 0 && !dry) {
		fprintf(stderr, "pidfd_open(%d): %s\n", pid, strerror(errno));
		fclose(f);
		return 1;
	}

	while (fgets(line, sizeof(line), f)) {
		unsigned long long a, b;
		char perms[8], rest[256];
		int got;

		rest[0] = 0;
		got = sscanf(line, "%llx-%llx %7s %*s %*s %*s %255[^\n]",
			     &a, &b, perms, rest);
		if (got < 3)
			continue;
		/* private, writable, anonymous (no backing file) */
		if (perms[1] != 'w' || perms[3] != 'p')
			continue;
		{
			char *p = rest;
			while (*p == ' ')
				p++;
			/* a pathname or a special region other than [heap] */
			if (*p && strcmp(p, "[heap]") != 0)
				continue;
		}
		if ((b - a) / 1024 < minkb)
			continue;

		total += b - a;
		vmas++;
		if (dry)
			continue;

		iov[n].iov_base = (void *)(uintptr_t)a;
		iov[n].iov_len = (size_t)(b - a);
		if (++n == BATCH) {
			ssize_t r = process_madvise_(pidfd, iov, n, advice);
			if (r < 0) {
				fprintf(stderr, "process_madvise: %s\n",
					strerror(errno));
				fclose(f);
				close(pidfd);
				return 1;
			}
			done += (unsigned long long)r;
			n = 0;
		}
	}
	fclose(f);

	if (!dry && n) {
		ssize_t r = process_madvise_(pidfd, iov, n, advice);
		if (r < 0)
			fprintf(stderr, "process_madvise: %s\n", strerror(errno));
		else
			done += (unsigned long long)r;
	}
	if (pidfd >= 0)
		close(pidfd);

	printf("pid %d: %d anon VMA(s), %llu MiB eligible", pid, vmas,
	       total / (1024 * 1024));
	if (dry)
		printf(" (dry run)\n");
	else
		printf(", %llu MiB advised (%s)\n", done / (1024 * 1024),
		       advice == MADV_COLD ? "MADV_COLD" : "MADV_PAGEOUT");
	return 0;
}

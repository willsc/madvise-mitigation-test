// SPDX-License-Identifier: GPL-2.0-or-later
/* Check diagnostics against synthetic proc/sysfs data; no RT or boot changes. */
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *normal_value, *isolation_value = "1\n";
static const char *cmdline_value;
static char empty_proc[128];
static FILE *fixture_fopen(const char *path, const char *mode);
static DIR *fixture_opendir(const char *path);
static int fixture_affinity(pid_t tid, size_t size, cpu_set_t *mask);

#define fopen fixture_fopen
#define opendir fixture_opendir
#define sched_getaffinity fixture_affinity
#define main lru_isolate_main
#include "../src/lru-isolate.c"
#undef main
#undef sched_getaffinity
#undef opendir
#undef fopen

static FILE *fixture_fopen(const char *path, const char *mode)
{
	const char *value = NULL;
	FILE *f;

	assert(!strcmp(mode, "r"));
	if (!strcmp(path, "/sys/module/rcupdate/parameters/rcu_normal"))
		value = normal_value;
	else if (!strcmp(path, "/proc/cmdline"))
		value = cmdline_value;
	else if (!strcmp(path, "/sys/devices/system/cpu/nohz_full") ||
		 !strcmp(path, "/sys/devices/system/cpu/isolated"))
		value = isolation_value;
	else
		assert(!"unexpected file read");
	if (!value) {
		errno = EACCES;
		return NULL;
	}
	f = tmpfile();
	assert(f && fputs(value, f) >= 0);
	rewind(f);
	return f;
}

static DIR *fixture_opendir(const char *path)
{
	assert(!strcmp(path, "/proc"));
	return opendir(empty_proc);
}

static int fixture_affinity(pid_t tid, size_t size, cpu_set_t *mask)
{
	(void)tid;
	CPU_ZERO_S(size, mask);
	CPU_SET_S(0, size, mask);
	CPU_SET_S(1, size, mask);
	return 0;
}

static void check_case(const char *value, const char *cmdline,
		       int expected_rc, const char *expected)
{
	FILE *output = tmpfile();
	char text[8192];
	int saved_stdout, rc;
	size_t n;

	assert(output);
	normal_value = value;
	cmdline_value = cmdline;
	assert(fflush(stdout) == 0);
	saved_stdout = dup(STDOUT_FILENO);
	assert(saved_stdout >= 0);
	assert(dup2(fileno(output), STDOUT_FILENO) >= 0);
	rc = cmd_check();
	assert(fflush(stdout) == 0);
	assert(dup2(saved_stdout, STDOUT_FILENO) >= 0);
	close(saved_stdout);
	rewind(output);
	n = fread(text, 1, sizeof(text) - 1, output);
	text[n] = 0;
	fclose(output);
	assert(rc == expected_rc);
	assert(strstr(text, expected));
	assert(strstr(text, "not a proof of migration progress"));
	assert(!strstr(text, "both causes mitigated"));
	if (expected_rc)
		assert(!strstr(text, "BYPASSED"));
}

int main(void)
{
	const char *domain = "nohz_full=1 isolcpus=domain,managed_irq,1\n";
	const char *no_domain = "nohz_full=1\n";

	snprintf(empty_proc, sizeof(empty_proc), "/tmp/lru-check-test-XXXXXX");
	assert(mkdtemp(empty_proc));
	g_ncpu = 2;
	g_setsz = CPU_ALLOC_SIZE(2);
	check_case("0\n", domain, 1, "ENABLED (progress unverified)");
	check_case("0\n", no_domain, 1, "ENABLED (progress unverified)");
	check_case("1\n", domain, 0, "BYPASSED (normal grace periods)");
	check_case("1\n", no_domain, 0, "BYPASSED (normal grace periods)");
	check_case("2\n", domain, 0, "BYPASSED (normal grace periods)");
	check_case(NULL, domain, 1, "UNKNOWN");
	check_case("invalid\n", domain, 1, "UNKNOWN");
	isolation_value = "\n";
	check_case("0\n", "\n", 1, "ENABLED (progress unverified)");
	assert(rmdir(empty_proc) == 0);
	puts("PASS: RCU mode checked independently of domain/nohz flags; unreadable mode is unknown");
	return 0;
}

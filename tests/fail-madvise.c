// SPDX-License-Identifier: GPL-2.0-or-later
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>

int madvise(void *addr, size_t length, int advice)
{
	static int calls;
	const char *after = getenv("LRU_TEST_FAIL_AFTER");
	(void)addr;
	(void)length;
	(void)advice;
	/* One drainer in the CLI test. Simulate successful earlier passes. */
	if (after && calls++ < atoi(after))
		return 0;
	errno = EINVAL;
	return -1;
}

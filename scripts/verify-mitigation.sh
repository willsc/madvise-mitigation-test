#!/bin/sh
# Superseded by src/lru-verify.c - run `make verify' (or ./lru-verify).
#
# The shell harness forked grep/seq/subshells inside the measurement window.
# Every one of those faults pages and dirties LRU batches on whatever CPU it
# lands on, which is noise in exactly the signal being measured. lru-verify is
# a single process that forks nothing, talks to tracefs directly, and reads the
# per-CPU ring buffer counters instead of matching on the trace CPU column.
#
# This file is kept only so existing runbooks fail loudly rather than silently
# testing something different. Delete it once nothing references it.
echo "verify-mitigation.sh is superseded by lru-verify; run 'make verify'." >&2
exit 64

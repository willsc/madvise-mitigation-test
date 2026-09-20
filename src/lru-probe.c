/*
 * Superseded by src/lru-verify.c - not built.
 *
 * lru-probe was the dirty/drain/trigger half of the proof, driven from
 * scripts/verify-mitigation.sh. Both are now one C program:
 *
 *   - nothing forks inside the measurement window (the shell wrapper forked
 *     grep/seq/subshells, which fault pages and dirty LRU batches on whatever
 *     CPU they land on - noise in the signal being measured);
 *   - the dirty and drain workers are persistent and futex-parked, so no
 *     pthread_exit() runs on the target CPU after the drain and re-dirties the
 *     batch it just emptied;
 *   - the drain worker runs on a pre-faulted mlocked stack, so it cannot fault
 *     on the target CPU at all;
 *   - detection reads per_cpu/cpuN/stats "entries:" instead of matching the
 *     "[015]" column in the trace text.
 *
 * Delete this file once nothing references it.
 */

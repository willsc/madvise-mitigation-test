# `lru-isolate` — a userspace madvise mitigation for LP#2165410

> [LP#2165410](https://bugs.launchpad.net/ubuntu/+source/linux-aws/+bug/2165410) —
> *systemd D-state during cpuset migration on nohz_full CPUs*, linux-aws 7.0 on
> 192-vCPU `c8a.metal-48xl`. PID 1 blocks in `D` for 60 s – 10 min during a
> `cpuset.mems` write and SSH logins stop working.

**Short answer: only on a cpuset holding a handful of threads, and even then
for one of the bug's two causes.** The `madvise` lever is real and measurable —
it removes a CPU from the `__lru_add_drain_all()` flush set — but a
`cpuset.mems` write queues one migration **per thread** in the cpuset, and each
one gets an independent chance to hit a dirty batch. At RT-daemon thread counts
those odds compound to roughly nothing. It never touches the
`synchronize_rcu_expedited()` stall at all, which needs a boot parameter.

**If you are here because your box still hangs, the useful fixes are in
[What actually fixes this](#what-actually-fixes-this), not in this tool.** Read
[Scope](#scope-what-this-does-and-does-not-fix) before deploying anything.

---

## The mechanism

A `cpuset.mems` write (systemd `AllowedMemoryNodes=`) reaches:

```
cpuset_migrate_mm()
  └─ do_migrate_pages()                 mm/mempolicy.c   — calls the next line unconditionally
       └─ lru_cache_disable()           mm/swap.c
            ├─ synchronize_rcu_expedited()      ← cause 1
            └─ __lru_add_drain_all(true)        ← cause 2
```

### The write is not where the migration happens

`do_migrate_pages()` does not run in the context of the `cpuset.mems` write.
`update_tasks_nodemask()` walks the cpuset and calls `cpuset_migrate_mm()` once
**per task**, and each call queues a work item and returns
(`kernel/cgroup/cpuset.c`):

```c
css_task_iter_start(&cs->css, 0, &it);
while ((task = css_task_iter_next(&it))) {        /* :2641 */
        ...
        if (migrate)
                cpuset_migrate_mm(mm, &cs->old_mems_allowed, &newmems);
}
        └─ queue_work(cpuset_migrate_mm_wq, &mwork->work);   /* :2556 */

cpuset_migrate_mm_wq = alloc_ordered_workqueue("cpuset_migrate_mm", 0);  /* :4010 */
```

The workqueue is **ordered** — `max_active=1` — so N tasks in the cpuset means N
`lru_cache_disable()` calls run strictly serially on an unbound `kworker/u*`,
each re-reading `cpu_needs_drain()` for every CPU, for as long as the migration
takes. The write itself returns immediately. This is why a `D`-state hang shows
up as `kworker/u<pool>:<n>` blocked with PID 1 piled up behind it, rather than
PID 1 blocked directly in the write.

It is also why a single drain before the write buys nothing: **the window to
cover is the whole migration, not the write.**

### One migration per thread, not per process

The iterator above takes `flags = 0`. `CSS_TASK_ITER_PROCS` is "walk only
threadgroup leaders" (`include/linux/cgroup.h:46`), and it is **not** set — so
the walk covers every *thread*, and queues a work item for each one even though
threads share an `mm`.

A `cpuset.mems` write against a 148-thread RT daemon therefore queues 148 work
items, each calling `lru_cache_disable()` → `synchronize_rcu_expedited()` +
`__lru_add_drain_all()`, serially. That is the amplification behind the bug's
60 s – 10 min, and it is what makes the userspace mitigation untenable at scale:
each of the 148 items re-reads `cpu_needs_drain(cpu)` independently, so the
chance of a clean migration is `(1-p)^N` for a per-read dirty probability `p`.

| threads | `p`=1.7% | `p`=0.5% | `p`=0.1% |
|---|---|---|---|
| 1 | 98.3% | 99.5% | 99.9% |
| 16 | 76.0% | 92.3% | 98.4% |
| 48 | 43.9% | 78.6% | 95.3% |
| 148 | **7.9%** | 47.6% | 86.2% |
| 384 | **0.1%** | 14.6% | 68.1% |

`p`=1.7% is the best rate ever measured here (1/60 trials, on a quiet CPU —
see [Proof](#proof)). Writeback completion reaches `lru_move_tail` from IRQ
context without the RT thread doing anything, so `p` is never zero. Draining
harder raises no row in that table by much; only shrinking `N` does.

`__lru_add_drain_all()` in linux-aws 7.0 (`mm/swap.c`):

```c
for_each_online_cpu(cpu) {
        struct work_struct *work = &per_cpu(lru_add_drain_work, cpu);

        if (cpu_needs_drain(cpu)) {
                INIT_WORK(work, lru_add_drain_per_cpu);
                queue_work_on(cpu, mm_percpu_wq, work);
                __cpumask_set_cpu(cpu, &has_work);
        }
}

for_each_cpu(cpu, &has_work)
        flush_work(&per_cpu(lru_add_drain_work, cpu));
```

On a CPU saturated by a `SCHED_FIFO` busy-poll thread the per-CPU kworker
(`SCHED_NORMAL`) never gets scheduled, so `flush_work()` blocks — and PID 1 is
in `D`. With `CONFIG_RT_GROUP_SCHED=n` in 7.0 there is no RT bandwidth
throttling to ever let the kworker in, so the block is unbounded.

### The lever

Note what is *not* in the queueing condition: **`force_all_cpus`**. It only
forces the drain *pass* past the generation check at label (C); the decision to
queue work to a given CPU is `cpu_needs_drain(cpu)` alone.

```c
static bool cpu_needs_drain(unsigned int cpu)
{
        struct cpu_fbatches *fbatches = &per_cpu(cpu_fbatches, cpu);

        return folio_batch_count(&fbatches->lru_add) ||
                folio_batch_count(&fbatches->lru_move_tail) ||
                folio_batch_count(&fbatches->lru_deactivate_file) ||
                folio_batch_count(&fbatches->lru_deactivate) ||
                folio_batch_count(&fbatches->lru_lazyfree) ||
                folio_batch_count(&fbatches->lru_activate) ||
                need_mlock_drain(cpu) ||
                has_bh_in_lru(cpu, NULL);
}
```

**A CPU whose per-CPU batches are empty is never sent work and is never
flushed.** Keep the isolated CPUs' batches empty and the stall cannot happen —
which is exactly what proposed kernel patch #3 in the bug does, from inside the
kernel. We can get most of the way there from userspace.

`lru_add_drain()` clears every term above except `has_bh_in_lru()`:

```c
void lru_add_drain(void)
{
        local_lock(&cpu_fbatches.lock);
        lru_add_drain_cpu(smp_processor_id());   /* all six folio batches */
        local_unlock(&cpu_fbatches.lock);
        mlock_drain_local();                     /* need_mlock_drain */
}
```

and it is reachable from userspace via `madvise(2)` — `mm/madvise.c`:

```c
static long madvise_willneed(struct madvise_behavior *madv_behavior)
{
        ...
#ifdef CONFIG_SWAP
        if (!file) {
                walk_page_range_vma(vma, start, end, &swapin_walk_ops, vma);
                lru_add_drain();        /* Push any new pages onto the LRU now */
                return 0;
        }
```

So: **`madvise(MADV_WILLNEED)` on a never-touched anonymous page, issued by a
thread pinned to CPU N, empties CPU N's batches.** The walk finds no present
PTEs, so nothing is allocated and nothing is added — it is a pure drain.

### Why `MADV_WILLNEED` and not `MADV_COLD`

`MADV_COLD` and `MADV_PAGEOUT` also call `lru_add_drain()`, but they are gated
first:

```c
static long madvise_cold(struct madvise_behavior *madv_behavior)
{
        if (!can_madv_lru_vma(vma))
                return -EINVAL;         /* VM_LOCKED | VM_PFNMAP | VM_HUGETLB */

        lru_add_drain();
```

The `-EINVAL` returns **before** the drain. RT daemons routinely call
`mlockall(MCL_CURRENT|MCL_FUTURE)`, which sets `VM_LOCKED` on every VMA — so on
exactly the systems this bug affects, a `MADV_COLD`-based drain would silently
do nothing. `madvise_willneed()` has no such gate.

---

## Proof

`make verify` (`lru-verify`) demonstrates the queueing decision changing. It
needs no `SCHED_FIFO` spinner, no NUMA and no cpuset write, so it is safe on a
live machine. It puts a kprobe on `lru_add_drain_per_cpu` (the work function),
dirties one CPU's batch, and triggers `lru_cache_disable()` via `move_pages(2)`
— whose `do_pages_move()` opens with the same unconditional
`lru_cache_disable()` the cpuset path uses.

It is one process that forks nothing. That is not tidiness: the earlier shell
harness forked `grep`, `seq` and subshells *inside* the measurement window, and
every fork faults pages and dirties LRU batches on whatever CPU it lands on —
noise in exactly the signal being measured. It also carries the same
isolated-core discipline as the mitigation itself, so the harness cannot dirty
the CPU it is testing: the dirty and drain workers are persistent and
futex-parked (a per-trial thread would run `pthread_exit()` on the target CPU
*after* the drain), and the drain worker runs on a pre-faulted `mlock`ed stack
via `pthread_attr_setstack()`. `mlockall()` is deliberately `MCL_CURRENT` only —
`MCL_FUTURE` would populate the dirty region at `mmap` time on the wrong CPU and
the baseline would silently measure nothing.

Detection reads `per_cpu/cpuN/stats` (`entries:`) rather than matching the
`[015]` CPU column in the trace text.

Measured on this workstation (16 CPUs, 6.17.0-35-generic):

```
target cpu=15  trigger cpu=0  trials=20

  BASELINE  (no mitigation)                    cpu15 queued drain work in 20/20 trials
  MITIGATED (madvise drain on cpu15)           cpu15 queued drain work in  0/20 trials

PASS: the drain removed cpu15 from the flush set (20/20 -> 0/20).
```

Across 60 trials on three CPUs: **baseline 60/60, mitigated 1/60.**

That one residual matters and the script tolerates up to 10%. Between the drain
and the trigger, anything that dirties the target CPU's batch re-opens the race
— on a general-purpose CPU with background load that occasionally wins (one run
scored 3/10 during an activity burst). A correctly isolated CPU has far fewer
such events, but **not zero**: writeback completion reaches `lru_move_tail` from
IRQ context without the RT thread doing anything at all. See
[Operating model](#operating-model--how-this-holds-up-continually) for the full
source list and how to remove each one. The test box is a worse case than a
tuned isolated CPU, but the difference is a rate, not a guarantee.

A single `lru_cache_disable()` on an *idle desktop* queued drain work to **15 of
16 CPUs**. Nearly every CPU carries a dirty batch at any moment, so an isolated
CPU being in the flush set is the normal case, not an unlucky one.

A second kprobe run confirms the primitive lands where intended — four threads
pinned to CPUs 3, 7, 11, 14 produced exactly four `lru_add_drain()` calls, one
per target CPU:

```
lru-isolate-3775543 [003] ..... lrudrain: (lru_add_drain+0x0/0x50)
lru-isolate-3775544 [007] ..... lrudrain: (lru_add_drain+0x0/0x50)
lru-isolate-3775546 [014] ..... lrudrain: (lru_add_drain+0x0/0x50)
lru-isolate-3775545 [011] ..... lrudrain: (lru_add_drain+0x0/0x50)
```

Cost: **8–30 µs per CPU**, ~370 µs for a four-CPU pass including thread
dispatch.

---

## Scope: what this does and does not fix

| | Cause 1 — `synchronize_rcu_expedited()` | Cause 2 — `__lru_add_drain_all()` |
|---|---|---|
| Blocks because | `rcu_exp_par_gp_kthread_worker/N` is pinned to a nohz_full CPU and starves behind the FIFO spinner | the per-CPU kworker never runs on a FIFO-saturated CPU, so `flush_work()` waits |
| Fixed by this tool | **No** | **Yes** |
| Fix without a kernel patch | `rcupdate.rcu_normal=1` **at boot**, or `isolcpus=domain,nohz,<list>` | `lru-isolate` |

`do_migrate_pages()` calls `lru_cache_disable()` unconditionally, and
`lru_cache_disable()` calls `synchronize_rcu_expedited()` before the drain. No
`madvise()` can avoid that. Per the bug's comment #10, the captured stack shows
the RCU wait is the dominant blocking point — **so treat this tool as one half
of a mitigation, not the whole thing.**

`rcu_normal` is **not runtime-writable** — it is `module_param(rcu_normal, int,
0444)`, confirmed by `-r--r--r--` on `/sys/module/rcupdate/parameters/rcu_normal`.
It requires a reboot.

**Complete no-patch mitigation** = boot with `rcupdate.rcu_normal=1` (kills
cause 1) **+** keep `N` small (see below) **+** run `lru-isolate` (makes cause 2
unlikely per migration, not impossible). `lru-isolate check` tells you which
boot parameters you are missing.

### The third axis: `N`

The two-cause table above is necessary but not sufficient, because both causes
scale with the number of threads in the cpuset. Even a perfect cause-2 fix
leaves `N` calls to `synchronize_rcu_expedited()`, and `lru-isolate` at its best
still loses the `(1-p)^N` race at RT thread counts.

**Do not deploy this tool on a cpuset holding hundreds of threads and expect it
to hold.** It is honest at `N` in the single digits. Above that, shrink `N` or
fix the kernel.

## What actually fixes this

In the order you should try them.

**1. Stop writing `cpuset.mems` on a live slice.** No reboot, no patch, and it
removes the amplification rather than racing it. `N` is the thread count *at
write time*, so set `AllowedMemoryNodes=` when the slice is created, before the
RT daemon starts. An empty cpuset has nothing to migrate and the stall has no
fuel. If something in your tooling retunes NUMA on a running slice, removing
that is worth more than everything else on this page.

**2. Boot with `rcupdate.rcu_normal=1`.** Required for cause 1 regardless of
anything else, and `N` expedited grace periods is exactly the workload it
protects against. Not runtime-writable — `module_param(rcu_normal, int, 0444)`.

**3. Add the `domain` flag: `isolcpus=domain,nohz,<list>`.** The
upstream-supported configuration. It also fixes the `kthread_fetch_affinity()`
regression (commit 041ee6f3727a) that lets unbound kworkers land on isolated
CPUs and dirty their batches — i.e. it lowers `p` as well as helping cause 1.

**4. Kernel patch #3 from the bug.** The only actual guarantee: a
`lru_batching_disabled()` helper that stops isolated CPUs from batching at all,
so `cpu_needs_drain()` can never be true and the race has no window.

**Where `lru-isolate` still earns its place:** `check` reports which boot
parameters you are missing, and `lru-verify --decay` measures the real `p` on
your hardware — the number that decides whether any of this is viable for you.
Use it as instrumentation first and a mitigation second.

### Residual: `has_bh_in_lru()`

`lru_add_drain()` does not clear the buffer-head LRU, the one remaining
`cpu_needs_drain()` term. An isolated CPU that does no buffered filesystem
metadata I/O will have an empty `bh_lru`, but stale entries from before the RT
thread was pinned persist indefinitely, and that alone is enough to keep the CPU
in the flush set.

`invalidate_bh_lrus()` is `on_each_cpu_cond(...)` — an **IPI**, so it reaches a
FIFO-saturated CPU without needing to be scheduled there. `--bh-flush DEV`
triggers it via `BLKFLSBUF`. It also drops `DEV`'s page cache, so point it at a
scratch loop device, not your root disk:

```bash
truncate -s 1M /var/lib/lru-isolate/scratch.img
losetup -f --show /var/lib/lru-isolate/scratch.img     # -> /dev/loop7
lru-isolate arm --bh-flush /dev/loop7 -- systemctl set-property my.slice AllowedMemoryNodes=0
```

---

## Usage

```bash
make
sudo make install          # /usr/local/bin + systemd unit
```

### Am I exposed?

```console
# lru-isolate check
kernel        : 7.0.0-1013.13-aws
isolated CPUs : nohz_full=18-95,108-178, isolated=18-95,108-178
isolcpus      : nohz,18-95,108-178  [domain flag: MISSING]
rcu_normal    : 0

cause 1  synchronize_rcu_expedited() stall : EXPOSED
         fix: boot with rcupdate.rcu_normal=1 (the module_param
              is 0444 - not runtime writable, reboot required),
              or use isolcpus=domain,nohz,<list>.
         lru-isolate CANNOT mitigate this one.
cause 2  __lru_add_drain_all() flush stall  : EXPOSED
         148 isolated CPU(s) currently carry SCHED_FIFO/RR tasks.
         fix: lru-isolate arm -- <your cpuset write>
```

### Arm a cpuset write (the main use)

Drains every isolated CPU, runs your command, and **keeps draining until the
migration the command triggered has finished** — because that migration runs
asynchronously, after the write returns (see [The write is not where the
migration happens](#the-write-is-not-where-the-migration-happens)).

```bash
lru-isolate arm -- systemctl set-property my.slice AllowedMemoryNodes=0
lru-isolate arm -- sh -c 'echo 0 > /sys/fs/cgroup/my.slice/cpuset.mems'
```

`arm` drains every `--interval` ms (default **10** for `arm`, vs 1000 for
`guard`) and stops once `cpuset_migrate_mm_wq` has been idle for 2 s, detected
by looking for `cpuset_migrate_mm_workfn` on the stack of any `kworker/u*`.
`--settle MS` caps how long it will keep draining after the command exits
(default 300000; `0` = no cap). It exits with the command's exit status, so it
stays usable in a script.

If it hits the cap while the migration is still running it says so — that means
the migration outlived the drain, and you want `guard` running independently
rather than a longer `arm`.

Reading `kworker` stacks needs root and `CONFIG_STACKTRACE`. Without them `arm`
cannot see the workqueue, says so, and falls back to draining for the full
`--settle` window.

#### The interval is a jitter trade, and 10 ms is not free

A pass costs each isolated core two context switches and one `madvise()`, and on
`nohz_full` it also drops the core out of tickless mode and back. At the 10 ms
default that is ~100 passes per second per core, sustained for the whole
migration — which is a very different jitter profile from the old
drain-once-and-exec behaviour, and potentially minutes long.

That default is chosen for coverage, not for your latency budget. Measure it
with `-v` (it reports per-core dispatch and drain latency) and widen
`--interval` until the jitter fits, accepting that a wider interval leaves more
of the migration uncovered. The honest position is that this is a coverage/jitter
dial with no free setting — the only configuration with neither cost is kernel
patch #3 from the bug.

### Operating model — how this holds up continually

Polling on a timer is **not** the correctness argument, and `guard` alone is not
a mitigation. The model is three layers, and only the middle one is tight.

#### 1. Make "drained" a stable state

This is the real answer. A drained isolated CPU stays drained *only* if nothing
refills its batches. Then you are not polling against a leak — you are holding a
state nothing perturbs, and the guard interval stops mattering. Four refill
sources, in rough order of how often they bite:

| Source | Reaches the batch via | Remove it by |
|---|---|---|
| The RT thread faulting pages | `folio_add_lru_vma()` → `lru_add` | pre-fault + `mlock` **from a housekeeping CPU** (mlock has its own per-CPU batch — `need_mlock_drain()` — so mlocking *on* the isolated CPU dirties it), then drain once |
| Writeback completion | `folio_rotate_reclaimable()` → `lru_move_tail`, **in IRQ context** | `irqaffinity=<housekeeping>` plus `/proc/irq/*/smp_affinity`; this one needs nothing at all from the spinner |
| Unbound kworkers landing on the isolated CPU | whatever page cache they touch | `/sys/devices/virtual/workqueue/cpumask` → housekeeping CPUs only |
| Stale buffer heads | `has_bh_in_lru()` | `--bh-flush` (IPI-based; see above). `lru_add_drain()` cannot clear this one |

The third row is the bug's own regression: per LP#2165410, commit 041ee6f3727a
made `kthread_fetch_affinity()` filter only against `HK_TYPE_DOMAIN`, which is
every CPU when `isolcpus=nohz` lacks the `domain` flag. So on an affected kernel
unbound kthreads *do* land on your isolated CPUs.

One thing that does **not** work: restricting the workqueue cpumask cannot move
the drain work off the isolated CPU. `mm_percpu_wq` is allocated
`WQ_MEM_RECLAIM | WQ_PERCPU` (`mm/vmstat.c:2269`) and `queue_work_on()` pins to
the named CPU. That knob reduces refill *sources*; it does not relocate the
flush.

#### 2. `arm` the write — the layer you actually rely on

```bash
lru-isolate arm -- systemctl set-property my.slice AllowedMemoryNodes=0
```

Drain, fork the command, and keep draining at a 10 ms interval until the
migration workqueue goes idle. The point is not the gap before the write — it is
that every one of the serialized `cpu_needs_drain()` reads the migration makes,
across its whole duration, lands inside a covered interval.

An earlier version of this tool drained once and `exec`ed, on the assumption
that the write reached `do_migrate_pages()` synchronously. It does not, so the
drain threads were gone before the first work item was picked up and `arm`
did nothing at all.

#### 3. `guard` — insurance for writes you do not control

```bash
systemctl enable --now lru-isolate-guard
```

For `cpuset.mems` writes you cannot wrap, because systemd issues them itself.
With layer 1 in place the interval barely matters, because nothing refills;
without layer 1 the guard is a probability reduction, not a fix. Edit
`CPUAffinity=` in the unit to keep the guard's own main loop on housekeeping
CPUs.

#### The limit

Even armed, the window is nonzero, and it is nonzero once per thread in the
cpuset — see [One migration per thread](#one-migration-per-thread-not-per-process).
**This is a probabilistic mitigation whose odds decay geometrically with the
thread count, not a guarantee.** The guarantee is kernel patch #3 from the bug — a
`lru_batching_disabled()` helper that skips LRU batching on isolated CPUs
entirely, so the batch can never become non-empty and the race has no window at
all. This tool buys you the time until that ships.

#### Measure it rather than trust the model

The number that tells you whether layer 1 is working is the **re-dirty rate** on
your isolated CPUs: drain, wait, trigger, see whether the CPU reappears in the
flush set.

```bash
make decay                      # sweeps 0,1,10,100,1000 ms on a default CPU
lru-verify --decay --cpu 42 --decay-ms 0,10,100,1000,5000
```

It drains the CPU, waits, then triggers, and reports how often the CPU is back
in the flush set — one line per delay, `re-dirtied in N/trials`. **No numbers
are quoted here because this has not been run on isolated hardware yet; the
curve is the thing you have to measure on your own fleet.**

A rate that stays near 0% as the delay grows means the drained state is stable
and `guard` is holding it rather than racing. A rate that climbs means a refill
source is still live — work the table above until it stops climbing, then pick
the guard interval. Do not trust `guard` at any interval before this curve is
flat.

### Priority

To preempt a `SCHED_FIFO/80` spinner the drain thread must run above it.
`--rtprio auto` (default) scans **userspace** `SCHED_FIFO`/`RR` threads per CPU
and picks the highest + 1; kernel threads are excluded, because `migration/N`
and the per-CPU RCU kthreads sit at FIFO 99 on every CPU and would otherwise
push every drain to 99 for no reason. It scans `/proc/<pid>/task/<tid>`, not
just thread-group leaders — a busy-poll spinner is usually a thread inside a
larger process.

Use `--rtprio N` to pin it, or `--rtprio none` to stay `SCHED_OTHER` (which will
not run at all on a saturated CPU). `--serial` staggers the drains one CPU at a
time instead of hitting every isolated CPU in the same instant.

### What it costs an isolated core

A pass costs an isolated core **two context switches and one `madvise()`** —
nothing else. Getting there took four deliberate choices, because the naive
implementation perturbs and even *dirties* the cores it is meant to protect.

**Drain threads are created once and parked on a futex.** Spawning a thread per
pass puts a `clone()`/`exit()` on a latency-critical core every interval. The
threads are created at startup, pinned, given their priority, and then sleep in
`FUTEX_WAIT` between passes.

**`mlockall(MCL_CURRENT|MCL_FUTURE)` runs before any drain thread exists.** This
one is a correctness bug, not just jitter. `pthread_create()` mmaps the thread
stack but does not populate it, so the new thread's first stack write faults *on
the isolated CPU* — and `handle_mm_fault()` → `folio_add_lru_vma()` adds to the
very `lru_add` batch we came to empty. The `madvise` that follows happens to
clean up after itself, but anything faulting *after* the drain re-dirties the
batch outright and silently puts the CPU back in the flush set. Locking first
moves that population to the housekeeping CPU that calls `pthread_create()`.

This is also why the drain is `MADV_WILLNEED` and not `MADV_COLD`: under
`mlockall()` every VMA is `VM_LOCKED`, and `can_madv_lru_vma()` would reject
`MADV_COLD` with `-EINVAL` *before* it reaches `lru_add_drain()`. The two
choices have to be made together.

**The main loop pins itself to the complement of the target set**, so only the
pinned drain threads ever execute on an isolated core.

**Target CPUs are checked against the process's own affinity mask first**, and
the two ways they can go missing are not the same problem. `CPUAffinity=` — in
the unit, in `/etc/systemd/system.conf`, or a `taskset` wrapper — is a plain
`sched_setaffinity()` restriction, and a task can widen its own mask straight
back out of it with no capability required; `lru-isolate` does exactly that and
carries on. `AllowedCPUs=` is a cpuset, which is a hard ceiling it cannot
escape, and that is reported as an error naming the unit and its parent slice.

This matters on precisely the hosts the mitigation is for: a manager-wide
`CPUAffinity=` in `system.conf` pinning services to housekeeping cores is
standard low-latency tuning, it is inherited by every unit including
`lru-isolate-guard`, and before this the guard unit would fail at startup with
every target CPU unreachable — the isolation that makes the mitigation
necessary was the same thing locking it out.

Two costs are inherent and cannot be engineered away from userspace: the futex
wake needs a rescheduling IPI to the isolated core, and on `nohz_full` that core
leaves tick-less mode while the drain thread is runnable, then re-enters it.

`-v` reports measured dispatch and drain latency per core, so you can put a real
number on this for your latency budget rather than taking the 8–30 µs measured
here.

### Large machines

CPU sets are allocated with `CPU_ALLOC`. The fixed `cpu_set_t` holds 1024 CPUs
and `CPU_SET()` past that corrupts memory — the bug's own hardware is 192 vCPUs,
but `--cpus` is user-supplied and the limit is not enforced by the type.

### Shrink the migration itself (optional, secondary)

`premigrate` reclaims a task's large anonymous VMAs via `process_madvise(2)`
before the cpuset write, so `do_migrate_pages()` has fewer resident pages to
walk and move. This shortens the migration; it does **not** address either
stall.

```bash
premigrate --pid $(pgrep -f my-trading-app) --pageout
premigrate --pid 12345 --dry-run          # show what is eligible
```

Honest caveat: `MADV_PAGEOUT` only lowers RSS if the pages can go somewhere —
i.e. swap is configured. On a swapless host it degrades to roughly `MADV_COLD`.

---

## Files

| Path | What |
|---|---|
| `src/lru-isolate.c` | the mitigation: `check` / `drain` / `guard` / `arm` |
| `src/lru-verify.c` | proof + measurement: `make verify` (baseline vs mitigated), `make decay` (re-dirty rate) |
| `src/premigrate.c` | `process_madvise(2)` volume reducer |
| `src/rt-spinner.c` | self-terminating SCHED_FIFO test load; built, deliberately **not** installed |
| `systemd/lru-isolate-guard.service` | continuous drain |
| `TESTING.md` | staged deploy + test plan, and the 2×2 acceptance matrix |

Everything is C; nothing forks inside a measurement window.

## TODO

- A `sources` subcommand to audit the four refill sources (IRQ affinity,
  unbound workqueue cpumask, RT-thread `mlock` state, `bh_lru`) and report
  which are still live on each isolated CPU.
- Run `lru-verify --decay` on real isolated hardware and record the curve here.
- `src/lru-probe.c` and `scripts/verify-mitigation.sh` are superseded stubs;
  delete once nothing references them.

## Verified against

linux-aws 7.0 source at `/opt/pid-1-bug/linux-aws-7.0-src/linux-aws-7.0-verified`
(`mm/swap.c`, `mm/madvise.c`, `mm/mempolicy.c`, `mm/migrate.c`, `mm/mlock.c`,
`kernel/rcu/tree_exp.h`, `kernel/rcu/update.c`). Runtime measurements on
6.17.0-35-generic; both kernels are `CONFIG_RT_GROUP_SCHED=n`,
`CONFIG_SWAP=y`, `CONFIG_PREEMPT_LAZY=y`.

Related: `/opt/debug` reproduces the PID 1 / PID 2 `D`-state signature;
`/opt/pid-1-bug` holds the kernel trees.

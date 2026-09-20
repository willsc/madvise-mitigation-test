# Deploying and testing `lru-isolate`

## Read this first: the 2×2 or you will draw the wrong conclusion

LP#2165410 has separate RCU and LRU-worker blocking paths. The madvise tool
reduces opportunities to queue LRU drain work; it does not guarantee progress
once that work has been queued. A delayed hang is a failed mitigation run and
a useful diagnostic observation, not proof that either cause is fixed.

| `rcupdate.rcu_normal` | `lru-isolate` | what to investigate |
|---|---|---|
| `0` | off | baseline RCU and LRU-worker blocking |
| `0` | on | RCU blocking remains; LRU queueing can still race the drain |
| `1` | off | normal RCU grace periods; LRU-worker starvation remains |
| `1` | on | candidate combination; still requires repeated workload tests |

Capture the blocked stack in every failing run. Do not identify the remaining
cause from the matrix alone. `rcu_normal=1` selects normal RCU grace periods;
it does not make all RCU waits impossible.

Run at the real thread count and CPU layout. `update_tasks_nodemask()` walks
threads and can queue migration for their shared address space repeatedly.
This provides more queueing opportunities, but the trials are not known to be
independent. The desktop 1/60 result is not a fleet failure probability.

`rcu_normal` is `module_param(..., 0444)` — **not runtime writable**. It is a
boot parameter, so each row that changes it costs a reboot. Plan the matrix as
two boots, not four.

---

## Stage 0 — build

Build the binaries and run the controller regression tests locally:

```bash
cd /opt/madvise-mitigation-test
make
make test
```

The regression tests mock RT scheduling; they do not run a FIFO spinner or
change cpusets. They cover late workload startup, priority increases, failed
drains, permission failures and timeouts. Then run the smoke test:

```bash
./lru-isolate --help
./lru-isolate check          # on a box with no isolated CPUs: "not exposed"
```

## Stage 1 — mechanism, on any machine

Safe on a live box: no `SCHED_FIFO` load, no NUMA, no cpuset write. It proves
the madvise primitive removes a CPU from the flush set, nothing more.

```bash
sudo make verify             # baseline vs mitigated
sudo make decay              # how long a drained CPU stays clean
sudo ./lru-isolate drain --cpus 13,14,15 -v   # dispatch + drain latency
```

`verify` should print a large baseline and a near-zero mitigated count. On a
busy general-purpose CPU a few percent residual is expected — that is
background activity re-dirtying the batch, and `decay` is what quantifies it.

**This stage cannot reproduce the bug.** No isolated CPUs, no FIFO starvation,
no stall. It only tells you the lever works.

## Stage 2 — isolated cores in a throwaway VM

The first stage that can actually stall. Use the KVM harness in `/opt/debug`
(`run-in-vm.sh`) or any disposable guest, booted with:

```
isolcpus=nohz,2-3 nohz_full=2-3 rcu_nocbs=2-3
```

Note: **no `domain` flag** — that is the configuration the bug is about. Adding
`domain` is itself one of the workarounds and will mask cause 1.

```bash
# inside the guest
sudo ./lru-isolate check        # should now report isolated CPUs and EXPOSED
sudo ./lru-verify --cpu 2 --trigger-cpu 0
sudo ./lru-verify --decay --cpu 2 --decay-ms 0,10,100,1000,5000
```

That decay curve is the number that decides whether `guard` is viable for you.
Flat near 0% means the drained state is stable. Climbing means a refill source
is still live — see "Operating model" in the README.

### The stall test

`rt-spinner` is the FIFO load, built but deliberately not installed. It
self-terminates on a per-thread `CLOCK_MONOTONIC` deadline, because with
`CONFIG_RT_GROUP_SCHED=n` there is no RT throttling and an unbounded FIFO
spinner starves the per-CPU kworker until you power-cycle.

```bash
# terminal 1 - saturate the isolated CPUs for 60s
sudo ./rt-spinner --cpus 2-3 --seconds 60

# terminal 2 - large-RSS process in a slice, then move it between NUMA nodes
sudo systemd-run --slice=test.slice --unit=hog \
     stress-ng --vm 1 --vm-bytes 4G --vm-hang 0
time sudo systemctl set-property test.slice AllowedMemoryNodes=0
```

Watch PID 1 from a shell that is already running — `ps` forks and will hang:

```bash
read -r _ _ s1 _ < /proc/1/stat; echo "pid1 state=$s1"    # D is the signature
```

Then repeat armed:

```bash
time sudo ./lru-isolate arm -- systemctl set-property test.slice AllowedMemoryNodes=0
```

Note what `time` measures here. `arm` does **not** return when `systemctl`
returns: the migration runs on `cpuset_migrate_mm_wq`, and the writer flushes the queue
from task_work before returning to userspace. `arm` drains while the command
runs and continues until its workqueue sampling has seen 2 s of quiet. The
elapsed time is therefore roughly *migration duration + 2 s*, and that is the
number you want — it is how long the stall would have had to be covered for.
The thing to compare against the unarmed run is PID 1's state, not the
wall clock.

Add `-v` to see the drain-pass count and confirm it kept draining across the
whole migration rather than exiting early:

```bash
sudo ./lru-isolate arm -v -- systemctl set-property test.slice AllowedMemoryNodes=0
# ... armed: 4127 drain passes over 41.3 s (interval 10 ms)
```

If it prints `warning: cpuset_migrate_mm_wq still busy at the --settle cap`,
the migration outlived the drain window — raise `--settle` or run `guard`
alongside.

Compare against the matrix above and classify failures by their captured
stacks. Even with the guard running, an LRU drain race can still cause a stall.

### Prove the batch state is what matters

`rt-spinner --clean` pre-faults and `mlock`s from the housekeeping CPU before
pinning, so the isolated CPU's batches stay empty. A `--clean` spinner should
**not** stall the cpuset write even with `lru-isolate` switched off:

```bash
sudo ./rt-spinner --cpus 2-3 --seconds 60 --clean
time sudo systemctl set-property test.slice AllowedMemoryNodes=0   # no stall
```

That isolates the mechanism precisely: FIFO saturation alone is not sufficient;
`cpu_needs_drain()` being true is what puts the CPU in the flush set. It is also
the "eliminate the source" half of the mitigation, demonstrated.

## Stage 3 — AWS metal, the faithful environment

Only metal on `linux-aws` 7.0 reproduces the reported timings, and only metal
matches the no-hypervisor condition. `/opt/debug/aws-metal-repro.sh` already
does the provisioning; reuse it rather than writing new launch tooling.

```bash
cd /opt/debug
./aws-metal-repro.sh up
./aws-metal-repro.sh serial     # OPEN THIS FIRST - SSH dies if you wedge it
./aws-metal-repro.sh ssh
# copy /opt/madvise-mitigation across, then repeat Stage 2 with the real
# isolation layout: isolcpus=nohz,18-95,108-178 nohz_full=... rcu_nocbs=...
./aws-metal-repro.sh down       # metal is ~$4/hr
```

Record on real hardware, per row of the 2×2:

- wall time of the `cpuset.mems` write
- longest observed PID 1 `D`-state duration
- `lru-verify --decay` curve on a genuinely isolated core
- `lru-isolate drain -v` dispatch/drain latency, for the RT latency budget

The decay curve from here is the one worth putting in the README.

## Stage 4 — production rollout

```bash
sudo make install       # lru-isolate, lru-verify, premigrate + the unit
```

**Both halves go in the same maintenance window**, because one of them is a
boot parameter:

1. Add `rcupdate.rcu_normal=1` to the kernel command line and reboot.
   (Or switch to `isolcpus=domain,nohz,<list>`, which is upstream's supported
   configuration and fixes cause 1 a different way.)
2. Confirm with `lru-isolate check` — cause 1 should read `mitigated`.
3. Deploy the cause-2 half, below.

### Which writes you can arm, and which you cannot

This is the real deployment decision.

**You control the write** — you run `systemctl set-property`, or a script does.
Wrap it to keep draining throughout the command. This still has a race with
kernel work queueing:

```bash
lru-isolate arm -- systemctl set-property my.slice AllowedMemoryNodes=0
```

**systemd issues the write itself** — during unit start, daemon-reload, or when
applying `AllowedMemoryNodes=` from a unit file. You cannot wrap PID 1. Options,
in order of preference:

1. Remove the refill sources so the isolated cores never enter the flush set
   (README "Operating model"). This is the only approach that does not depend
   on winning a race.
2. Set `AllowedMemoryNodes=` statically in the unit rather than changing it at
   runtime, so the write happens at unit start before the RT threads are pinned
   and the batches are dirty.
3. `systemctl enable --now lru-isolate-guard`, and size the interval from your
   measured decay curve. Probability reduction, not a fix.

### Verify in production

```bash
lru-isolate check               # configuration snapshot, not proof of coverage
systemctl status lru-isolate-guard
lru-isolate drain -v            # dispatch/drain latency on live isolated cores
```

The guard needs `CAP_SYS_NICE` (to preempt the spinners) and `CAP_IPC_LOCK`
(for `mlockall`, which must succeed before the drain threads are created — see
the README). If `check` reports isolated CPUs as **unreachable**, a cpuset is
excluding them: look at the unit's `AllowedCPUs=`/`CPUAffinity=`, or a `taskset`
wrapper.

### Rollback

```bash
systemctl disable --now lru-isolate-guard
make uninstall
```

Nothing persists: no kernel module, no sysctl, no boot parameter of its own, and
`lru-verify` removes its kprobe by name on exit and restores `tracing_on`.
Dropping `rcupdate.rcu_normal=1` needs a reboot, as adding it did.

## Diagnose a delayed hang

Capture must be armed **before** the reproducer starts. During the hang,
assume that new SSH logins and commands in existing shells are unavailable.
No step below requires an interactive command during that period. The bundled
`reproducer.tar.xz` contains FIFO/80 workers that continuously refault memory;
`rt-spinner` only dirties its small allocation at startup. They test different
refill patterns. Do not treat a pass with `rt-spinner` as a reproducer pass.

### Start automatic capture before triggering the hang

On the recovered disposable target, from this repository, run:

```bash
sudo -v
sudo nohup python3 scripts/capture-hang.py --out /var/tmp/lru-hang-01 \
    --duration 900 --interval 2 > /var/tmp/lru-hang-01.start.log 2>&1 < /dev/null &
```

Before launching the reproducer, read `/var/tmp/lru-hang-01.start.log` and
wait for `READY`. It reports whether PID 1's stack is readable and whether the
kernel-message reader started. If both are unavailable, it prints `NOT READY`
and exits nonzero. Resolve capture access before running the test; task-state
samples alone cannot identify the wait.
Use a fresh output directory for every run; existing directories are rejected.

The collector pins itself to an allowed CPU outside `nohz_full` and `isolated`.
It starts all its threads before reporting readiness, then reads `/proc` and
`/dev/kmsg` directly. It never invokes `systemctl`, starts subprocesses,
changes RT priorities/sysctls, kills the workload or reboots. A separate thread
reads kernel messages so a blocked task-stack read need not stop that stream.
The bundled reproducer already sets `hung_task_timeout_secs=60`; any reports
the kernel produces can be captured without a new shell or a running journald.

Launch the reproducer as usual **after readiness**, then leave the collector
alone. Its default capture period is 15 minutes from collector startup. After
the host recovers (or reboots), retrieve:

- `/var/tmp/lru-hang-01/samples.jsonl`: boot configuration, timestamps,
  PID 1 and worker stacks, guard-thread CPU time/affinity, and migration counters.
- `/var/tmp/lru-hang-01/kernel.jsonl`: raw kernel records, including hung-task
  and RCU reports if generated and readable.
- `/var/tmp/lru-hang-01.start.log`: readiness and collector errors.

Each JSON record is flushed and fsynced. Use persistent storage: `/tmp` may
be cleared at reboot. A full kernel lockup, unscheduled collector or blocked
storage can still stop capture; absence of later records is **not** proof of
a particular wait. This is not a replacement for a previously configured
kernel crash dump or external kernel-console recording if userspace cannot
make progress. Collection also adds housekeeping CPU and I/O load, so use the
same collection settings in baseline and mitigation runs.

### Compare priority handling only after capture is ready

For a priority comparison, keep the guard interval and all other settings the
same and use a fixed priority above the bundled workers before starting them:

```bash
sudo ./lru-isolate guard --cpus 18-95,108-178 --interval 1000 --rtprio 81 -v
```

This CPU list is specific to the bundled reproducer; adapt it to the target.
Use one guard instance, and replace 1000 with the previous interval if different.
The former auto mode sampled priorities only at startup and could remain at
normal priority when the FIFO workers started later. The updated auto mode
rescans after 100 ms without a response and raises the drainer priority. Fixed
81 removes that recovery delay for the known FIFO/80 workload.

Record the trigger timestamp separately from script startup: the bundled
script already sleeps 5 + 25 seconds before its final memory-node restriction.
Use the saved timestamps and snapshots to inspect PID 1, the migration worker
and guard-thread progress after recovery. CPU-time changes show thread activity,
not proof of successful drain calls. Saved samples also leave gaps, so they
cannot establish an exact maximum D-state duration.

- Passes stop: check timeout/priority errors. A guard process existing is not
  evidence that its drain threads are still running.
- Passes continue and migration waits in `flush_work()` beneath
  `__lru_add_drain_all()`: a local madvise drain cannot complete the already
  queued work item; the kernel worker needs CPU time.
- Migration waits in `synchronize_rcu_expedited()`: investigate the RCU workers
  and boot configuration separately.

Only after this comparison should you change the interval, one variable at a
time. Repeat runs and report time from the trigger to first hang; do not infer
a reliable mitigation from one later failure.

## What would make this unnecessary

A kernel fix must prevent or resolve work queued to a CPU whose worker cannot
run. The proposed batching patch needs validation against all drain conditions,
including mlock and buffer heads, and the RCU path needs separate coverage.

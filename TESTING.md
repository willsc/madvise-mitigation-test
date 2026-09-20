# Deploying and testing `lru-isolate`

## Read this first: the 2×2 or you will draw the wrong conclusion

LP#2165410 has **two independent causes**. `lru-isolate` fixes one of them.
`rcupdate.rcu_normal=1` fixes the other. Test either one alone and the cpuset
write still stalls, and you will conclude the tool does not work.

| `rcupdate.rcu_normal` | `lru-isolate` | cause 1 (RCU expedited) | cause 2 (LRU flush) | expected result |
|---|---|---|---|---|
| `0` | off | stalls | stalls | **full stall** — the bug |
| `0` | on  | stalls | fixed  | **still stalls** ← the trap |
| `1` | off | fixed  | stalls | **still stalls** |
| `1` | on  | fixed  | fixed  | **no stall** |

Only the fourth row is a pass. Run all four: rows 2 and 3 are what prove each
half is actually doing something, and row 2 is the one that gets misread as
"the mitigation is broken".

`rcu_normal` is `module_param(..., 0444)` — **not runtime writable**. It is a
boot parameter, so each row that changes it costs a reboot. Plan the matrix as
two boots, not four.

---

## Stage 0 — build

Nothing here has been compiled yet.

```bash
cd /opt/madvise-mitigation
make
```

Expect to fix compile errors first. Then the no-privilege smoke test:

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

Compare against the 2×2 above. If row 2 still stalls, that is expected and
correct — it is cause 1, and it needs the reboot.

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
Wrap it. This is deterministic and is what you should rely on:

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
lru-isolate check               # expect: both causes mitigated
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

## What would make this unnecessary

Kernel patch #3 from the bug — a `lru_batching_disabled()` helper that skips LRU
batching on isolated CPUs entirely, so the batch can never become non-empty and
there is no race window at all. Everything here is a userspace approximation of
that patch. Track the bug; when the patched kernel ships, this comes out.

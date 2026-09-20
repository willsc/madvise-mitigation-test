#!/usr/bin/env python3
"""Start BEFORE the reproducer; collect evidence without an interactive shell.

Reads /proc and /dev/kmsg directly. Does not contact systemd, run subprocesses,
change scheduling policy/sysctls, stop workloads, or reboot. A kernel-wide
lockup or blocked storage can also stop collection; this is not a crash dump.
"""
import argparse
import errno
import json
import math
import os
from pathlib import Path
import signal
import sys
import threading
import time


def read_text(path):
    try:
        return Path(path).read_text().strip()
    except OSError as exc:
        return {"error": str(exc)}


def cpulist(value):
    if not value or value == "(null)":
        return set()
    result = set()
    for part in value.split(","):
        ends = [int(n) for n in part.split("-")]
        if len(ends) not in (1, 2) or ends[0] < 0 or ends[-1] < ends[0]:
            raise ValueError(f"invalid CPU list: {value}")
        result.update(range(ends[0], ends[-1] + 1))
    return result


def parse_stat(text):
    # comm may contain spaces and closing parentheses.
    left, right = text.index("("), text.rindex(")")
    fields = text[right + 2:].split()
    return {"tid": int(text[:left]), "comm": text[left + 1:right],
            "state": fields[0], "utime_ticks": int(fields[11]),
            "stime_ticks": int(fields[12]), "starttime_ticks": int(fields[19]),
            "cpu": int(fields[36]), "rtprio": int(fields[37]),
            "policy": int(fields[38])}


def stat_at(path):
    value = read_text(path)
    if isinstance(value, dict):
        return None
    try:
        return parse_stat(value)
    except (ValueError, IndexError):
        return None


def relevant(pid, stat):
    return (pid == 1 or stat["state"] == "D" or
            stat["comm"].startswith(("kworker/", "rcu_exp", "rcub/")) or
            stat["comm"] in {"lru-isolate", "repro_worker", "rt-spinner", "sshd"})


def snapshot(emit, proc=Path("/proc")):
    emit("snapshot_start", {})  # persisted even if a subsequent read blocks
    emit("vmstat", {"text": read_text(proc / "vmstat")})
    for entry in sorted(proc.iterdir(), key=lambda p: p.name):
        if not entry.name.isdigit():
            continue
        pid = int(entry.name)
        leader = stat_at(entry / "stat")
        if not leader or not relevant(pid, leader):
            continue
        try:
            threads = list((entry / "task").iterdir())
        except OSError:
            continue  # task may exit during enumeration
        for task in threads:
            stat = leader if task.name == entry.name else stat_at(task / "stat")
            if not stat:
                continue
            record = {"pid": pid, **stat}
            status = read_text(task / "status")
            if isinstance(status, str):
                record["status"] = [line for line in status.splitlines()
                                    if line.startswith(("Cpus_allowed_list:",
                                                        "Mems_allowed_list:",
                                                        "voluntary_ctxt_switches:",
                                                        "nonvoluntary_ctxt_switches:"))]
            else:
                record["status"] = status
            # Write identity first: a stack read can itself stall.
            emit("task", record)
            if (pid == 1 or stat["state"] == "D" or
                    stat["comm"].startswith(("kworker/u", "rcu_exp", "rcub/")) or
                    stat["comm"] == "lru-isolate"):
                emit("stack", {"pid": pid, "tid": stat["tid"],
                               "text": read_text(task / "stack")})
    emit("snapshot_end", {})


class Log:
    def __init__(self, path):
        self.file = path.open("x", buffering=1)

    def emit(self, kind, data):
        record = {"event": kind, "wall_ns": time.time_ns(),
                  "monotonic_ns": time.monotonic_ns(), **data}
        self.file.write(json.dumps(record) + "\n")
        self.file.flush()
        os.fsync(self.file.fileno())

    def close(self):
        self.file.close()


def kernel_reader(fd, log, stop, deadline):
    try:
        while not stop.is_set() and time.monotonic() < deadline:
            try:
                data = os.read(fd, 65536)
            except BlockingIOError:
                stop.wait(0.1)
                continue
            except OSError as exc:
                log.emit("kmsg_error", {"error": str(exc)})
                if exc.errno == errno.EPIPE:  # ring overrun; explicitly record loss
                    continue
                break
            if not data:
                break
            log.emit("kmsg", {"text": data.decode(errors="replace").rstrip()})
    finally:
        os.close(fd)
        log.close()


def positive_seconds(value):
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError("must be a finite positive number")
    return number


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True,
                        help="new directory on persistent storage (not /tmp)")
    parser.add_argument("--duration", type=positive_seconds, default=900)
    parser.add_argument("--interval", type=positive_seconds, default=2)
    args = parser.parse_args()
    try:
        isolated = set()
        cpu_lists = {}
        for name in ("nohz_full", "isolated"):
            value = read_text(Path("/sys/devices/system/cpu") / name)
            if not isinstance(value, str):
                raise ValueError(f"cannot read {name}: {value}")
            cpu_lists[name] = value
            isolated.update(cpulist(value))
        housekeeping = os.sched_getaffinity(0) - isolated
        if not housekeeping:
            raise ValueError("no allowed housekeeping CPU outside the isolation lists")
        os.sched_setaffinity(0, {min(housekeeping)})
        args.out.mkdir(mode=0o700, parents=True, exist_ok=False)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))

    samples = Log(args.out / "samples.jsonl")
    kernel = Log(args.out / "kernel.jsonl")
    stop = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    deadline = time.monotonic() + args.duration
    samples.emit("metadata", {"pid": os.getpid(), "kernel": os.uname().release,
                              "cmdline": read_text("/proc/cmdline"),
                              "cpu_lists": cpu_lists,
                              "collector_cpus": sorted(os.sched_getaffinity(0)),
                              "clock_ticks": os.sysconf("SC_CLK_TCK"),
                              "duration_seconds": args.duration,
                              "interval_seconds": args.interval,
                              "hung_task_timeout_secs": read_text(
                                  "/proc/sys/kernel/hung_task_timeout_secs"),
                              "rcu_normal": read_text(
                                  "/sys/module/rcupdate/parameters/rcu_normal")})
    kernel_thread = None
    try:
        fd = os.open("/dev/kmsg", os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
    except OSError as exc:
        kernel.emit("kmsg_unavailable", {"error": str(exc)})
        kernel.close()
        print(f"WARNING: kernel messages unavailable: {exc}", flush=True)
    else:
        kernel_thread = threading.Thread(target=kernel_reader,
                                         args=(fd, kernel, stop, deadline), daemon=True)
        kernel_thread.start()
    try:
        snapshot(samples.emit)
        stack = read_text("/proc/1/stack")
        kernel_available = kernel_thread is not None and kernel_thread.is_alive()
        if not isinstance(stack, str) and not kernel_available:
            samples.emit("capture_unavailable", {"pid1_stack": stack,
                                                  "kernel_reader_alive": False})
            print("NOT READY: cannot read PID 1's stack or kernel messages. "
                  "Resolve capture access before starting the reproducer.",
                  file=sys.stderr, flush=True)
            return 1
        if time.monotonic() >= deadline:
            samples.emit("expired_before_ready", {})
            print("NOT READY: capture duration expired during baseline collection; "
                  "use a longer duration.", file=sys.stderr, flush=True)
            return 1
        samples.emit("ready", {"pid1_stack_readable": isinstance(stack, str),
                               "kernel_reader_alive": kernel_available})
        print(f"READY: {args.out.resolve()} (PID 1 stack readable: "
              f"{isinstance(stack, str)}; kernel reader started: "
              f"{kernel_available}). Start the reproducer now.", flush=True)
        while not stop.wait(min(args.interval, max(0, deadline - time.monotonic()))):
            if time.monotonic() >= deadline:
                break
            snapshot(samples.emit)
        samples.emit("finished", {})
    finally:
        stop.set()
        samples.close()
        if kernel_thread:
            kernel_thread.join(timeout=2)


if __name__ == "__main__":
    sys.exit(main())

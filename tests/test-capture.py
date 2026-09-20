#!/usr/bin/env python3
"""Read-only fixture tests; no reproducer, kernel settings or RT policy changes."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "capture", Path(__file__).resolve().parents[1] / "scripts/capture-hang.py")
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)


def stat(pid, comm, state="S"):
    fields = ["0"] * 39
    fields[0] = state
    fields[11], fields[12], fields[19] = "11", "12", "123456"
    fields[36], fields[37], fields[38] = "3", "80", "1"
    return f"{pid} ({comm}) " + " ".join(fields)


def task(root, pid, comm, state="S", tid=None):
    proc = root / str(pid)
    proc.mkdir(exist_ok=True)
    if tid is None:
        tid = pid
        (proc / "stat").write_text(stat(pid, comm, state))
    thread = proc / "task" / str(tid)
    thread.mkdir(parents=True)
    (thread / "stat").write_text(stat(tid, comm, state))
    (thread / "status").write_text("Cpus_allowed_list:\t3\nvoluntary_ctxt_switches:\t7\n")
    (thread / "stack").write_text("[<0>] flush_work+0x0/0x10\n")


class CaptureTests(unittest.TestCase):
    def test_stat_and_cpu_lists(self):
        parsed = capture.parse_stat(stat(42, "worker ) with spaces", "D"))
        self.assertEqual(parsed["comm"], "worker ) with spaces")
        self.assertEqual(parsed["state"], "D")
        self.assertEqual(parsed["cpu"], 3)
        self.assertEqual(parsed["rtprio"], 80)
        self.assertEqual(parsed["starttime_ticks"], 123456)
        self.assertEqual(capture.cpulist("2-4,7"), {2, 3, 4, 7})
        self.assertEqual(capture.cpulist("(null)"), set())
        with self.assertRaises(ValueError):
            capture.cpulist("4-2")

    def test_blocking_paths_and_guard_threads(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "vmstat").write_text("pgmigrate_success 10\n")
            task(root, 1, "systemd", "D")
            task(root, 2, "kworker/u2:0", "D")
            task(root, 3, "rcu_exp_par_gp_k")
            task(root, 4, "lru-isolate")
            task(root, 4, "lru-isolate", tid=44)
            task(root, 5, "unrelated")
            task(root, 6, "some-blocked-app", "D")
            (root / "4/task/44/stack").unlink()  # access failure is evidence
            (root / "99").mkdir()  # exited task must not abort the capture
            records = []
            capture.snapshot(lambda kind, data: records.append((kind, data)), root)
            tids = {data["tid"] for kind, data in records if kind == "task"}
            self.assertEqual(tids, {1, 2, 3, 4, 44, 6})
            stacks = {data["tid"]: data["text"]
                      for kind, data in records if kind == "stack"}
            self.assertIn("flush_work", stacks[2])
            self.assertIn("error", stacks[44])
            self.assertEqual(records[0][0], "snapshot_start")
            self.assertEqual(records[-1][0], "snapshot_end")
            # Identity is emitted before attempting the potentially blocking read.
            identity = next(i for i, (kind, data) in enumerate(records)
                            if kind == "task" and data["tid"] == 2)
            self.assertEqual(records[identity + 1][0], "stack")

    def test_log_preserves_completed_records(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "samples.jsonl"
            log = capture.Log(path)
            log.emit("task", {"tid": 1})
            self.assertIn('"tid": 1', path.read_text())
            log.close()
            with self.assertRaises(FileExistsError):
                capture.Log(path)

    def test_missing_evidence_sources_prevents_readiness(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "capture"
            real_read = capture.read_text

            def inaccessible(path):
                if str(path) == "/proc/1/stack":
                    return {"error": "permission denied"}
                return real_read(path)

            with patch("sys.argv", ["capture-hang.py", "--out", str(output)]), \
                    patch.object(capture, "read_text", side_effect=inaccessible), \
                    patch.object(capture.os, "open", side_effect=PermissionError("kmsg denied")), \
                    patch.object(capture, "snapshot"), \
                    patch.object(capture.os, "sched_setaffinity"):
                self.assertEqual(capture.main(), 1)
            text = (output / "samples.jsonl").read_text()
            self.assertIn('"event": "capture_unavailable"', text)
            self.assertNotIn('"event": "ready"', text)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Real CLI checks using normal scheduling, including an injected drain failure."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())
preload = str(Path(sys.argv[2]).resolve())
cpu = str(min(os.sched_getaffinity(0)))
with tempfile.TemporaryDirectory(prefix="lru-cli-test-") as temp:
    marker = Path(temp) / "launched"
    args = [binary, "arm", "--cpus", cpu, "--rtprio", "none", "--settle", "1",
            "--", "/usr/bin/touch", str(marker)]
    good = subprocess.run(args, capture_output=True, text=True, timeout=15)
    assert good.returncode == 0 and marker.exists(), good.stdout + good.stderr
    marker.unlink()
    env = dict(os.environ, LD_PRELOAD=preload)
    bad = subprocess.run(args, env=env, capture_output=True, text=True, timeout=15)
    assert bad.returncode == 1 and not marker.exists(), bad.stdout + bad.stderr
    assert "madvise drain failed" in bad.stderr, bad.stdout + bad.stderr
    print("PASS: successful arm launches command; failed initial drain prevents launch")
    env["LRU_TEST_FAIL_AFTER"] = "1"
    for command in ("guard", "arm"):
        ongoing = [binary, command, "--cpus", cpu, "--rtprio", "none",
                   "--interval", "1"]
        if command == "arm":
            ongoing += ["--", "/usr/bin/true"]
        result = subprocess.run(ongoing, env=env, capture_output=True,
                                text=True, timeout=15)
        assert result.returncode == 1, result.stdout + result.stderr
        assert "madvise drain failed" in result.stderr, result.stdout + result.stderr
        if command == "arm":
            assert "migration completion is unknown" in result.stderr, result.stderr
    print("PASS: guard and arm report a drain failure after an earlier successful pass")

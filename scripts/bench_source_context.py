#!/usr/bin/env python3
"""Compare two Release component probes on identical histories (Linux, no models)."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
import platform
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def digest(path):
    with path.open("rb") as file:
        return hashlib.file_digest(file, "sha256").hexdigest()


def wait_probe(pid):
    deadline = time.monotonic() + 60
    try:
        while True:
            observed, status, usage = os.wait4(pid, os.WNOHANG)
            if observed:
                return status, usage
            if time.monotonic() >= deadline:
                raise TimeoutError("component probe exceeded 60 seconds")
            time.sleep(0.005)
    except BaseException:
        try:
            os.killpg(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            os.wait4(pid, 0)
        except ChildProcessError:
            pass
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", required=True, type=Path)
    parser.add_argument("--after", required=True, type=Path)
    parser.add_argument("--events", type=int, default=2048)
    parser.add_argument("--bytes", type=int, default=65536)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if sys.platform != "linux" or not 1 <= args.repeats <= 30:
        parser.error("Linux and 1..30 repeats are required")
    if not 4096 <= args.bytes <= 1024**2 or not 1 <= args.events <= 256 * 1024**2 // args.bytes:
        parser.error("fixture must fit 256 MiB, with events between 4 KiB and 1 MiB")
    binaries = {name: getattr(args, name).resolve(strict=True) for name in ("before", "after")}
    if args.output.exists():
        parser.error("output already exists")
    rows, expected = [], None
    with tempfile.TemporaryDirectory(prefix="asper-context-bench-") as temp:
        root = Path(temp)
        fixture = root / "fixture"
        created = subprocess.run([str(binaries["after"]), "create", str(fixture), str(args.events),
                                  str(args.bytes)], capture_output=True, timeout=180)
        if created.returncode:
            parser.exit(1, "Fixture failed: " + created.stderr.decode(errors="replace")[-4096:] + "\n")
        for repeat in range(args.repeats):
            for name in (("before", "after") if repeat % 2 == 0 else ("after", "before")):
                run = root / "run"
                shutil.copytree(fixture, run)
                stdout, stderr = root / "stdout", root / "stderr"
                actions = [(os.POSIX_SPAWN_OPEN, fd, str(path), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
                           for fd, path in ((1, stdout), (2, stderr))]
                started = time.perf_counter()
                pid = os.posix_spawn(str(binaries[name]), [str(binaries[name]), "measure", str(run)],
                                     os.environ, file_actions=actions, setpgroup=0)
                try:
                    status, usage = wait_probe(pid)
                except TimeoutError as error:
                    parser.exit(1, str(error) + "\n")
                elapsed = time.perf_counter() - started
                if os.waitstatus_to_exitcode(status):
                    parser.exit(1, "Probe failed: " + stderr.read_text(errors="replace")[-4096:] + "\n")
                output_hash = digest(stdout)
                if expected is None:
                    expected = output_hash
                if output_hash != expected:
                    parser.exit(1, "Selected context differs; timings cannot stand in for equivalent output.\n")
                rows.append({"variant": name, "repeat": repeat + 1, "wall_seconds": elapsed,
                             "max_rss_kib": usage.ru_maxrss, "user_seconds": usage.ru_utime,
                             "system_seconds": usage.ru_stime})
                shutil.rmtree(run)
    result = {"schema": 1, "created_at_utc": datetime.now(timezone.utc).isoformat(),
              "profile": platform.platform(), "method":
              "Open store, materialize history, close; warm copied fixture, alternating order, wait4 process peak RSS including launch/loader floor. No model generation.",
              "events": args.events + 9, "large_event_bytes": args.bytes,
              "binaries": {name: {"path": str(path), "sha256": digest(path)} for name, path in binaries.items()},
              "identical_output_sha256": expected, "runs": rows, "summary": {}}
    for name in binaries:
        selected = [row for row in rows if row["variant"] == name]
        result["summary"][name] = {
            key: {"median": statistics.median(row[key] for row in selected),
                  "min": min(row[key] for row in selected), "max": max(row[key] for row in selected)}
            for key in ("wall_seconds", "max_rss_kib")}
    with args.output.open("x") as output:
        json.dump(result, output, indent=2); output.write("\n")
    print(json.dumps(result["summary"], indent=2))


if __name__ == "__main__":
    main()

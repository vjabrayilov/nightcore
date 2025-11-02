#!/usr/bin/env python3
import argparse
import subprocess
import sys
import shutil
import time
import re
import csv
import os
from datetime import datetime


NIGHTCORE_ROOT = os.path.realpath(os.path.join(os.path.dirname(__file__), ".."))
BENCH_DIR = os.path.realpath(os.path.dirname(__file__))
RUN_BENCH = os.path.join(BENCH_DIR, "run_bench.sh")
DEFAULT_DURATION = 5


def build_rps_sweep() -> list:
    rps = list(range(10_000, 100_000 + 1, 10_000))
    rps += list(range(125_000, 300_000 + 1, 25_000))
    return rps


def build_workers_sweep() -> list:
    values = []
    v = 1
    while v <= 256:
        values.append(v)
        v *= 2
    return values


def unique_run_name(base: str | None) -> str:
    if base:
        return base
    ts = datetime.now().strftime("%Y%m%d-%H%M%S")
    return f"run-{ts}"


def ensure_dir(path: str) -> None:
    os.makedirs(path, exist_ok=True)


THROUGHPUT_RE = re.compile(r"Throughput:\s+([0-9]+(?:\.[0-9]+)?)\s+rps")
P50_RE = re.compile(r"\bp50:\s*([0-9]+)\b")
P99_RE = re.compile(r"\bp99:\s*([0-9]+)\b")


def parse_metrics(stress_client_path: str) -> tuple[float | None, int | None, int | None]:
    throughput = None
    p50 = None
    p99 = None
    try:
        with open(stress_client_path, "r", encoding="utf-8", errors="ignore") as f:
            for raw_line in f:
                line = raw_line.strip()
                m = THROUGHPUT_RE.search(line)
                if m:
                    # keep last seen in case multiple summaries appear
                    throughput = float(m.group(1))
                m = P50_RE.search(line)
                if m:
                    p50 = int(m.group(1))
                m = P99_RE.search(line)
                if m:
                    p99 = int(m.group(1))
    except FileNotFoundError:
        return None, None, None
    return throughput, p50, p99


def copy_outputs_to(dest_dir: str) -> None:
    src_dir = os.path.join(BENCH_DIR, "outputs")
    ensure_dir(dest_dir)
    if not os.path.isdir(src_dir):
        return
    for entry in os.listdir(src_dir):
        src_path = os.path.join(src_dir, entry)
        dst_path = os.path.join(dest_dir, entry)
        if os.path.isdir(src_path):
            shutil.copytree(src_path, dst_path, dirs_exist_ok=True)
        else:
            shutil.copy2(src_path, dst_path)


def run_single(mode: str,
               workers: int,
               target_rps: int,
               duration: int,
               remote: str,
               remote_addr: str,
               local_machnet_ip: str,
               extra_args: list[str],
               stdout_log_path: str) -> int:
    cmd = [
        RUN_BENCH,
        "--duration", str(duration),
        "--workers", str(workers),
        "--remote", remote,
        "--remote_addr", remote_addr,
        "--machnet",
        "--local_machnet_ip", local_machnet_ip,
        "--mode", mode,
        "--target_rps", str(target_rps),
    ] + extra_args

    ensure_dir(os.path.dirname(stdout_log_path))
    with open(stdout_log_path, "w", encoding="utf-8") as logf:
        logf.write("$ " + " ".join(cmd) + "\n\n")
        logf.flush()
        proc = subprocess.run(cmd, cwd=BENCH_DIR, stdout=logf, stderr=subprocess.STDOUT)
        return proc.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description="Nightcore benchmark experiment orchestrator")
    parser.add_argument("--duration", type=int, default=DEFAULT_DURATION)
    parser.add_argument("--remote", type=str, default="vj2267@sm110p-10s10615.wisc.cloudlab.us")
    parser.add_argument("--remote_addr", type=str, default="10.10.1.2")
    parser.add_argument("--local_machnet_ip", type=str, default="10.10.1.1")
    parser.add_argument("--modes", type=str, nargs="*", default=["noop", "hyperlight"],
                        help="Modes to test, default: noop hyperlight")
    parser.add_argument("--run_name", type=str, default=None)
    parser.add_argument("--extra", type=str, nargs=argparse.REMAINDER,
                        help="Extra args passed to run_bench.sh after '--' sentinel")

    args = parser.parse_args()

    # Build sweeps
    modes = list(args.modes)
    workers_list = build_workers_sweep()
    rps_list = build_rps_sweep()

    run_name = unique_run_name(args.run_name)
    run_dir = os.path.join(BENCH_DIR, "experiments", run_name)
    ensure_dir(run_dir)

    csv_path = os.path.join(run_dir, "results.csv")
    master_log_path = os.path.join(run_dir, "run.log")

    extra_args = []
    if args.extra:
        # If user passed "-- ..." collect the rest verbatim
        if args.extra and len(args.extra) > 0 and args.extra[0] == "--":
            extra_args = args.extra[1:]
        else:
            extra_args = args.extra

    total = len(modes) * len(workers_list) * len(rps_list)
    completed = 0
    started_at = time.time()

    with open(master_log_path, "w", encoding="utf-8") as mlog, \
         open(csv_path, "w", newline="", encoding="utf-8") as csvf:
        writer = csv.writer(csvf)
        writer.writerow(["mode", "workers", "target_rps", "throughput", "p50", "p99"])  # units: rps, us, us
        csvf.flush()

        for mode in modes:
            for workers in workers_list:
                for rps in rps_list:
                    idx = completed + 1
                    status_line = f"[{idx}/{total}] mode={mode} workers={workers} target_rps={rps}"
                    print(status_line)
                    mlog.write(status_line + "\n")
                    mlog.flush()

                    # Prepare per-run dirs and logs
                    per_run_dir = os.path.join(run_dir, f"mode={mode}", f"workers={workers}", f"rps={rps}")
                    ensure_dir(per_run_dir)
                    bench_stdout = os.path.join(per_run_dir, "bench_stdout.log")

                    rc = run_single(
                        mode=mode,
                        workers=workers,
                        target_rps=rps,
                        duration=args.duration,
                        remote=args.remote,
                        remote_addr=args.remote_addr,
                        local_machnet_ip=args.local_machnet_ip,
                        extra_args=extra_args,
                        stdout_log_path=bench_stdout,
                    )

                    # Copy outputs generated by run_bench.sh
                    copy_outputs_to(per_run_dir)

                    stress_out = os.path.join(per_run_dir, "stress_client.txt")
                    throughput, p50, p99 = parse_metrics(stress_out)

                    # Log a concise per-run summary
                    summary = (
                        f"SUMMARY mode={mode} workers={workers} target_rps={rps}: "
                        f"throughput={throughput if throughput is not None else 'NA'} rps, "
                        f"p50={p50 if p50 is not None else 'NA'} us, "
                        f"p99={p99 if p99 is not None else 'NA'} us, "
                        f"rc={rc}"
                    )
                    print(summary)
                    mlog.write(summary + "\n")
                    mlog.flush()

                    # Append to CSV if we have metrics
                    writer.writerow([
                        mode,
                        workers,
                        rps,
                        f"{throughput:.1f}" if throughput is not None else "",
                        p50 if p50 is not None else "",
                        p99 if p99 is not None else "",
                    ])
                    csvf.flush()

                    completed += 1
                    elapsed = time.time() - started_at
                    avg_per = elapsed / max(1, completed)
                    remaining = (total - completed) * avg_per
                    prog = f"Progress: {completed}/{total} done | elapsed={elapsed:.1f}s eta={remaining:.1f}s"
                    print(prog)
                    mlog.write(prog + "\n\n")
                    mlog.flush()

    print(f"\nExperiment complete. Results saved under: {run_dir}")
    print(f"Unified CSV: {csv_path}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("Interrupted.")
        sys.exit(130)



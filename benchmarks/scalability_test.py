#!/usr/bin/env python3
"""
Nightcore Scalability Testing Script

Tests scalability by varying num_io_workers, gateway_conn_per_worker, and workers.
Collects latency and throughput data and saves to CSV files.
"""

import subprocess
import os
import sys
import re
import csv
import json
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Tuple, Optional
import argparse

# ============================================================================
# Configuration
# ============================================================================

# Test modes to run
MODES = ["noop", "hyperlight"]  # Change this to test specific modes

# Scalability parameter ranges (modify these for your tests)
NUM_IO_WORKERS_RANGE = [1, 2, 4, 8]
GATEWAY_CONN_PER_WORKER_RANGE = [1]
WORKERS_RANGE = [1, 2, 4, 8, 16, 32]
# WORKERS_RANGE = [8]

# Fixed parameters (can be overridden via command line)
DEFAULT_DURATION = 5
DEFAULT_TARGET_RPS = 0  # 0 = unlimited
DEFAULT_INPUT_SIZE = 64
DEFAULT_INFLIGHT_LIMIT = 1
DEFAULT_WARMUP_SEC = 0

# Remote and Machnet configuration (optional)
# REMOTE_HOST = "vj2267@sm110p-10s10615.wisc.cloudlab.us"  # e.g., "user@host.example.com"
REMOTE_HOST = "" # e.g., "user@host.example.com"
# REMOTE_ADDR = "10.10.1.2"  # e.g., "10.10.1.2"
REMOTE_ADDR = "" # e.g., "10.10.1.2"
REMOTE_ROOT = ""  # Leave empty to use same path as local
USE_MACHNET = False
LOCAL_MACHNET_IP = "10.10.1.1"  # Leave empty for auto-detect

# ============================================================================
# Helper Functions
# ============================================================================

def get_script_dir() -> Path:
    """Get the directory containing this script."""
    return Path(__file__).parent.resolve()

def get_run_bench_script() -> Path:
    """Get the path to run_bench.sh."""
    return get_script_dir() / "run_bench.sh"

def parse_stress_output(output: str) -> Optional[Dict[str, float]]:
    """
    Parse stress_client output to extract latency and throughput metrics.

    Returns:
        Dict with keys: throughput, min_lat, p50_lat, p90_lat, p99_lat, p999_lat, max_lat
        or None if parsing failed
    """
    metrics = {}

    # Extract throughput (rps)
    throughput_match = re.search(r'Throughput:\s+([\d.]+)\s+rps', output)
    if throughput_match:
        metrics['throughput'] = float(throughput_match.group(1))

    # Extract latency percentiles (in microseconds)
    latency_patterns = {
        'min_lat': r'Min:\s+(\d+)',
        'p50_lat': r'p50:\s+(\d+)',
        'p90_lat': r'p90:\s+(\d+)',
        'p99_lat': r'p99:\s+(\d+)',
        'p999_lat': r'p99\.9:\s+(\d+)',
        'max_lat': r'Max:\s+(\d+)',
    }

    for key, pattern in latency_patterns.items():
        match = re.search(pattern, output)
        if match:
            metrics[key] = float(match.group(1))

    # Extract completion stats
    completed_match = re.search(r'Total Completed:\s+(\d+)', output)
    if completed_match:
        metrics['completed_count'] = int(completed_match.group(1))

    failed_match = re.search(r'Total Failed:\s+(\d+)', output)
    if failed_match:
        metrics['failed_count'] = int(failed_match.group(1))

    sent_match = re.search(r'Total Sent:\s+(\d+)', output)
    if sent_match:
        metrics['sent_count'] = int(sent_match.group(1))

    # Only return metrics if we got at least throughput
    if 'throughput' in metrics:
        return metrics
    return None

def run_benchmark(
    mode: str,
    num_workers: int,
    num_io_workers: int,
    gateway_conn_per_worker: int,
    duration: int,
    target_rps: int,
    input_size: int,
    inflight_limit: int,
    warmup_sec: int,
    remote_host: str = "",
    remote_addr: str = "",
    remote_root: str = "",
    use_machnet: bool = False,
    local_machnet_ip: str = "",
    output_dir: Path = None
) -> Optional[Dict[str, any]]:
    """
    Run a single benchmark with specified parameters.

    Returns:
        Dict with benchmark results or None if failed
    """
    script_path = get_run_bench_script()

    if not script_path.exists():
        print(f"ERROR: run_bench.sh not found at {script_path}")
        return None

    # Build command
    cmd = [
        str(script_path),
        "--mode", mode,
        "--workers", str(num_workers),
        "--num_io_workers", str(num_io_workers),
        "--gateway_conn_per_worker", str(gateway_conn_per_worker),
        "--duration", str(duration),
        "--target_rps", str(target_rps),
        "--input_size", str(input_size),
        "--inflight_limit", str(inflight_limit),
        "--warmup_sec", str(warmup_sec),
    ]

    # Add remote configuration if specified
    if remote_host:
        cmd.extend(["--remote", remote_host])
    if remote_addr:
        cmd.extend(["--remote_addr", remote_addr])
    if remote_root:
        cmd.extend(["--remote_root", remote_root])

    # Add Machnet configuration if enabled
    if use_machnet:
        cmd.append("--machnet")
        if local_machnet_ip:
            cmd.extend(["--local_machnet_ip", local_machnet_ip])

    print(f"\n{'='*80}")
    print(f"Running: {mode} mode")
    print(f"  Workers: {num_workers}")
    print(f"  IO Workers: {num_io_workers}")
    print(f"  Gateway Conns/Worker: {gateway_conn_per_worker}")
    print(f"  Total Gateway Conns: {num_io_workers * gateway_conn_per_worker}")
    print(f"{'='*80}")

    try:
        # Run the benchmark
        result = subprocess.run(
            cmd,
            cwd=get_script_dir(),
            capture_output=True,
            text=True,
            timeout=duration + warmup_sec + 60  # Add buffer time
        )

        # Parse output
        output = result.stdout + result.stderr
        metrics = parse_stress_output(output)

        if metrics is None:
            print(f"WARNING: Failed to parse metrics from output")
            print(f"Exit code: {result.returncode}")
            if result.returncode != 0:
                print(f"STDERR:\n{result.stderr[:1000]}")
            return None

        # Add configuration to metrics
        metrics.update({
            'mode': mode,
            'num_workers': num_workers,
            'num_io_workers': num_io_workers,
            'gateway_conn_per_worker': gateway_conn_per_worker,
            'total_gateway_conns': num_io_workers * gateway_conn_per_worker,
            'duration': duration,
            'target_rps': target_rps,
            'input_size': input_size,
            'inflight_limit': inflight_limit,
            'warmup_sec': warmup_sec,
            'use_machnet': use_machnet,
            'exit_code': result.returncode,
        })

        print(f"✓ Throughput: {metrics.get('throughput', 0):.1f} rps")
        print(f"  p50 latency: {metrics.get('p50_lat', 0):.0f} µs")
        print(f"  p99 latency: {metrics.get('p99_lat', 0):.0f} µs")

        return metrics

    except subprocess.TimeoutExpired:
        print(f"ERROR: Benchmark timed out")
        return None
    except Exception as e:
        print(f"ERROR: {e}")
        return None

def save_results_to_csv(results: List[Dict], output_file: Path):
    """Save benchmark results to CSV file."""
    if not results:
        print("No results to save")
        return

    # Define column order
    columns = [
        'mode', 'num_workers', 'num_io_workers', 'gateway_conn_per_worker',
        'total_gateway_conns', 'throughput', 'completed_count', 'sent_count',
        'failed_count', 'min_lat', 'p50_lat', 'p90_lat', 'p99_lat', 'p999_lat',
        'max_lat', 'duration', 'target_rps', 'input_size', 'inflight_limit',
        'warmup_sec', 'use_machnet', 'exit_code'
    ]

    # Write CSV
    with open(output_file, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=columns, extrasaction='ignore')
        writer.writeheader()
        writer.writerows(results)

    print(f"\n✓ Results saved to: {output_file}")

def run_scalability_tests(
    modes: List[str],
    num_io_workers_range: List[int],
    gateway_conn_per_worker_range: List[int],
    workers_range: List[int],
    duration: int,
    target_rps: int,
    input_size: int,
    inflight_limit: int,
    warmup_sec: int,
    remote_host: str = "",
    remote_addr: str = "",
    remote_root: str = "",
    use_machnet: bool = False,
    local_machnet_ip: str = "",
    output_prefix: str = ""
) -> Tuple[List[Dict], Path]:
    """
    Run comprehensive scalability tests.

    Returns:
        Tuple of (results_list, output_csv_path)
    """
    results = []

    # Create timestamped output directory
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    output_dir = get_script_dir() / "scalability_results" / timestamp
    output_dir.mkdir(parents=True, exist_ok=True)

    # Save test configuration
    config = {
        'modes': modes,
        'num_io_workers_range': num_io_workers_range,
        'gateway_conn_per_worker_range': gateway_conn_per_worker_range,
        'workers_range': workers_range,
        'duration': duration,
        'target_rps': target_rps,
        'input_size': input_size,
        'inflight_limit': inflight_limit,
        'warmup_sec': warmup_sec,
        'use_machnet': use_machnet,
        'timestamp': timestamp,
    }

    config_file = output_dir / "config.json"
    with open(config_file, 'w') as f:
        json.dump(config, f, indent=2)

    print(f"\nConfiguration saved to: {config_file}")

    # Calculate total tests
    total_tests = len(modes) * len(num_io_workers_range) * \
                  len(gateway_conn_per_worker_range) * len(workers_range)

    print(f"\n{'='*80}")
    print(f"Starting scalability tests: {total_tests} total configurations")
    print(f"Results will be saved to: {output_dir}")
    print(f"{'='*80}\n")

    test_num = 0

    for mode in modes:
        for num_io_workers in num_io_workers_range:
            for gateway_conn_per_worker in gateway_conn_per_worker_range:
                for num_workers in workers_range:
                    test_num += 1

                    print(f"\n[Test {test_num}/{total_tests}]")

                    metrics = run_benchmark(
                        mode=mode,
                        num_workers=num_workers,
                        num_io_workers=num_io_workers,
                        gateway_conn_per_worker=gateway_conn_per_worker,
                        duration=duration,
                        target_rps=target_rps,
                        input_size=input_size,
                        inflight_limit=inflight_limit,
                        warmup_sec=warmup_sec,
                        remote_host=remote_host,
                        remote_addr=remote_addr,
                        remote_root=remote_root,
                        use_machnet=use_machnet,
                        local_machnet_ip=local_machnet_ip,
                        output_dir=output_dir
                    )

                    if metrics:
                        results.append(metrics)

                        # Save intermediate results after each test
                        csv_file = output_dir / f"results_{output_prefix}.csv"
                        save_results_to_csv(results, csv_file)

                    # Small delay between tests
                    import time
                    time.sleep(2)

    # Final save
    csv_file = output_dir / f"results_{output_prefix}.csv"
    save_results_to_csv(results, csv_file)

    print(f"\n{'='*80}")
    print(f"Scalability tests completed!")
    print(f"Total tests run: {len(results)}/{total_tests}")
    print(f"Results directory: {output_dir}")
    print(f"{'='*80}\n")

    return results, csv_file

# ============================================================================
# Main
# ============================================================================

def main():
    parser = argparse.ArgumentParser(
        description="Run Nightcore scalability tests",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Run with default settings
  ./scalability_test.py

  # Custom parameter ranges
  ./scalability_test.py --workers 1,2,4,8 --io-workers 1,2,4

  # Test only noop mode with shorter duration
  ./scalability_test.py --modes noop --duration 15

  # Remote testing with Machnet
  ./scalability_test.py --remote user@host --remote-addr 10.10.1.2 --machnet
        """
    )

    parser.add_argument('--modes', type=str, default=','.join(MODES),
                       help=f'Comma-separated modes to test (default: {",".join(MODES)})')
    parser.add_argument('--workers', type=str, default=','.join(map(str, WORKERS_RANGE)),
                       help=f'Comma-separated worker counts (default: {",".join(map(str, WORKERS_RANGE))})')
    parser.add_argument('--io-workers', type=str, default=','.join(map(str, NUM_IO_WORKERS_RANGE)),
                       help=f'Comma-separated IO worker counts (default: {",".join(map(str, NUM_IO_WORKERS_RANGE))})')
    parser.add_argument('--gateway-conns', type=str, default=','.join(map(str, GATEWAY_CONN_PER_WORKER_RANGE)),
                       help=f'Comma-separated gateway conns per worker (default: {",".join(map(str, GATEWAY_CONN_PER_WORKER_RANGE))})')

    parser.add_argument('--duration', type=int, default=DEFAULT_DURATION,
                       help=f'Test duration in seconds (default: {DEFAULT_DURATION})')
    parser.add_argument('--target-rps', type=int, default=DEFAULT_TARGET_RPS,
                       help=f'Target RPS, 0=unlimited (default: {DEFAULT_TARGET_RPS})')
    parser.add_argument('--input-size', type=int, default=DEFAULT_INPUT_SIZE,
                       help=f'Input size in bytes (default: {DEFAULT_INPUT_SIZE})')
    parser.add_argument('--inflight-limit', type=int, default=DEFAULT_INFLIGHT_LIMIT,
                       help=f'Max inflight per connection (default: {DEFAULT_INFLIGHT_LIMIT})')
    parser.add_argument('--warmup', type=int, default=DEFAULT_WARMUP_SEC,
                       help=f'Warmup seconds (default: {DEFAULT_WARMUP_SEC})')

    parser.add_argument('--remote', type=str, default=REMOTE_HOST,
                       help='Remote host for stress_client (e.g., user@host)')
    parser.add_argument('--remote-addr', type=str, default=REMOTE_ADDR,
                       help='Remote IP address for Engine connection')
    parser.add_argument('--remote-root', type=str, default=REMOTE_ROOT,
                       help='Nightcore root on remote host')
    parser.add_argument('--machnet', action='store_true', default=USE_MACHNET,
                       help='Use Machnet transport')
    parser.add_argument('--local-machnet-ip', type=str, default=LOCAL_MACHNET_IP,
                       help='Local Machnet IP address')

    parser.add_argument('--output-prefix', type=str, default='scalability',
                       help='Prefix for output files (default: scalability)')

    args = parser.parse_args()

    # Parse comma-separated lists
    modes = args.modes.split(',')
    workers_range = [int(x) for x in args.workers.split(',')]
    num_io_workers_range = [int(x) for x in args.io_workers.split(',')]
    gateway_conn_per_worker_range = [int(x) for x in args.gateway_conns.split(',')]

    # Run tests
    results, csv_file = run_scalability_tests(
        modes=modes,
        num_io_workers_range=num_io_workers_range,
        gateway_conn_per_worker_range=gateway_conn_per_worker_range,
        workers_range=workers_range,
        duration=args.duration,
        target_rps=args.target_rps,
        input_size=args.input_size,
        inflight_limit=args.inflight_limit,
        warmup_sec=args.warmup,
        remote_host=args.remote,
        remote_addr=args.remote_addr,
        remote_root=args.remote_root,
        use_machnet=args.machnet,
        local_machnet_ip=args.local_machnet_ip,
        output_prefix=args.output_prefix
    )

    print(f"\nTo plot results, run:")
    print(f"  ./plot_scalability.py {csv_file}")

if __name__ == "__main__":
    main()

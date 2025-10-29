#!/usr/bin/env python3
"""
Nightcore Scalability Results Plotting Script

Generates visualizations from scalability_test.py CSV output.
Creates multiple plots showing throughput, latency, and scalability trends.
"""

import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
import argparse
import sys
from pathlib import Path
from typing import List, Dict

# Set plotting style
sns.set_style("whitegrid")
plt.rcParams['figure.figsize'] = (12, 8)
plt.rcParams['font.size'] = 10

def load_results(csv_file: Path) -> pd.DataFrame:
    """Load results from CSV file."""
    if not csv_file.exists():
        print(f"ERROR: CSV file not found: {csv_file}")
        sys.exit(1)

    df = pd.read_csv(csv_file)
    print(f"Loaded {len(df)} results from {csv_file}")
    print(f"Columns: {list(df.columns)}")
    print(f"Modes: {df['mode'].unique()}")
    print(f"Workers range: {sorted(df['num_workers'].unique())}")
    print(f"IO workers range: {sorted(df['num_io_workers'].unique())}")
    print(f"Gateway conns/worker range: {sorted(df['gateway_conn_per_worker'].unique())}")
    return df

def plot_throughput_vs_workers(df: pd.DataFrame, output_dir: Path):
    """Plot throughput vs number of workers for different configurations."""
    modes = df['mode'].unique()
    io_workers_values = sorted(df['num_io_workers'].unique())
    gateway_conn_values = sorted(df['gateway_conn_per_worker'].unique())

    for mode in modes:
        mode_df = df[df['mode'] == mode]

        # Plot for each IO worker count
        fig, axes = plt.subplots(1, len(io_workers_values),
                                figsize=(6*len(io_workers_values), 5))
        if len(io_workers_values) == 1:
            axes = [axes]

        for idx, num_io_workers in enumerate(io_workers_values):
            ax = axes[idx]

            for gateway_conns in gateway_conn_values:
                subset = mode_df[
                    (mode_df['num_io_workers'] == num_io_workers) &
                    (mode_df['gateway_conn_per_worker'] == gateway_conns)
                ]
                if len(subset) > 0:
                    label = f"GW conns/worker={gateway_conns} (total={num_io_workers*gateway_conns})"
                    ax.plot(subset['num_workers'], subset['throughput'],
                           marker='o', label=label, linewidth=2)

            ax.set_xlabel('Number of Workers', fontsize=12)
            ax.set_ylabel('Throughput (RPS)', fontsize=12)
            ax.set_title(f'IO Workers = {num_io_workers}', fontsize=13)
            ax.legend()
            ax.grid(True, alpha=0.3)

        plt.suptitle(f'Throughput vs Workers - {mode.upper()} Mode',
                    fontsize=15, fontweight='bold')
        plt.tight_layout()

        output_file = output_dir / f'throughput_vs_workers_{mode}.png'
        plt.savefig(output_file, dpi=150, bbox_inches='tight')
        print(f"✓ Saved: {output_file}")
        plt.close()

def plot_latency_vs_workers(df: pd.DataFrame, output_dir: Path):
    """Plot latency percentiles vs number of workers."""
    modes = df['mode'].unique()
    latency_metrics = ['p50_lat', 'p90_lat', 'p99_lat', 'p999_lat']

    for mode in modes:
        mode_df = df[df['mode'] == mode]

        # Find the "best" configuration (highest IO workers and gateway conns)
        best_io_workers = mode_df['num_io_workers'].max()
        best_gateway_conns = mode_df['gateway_conn_per_worker'].max()

        best_config_df = mode_df[
            (mode_df['num_io_workers'] == best_io_workers) &
            (mode_df['gateway_conn_per_worker'] == best_gateway_conns)
        ]

        if len(best_config_df) == 0:
            continue

        fig, ax = plt.subplots(figsize=(10, 6))

        for metric in latency_metrics:
            if metric in best_config_df.columns:
                label = metric.upper().replace('_LAT', '')
                ax.plot(best_config_df['num_workers'],
                       best_config_df[metric] / 1000,  # Convert to ms
                       marker='o', label=label, linewidth=2)

        ax.set_xlabel('Number of Workers', fontsize=12)
        ax.set_ylabel('Latency (ms)', fontsize=12)
        ax.set_title(f'Latency Percentiles - {mode.upper()} Mode\n'
                    f'(IO Workers={best_io_workers}, GW Conns/Worker={best_gateway_conns})',
                    fontsize=13, fontweight='bold')
        ax.legend()
        ax.grid(True, alpha=0.3)
        ax.set_yscale('log')

        plt.tight_layout()
        output_file = output_dir / f'latency_vs_workers_{mode}.png'
        plt.savefig(output_file, dpi=150, bbox_inches='tight')
        print(f"✓ Saved: {output_file}")
        plt.close()

def plot_mode_comparison(df: pd.DataFrame, output_dir: Path):
    """Compare throughput between different modes."""
    modes = df['mode'].unique()

    if len(modes) < 2:
        print("Skipping mode comparison (only one mode in results)")
        return

    # Find common configuration across modes
    io_workers_values = sorted(df['num_io_workers'].unique())
    gateway_conn_values = sorted(df['gateway_conn_per_worker'].unique())

    # Use best configuration
    best_io_workers = max(io_workers_values)
    best_gateway_conns = max(gateway_conn_values)

    fig, ax = plt.subplots(figsize=(10, 6))

    for mode in modes:
        mode_df = df[
            (df['mode'] == mode) &
            (df['num_io_workers'] == best_io_workers) &
            (df['gateway_conn_per_worker'] == best_gateway_conns)
        ]

        if len(mode_df) > 0:
            ax.plot(mode_df['num_workers'], mode_df['throughput'],
                   marker='o', label=mode.upper(), linewidth=2, markersize=8)

    ax.set_xlabel('Number of Workers', fontsize=12)
    ax.set_ylabel('Throughput (RPS)', fontsize=12)
    ax.set_title(f'Mode Comparison - Throughput\n'
                f'(IO Workers={best_io_workers}, GW Conns/Worker={best_gateway_conns})',
                fontsize=13, fontweight='bold')
    ax.legend()
    ax.grid(True, alpha=0.3)

    plt.tight_layout()
    output_file = output_dir / 'mode_comparison_throughput.png'
    plt.savefig(output_file, dpi=150, bbox_inches='tight')
    print(f"✓ Saved: {output_file}")
    plt.close()

def plot_io_workers_impact(df: pd.DataFrame, output_dir: Path):
    """Plot impact of number of IO workers on throughput."""
    modes = df['mode'].unique()

    for mode in modes:
        mode_df = df[df['mode'] == mode]

        # Use best gateway conn config
        best_gateway_conns = mode_df['gateway_conn_per_worker'].max()
        subset = mode_df[mode_df['gateway_conn_per_worker'] == best_gateway_conns]

        if len(subset) == 0:
            continue

        fig, ax = plt.subplots(figsize=(10, 6))

        io_workers_values = sorted(subset['num_io_workers'].unique())

        for num_io_workers in io_workers_values:
            io_subset = subset[subset['num_io_workers'] == num_io_workers]
            ax.plot(io_subset['num_workers'], io_subset['throughput'],
                   marker='o', label=f'IO Workers = {num_io_workers}',
                   linewidth=2, markersize=8)

        ax.set_xlabel('Number of Workers', fontsize=12)
        ax.set_ylabel('Throughput (RPS)', fontsize=12)
        ax.set_title(f'Impact of IO Workers - {mode.upper()} Mode\n'
                    f'(GW Conns/Worker={best_gateway_conns})',
                    fontsize=13, fontweight='bold')
        ax.legend()
        ax.grid(True, alpha=0.3)

        plt.tight_layout()
        output_file = output_dir / f'io_workers_impact_{mode}.png'
        plt.savefig(output_file, dpi=150, bbox_inches='tight')
        print(f"✓ Saved: {output_file}")
        plt.close()

def plot_gateway_conns_impact(df: pd.DataFrame, output_dir: Path):
    """Plot impact of gateway connections per worker on throughput."""
    modes = df['mode'].unique()

    for mode in modes:
        mode_df = df[df['mode'] == mode]

        # Use best IO workers config
        best_io_workers = mode_df['num_io_workers'].max()
        subset = mode_df[mode_df['num_io_workers'] == best_io_workers]

        if len(subset) == 0:
            continue

        fig, ax = plt.subplots(figsize=(10, 6))

        gateway_conn_values = sorted(subset['gateway_conn_per_worker'].unique())

        for gateway_conns in gateway_conn_values:
            gw_subset = subset[subset['gateway_conn_per_worker'] == gateway_conns]
            total_conns = best_io_workers * gateway_conns
            ax.plot(gw_subset['num_workers'], gw_subset['throughput'],
                   marker='o',
                   label=f'Conns/Worker={gateway_conns} (total={total_conns})',
                   linewidth=2, markersize=8)

        ax.set_xlabel('Number of Workers', fontsize=12)
        ax.set_ylabel('Throughput (RPS)', fontsize=12)
        ax.set_title(f'Impact of Gateway Connections - {mode.upper()} Mode\n'
                    f'(IO Workers={best_io_workers})',
                    fontsize=13, fontweight='bold')
        ax.legend()
        ax.grid(True, alpha=0.3)

        plt.tight_layout()
        output_file = output_dir / f'gateway_conns_impact_{mode}.png'
        plt.savefig(output_file, dpi=150, bbox_inches='tight')
        print(f"✓ Saved: {output_file}")
        plt.close()

def plot_scalability_efficiency(df: pd.DataFrame, output_dir: Path):
    """Plot scalability efficiency (throughput per worker)."""
    modes = df['mode'].unique()

    for mode in modes:
        mode_df = df[df['mode'] == mode]

        # Use best configuration
        best_io_workers = mode_df['num_io_workers'].max()
        best_gateway_conns = mode_df['gateway_conn_per_worker'].max()

        subset = mode_df[
            (mode_df['num_io_workers'] == best_io_workers) &
            (mode_df['gateway_conn_per_worker'] == best_gateway_conns)
        ]

        if len(subset) == 0:
            continue

        # Calculate throughput per worker
        subset = subset.copy()
        subset['throughput_per_worker'] = subset['throughput'] / subset['num_workers']

        fig, ax = plt.subplots(figsize=(10, 6))

        ax.plot(subset['num_workers'], subset['throughput_per_worker'],
               marker='o', linewidth=2, markersize=8, color='darkblue')

        ax.set_xlabel('Number of Workers', fontsize=12)
        ax.set_ylabel('Throughput per Worker (RPS)', fontsize=12)
        ax.set_title(f'Scalability Efficiency - {mode.upper()} Mode\n'
                    f'(IO Workers={best_io_workers}, GW Conns/Worker={best_gateway_conns})',
                    fontsize=13, fontweight='bold')
        ax.grid(True, alpha=0.3)

        plt.tight_layout()
        output_file = output_dir / f'scalability_efficiency_{mode}.png'
        plt.savefig(output_file, dpi=150, bbox_inches='tight')
        print(f"✓ Saved: {output_file}")
        plt.close()

def generate_summary_report(df: pd.DataFrame, output_dir: Path):
    """Generate a text summary report of key findings."""
    report_file = output_dir / 'summary_report.txt'

    with open(report_file, 'w') as f:
        f.write("="*80 + "\n")
        f.write("NIGHTCORE SCALABILITY TEST SUMMARY REPORT\n")
        f.write("="*80 + "\n\n")

        f.write(f"Total configurations tested: {len(df)}\n")
        f.write(f"Modes: {', '.join(df['mode'].unique())}\n\n")

        for mode in df['mode'].unique():
            mode_df = df[df['mode'] == mode]

            f.write(f"\n{'='*80}\n")
            f.write(f"MODE: {mode.upper()}\n")
            f.write(f"{'='*80}\n\n")

            # Best throughput configuration
            best_idx = mode_df['throughput'].idxmax()
            best = mode_df.loc[best_idx]

            f.write("Best Throughput Configuration:\n")
            f.write(f"  Workers: {int(best['num_workers'])}\n")
            f.write(f"  IO Workers: {int(best['num_io_workers'])}\n")
            f.write(f"  Gateway Conns/Worker: {int(best['gateway_conn_per_worker'])}\n")
            f.write(f"  Total Gateway Conns: {int(best['total_gateway_conns'])}\n")
            f.write(f"  Throughput: {best['throughput']:.1f} RPS\n")
            f.write(f"  P50 Latency: {best['p50_lat']:.0f} µs\n")
            f.write(f"  P99 Latency: {best['p99_lat']:.0f} µs\n")
            f.write(f"  P99.9 Latency: {best['p999_lat']:.0f} µs\n\n")

            # Best latency configuration (lowest p99)
            best_lat_idx = mode_df['p99_lat'].idxmin()
            best_lat = mode_df.loc[best_lat_idx]

            f.write("Best Latency Configuration:\n")
            f.write(f"  Workers: {int(best_lat['num_workers'])}\n")
            f.write(f"  IO Workers: {int(best_lat['num_io_workers'])}\n")
            f.write(f"  Gateway Conns/Worker: {int(best_lat['gateway_conn_per_worker'])}\n")
            f.write(f"  Total Gateway Conns: {int(best_lat['total_gateway_conns'])}\n")
            f.write(f"  Throughput: {best_lat['throughput']:.1f} RPS\n")
            f.write(f"  P50 Latency: {best_lat['p50_lat']:.0f} µs\n")
            f.write(f"  P99 Latency: {best_lat['p99_lat']:.0f} µs\n")
            f.write(f"  P99.9 Latency: {best_lat['p999_lat']:.0f} µs\n\n")

            # Statistics
            f.write("Overall Statistics:\n")
            f.write(f"  Throughput range: {mode_df['throughput'].min():.1f} - {mode_df['throughput'].max():.1f} RPS\n")
            f.write(f"  P50 latency range: {mode_df['p50_lat'].min():.0f} - {mode_df['p50_lat'].max():.0f} µs\n")
            f.write(f"  P99 latency range: {mode_df['p99_lat'].min():.0f} - {mode_df['p99_lat'].max():.0f} µs\n")

    print(f"\n✓ Summary report saved to: {report_file}")

def main():
    parser = argparse.ArgumentParser(
        description="Plot Nightcore scalability test results",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Plot results from a CSV file
  ./plot_scalability.py results.csv

  # Specify custom output directory
  ./plot_scalability.py results.csv --output-dir ./my_plots

  # Plot only specific plot types
  ./plot_scalability.py results.csv --plots throughput latency
        """
    )

    parser.add_argument('csv_file', type=str,
                       help='Path to CSV file from scalability_test.py')
    parser.add_argument('--output-dir', type=str, default=None,
                       help='Output directory for plots (default: same as CSV)')
    parser.add_argument('--plots', nargs='+',
                       choices=['throughput', 'latency', 'comparison',
                               'io_workers', 'gateway_conns', 'efficiency', 'all'],
                       default=['all'],
                       help='Which plots to generate (default: all)')

    args = parser.parse_args()

    # Load results
    csv_file = Path(args.csv_file)
    df = load_results(csv_file)

    # Determine output directory
    if args.output_dir:
        output_dir = Path(args.output_dir)
    else:
        output_dir = csv_file.parent / 'plots'

    output_dir.mkdir(parents=True, exist_ok=True)
    print(f"\nGenerating plots in: {output_dir}")

    # Generate requested plots
    plot_types = args.plots
    if 'all' in plot_types:
        plot_types = ['throughput', 'latency', 'comparison',
                     'io_workers', 'gateway_conns', 'efficiency']

    print("\nGenerating plots...")

    if 'throughput' in plot_types:
        plot_throughput_vs_workers(df, output_dir)

    if 'latency' in plot_types:
        plot_latency_vs_workers(df, output_dir)

    if 'comparison' in plot_types:
        plot_mode_comparison(df, output_dir)

    if 'io_workers' in plot_types:
        plot_io_workers_impact(df, output_dir)

    if 'gateway_conns' in plot_types:
        plot_gateway_conns_impact(df, output_dir)

    if 'efficiency' in plot_types:
        plot_scalability_efficiency(df, output_dir)

    # Always generate summary report
    generate_summary_report(df, output_dir)

    print(f"\n{'='*80}")
    print("All plots generated successfully!")
    print(f"Output directory: {output_dir}")
    print(f"{'='*80}\n")

if __name__ == "__main__":
    main()

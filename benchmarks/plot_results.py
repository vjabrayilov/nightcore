#!/usr/bin/env python3
import argparse
import csv
import os
import sys
import time
from collections import defaultdict
from datetime import datetime


def try_import_matplotlib():
    try:
        import matplotlib.pyplot as plt  # type: ignore
        return plt
    except Exception as exc:  # pragma: no cover
        print("Error: matplotlib is required to run this script.")
        print("Install it via: pip install matplotlib")
        print(f"Details: {exc}")
        sys.exit(1)


def discover_latest_results_csv(benchmarks_dir: str) -> str | None:
    experiments_dir = os.path.join(benchmarks_dir, "experiments")
    if not os.path.isdir(experiments_dir):
        return None
    newest_csv_path = None
    newest_mtime = -1.0
    for root, _dirs, files in os.walk(experiments_dir):
        for name in files:
            if name == "results.csv":
                path = os.path.join(root, name)
                try:
                    mtime = os.path.getmtime(path)
                except OSError:
                    continue
                if mtime > newest_mtime:
                    newest_mtime = mtime
                    newest_csv_path = path
    return newest_csv_path


def read_results(csv_path: str) -> list[dict]:
    rows: list[dict] = []
    with open(csv_path, "r", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for r in reader:
            try:
                mode = r["mode"].strip()
                workers = int(r["workers"]) if r["workers"] else None
                target_rps = int(r["target_rps"]) if r["target_rps"] else None
                throughput = float(r["throughput"]) if r.get("throughput") else None
                p50 = int(r["p50"]) if r.get("p50") else None
                p99 = int(r["p99"]) if r.get("p99") else None
            except Exception:
                # Skip malformed rows
                continue
            rows.append({
                "mode": mode,
                "workers": workers,
                "target_rps": target_rps,
                "throughput": throughput,
                "p50": p50,
                "p99": p99,
            })
    return rows


def filter_results(rows: list[dict], modes: list[str] | None, workers: list[int] | None) -> list[dict]:
    def mode_ok(m: str) -> bool:
        return True if not modes else (m in modes)

    def workers_ok(w: int | None) -> bool:
        return True if not workers else (w in workers)

    return [r for r in rows if r["mode"] is not None and mode_ok(r["mode"]) and workers_ok(r["workers"])]


def group_by_mode_workers(rows: list[dict]) -> dict[tuple[str, int], list[dict]]:
    grouped: dict[tuple[str, int], list[dict]] = defaultdict(list)
    for r in rows:
        key = (r["mode"], int(r["workers"]))
        grouped[key].append(r)
    # Sort each series by target_rps for nice lines
    for key, series in grouped.items():
        series.sort(key=lambda x: (x["target_rps"] if x["target_rps"] is not None else -1))
    return grouped


def ensure_output_dir(base_dir: str | None, csv_path: str) -> str:
    if base_dir:
        out_dir = os.path.realpath(base_dir)
    else:
        csv_dir = os.path.dirname(os.path.realpath(csv_path))
        ts = datetime.now().strftime("%Y%m%d-%H%M%S")
        out_dir = os.path.join(csv_dir, "plots", ts)
    os.makedirs(out_dir, exist_ok=True)
    return out_dir


def collect_distinct(values: list[dict], field: str) -> list:
    seen = []
    for r in values:
        v = r[field]
        if v not in seen:
            seen.append(v)
    return seen


def plot_latencies(
    csv_path: str,
    metric: str,
    overlay: str,
    facet: str,
    modes_filter: list[str] | None,
    workers_filter: list[int] | None,
    latency_unit: str,
    output_dir: str | None,
    show: bool,
):
    plt = try_import_matplotlib()

    rows = read_results(csv_path)
    rows = filter_results(rows, modes_filter, workers_filter)
    if not rows:
        print("No rows after filtering; nothing to plot.")
        return

    unit_divisor = 1.0 if latency_unit == "us" else 1000.0
    unit_label = latency_unit

    grouped = group_by_mode_workers(rows)
    all_modes = sorted(collect_distinct(rows, "mode"))
    all_workers = sorted(collect_distinct(rows, "workers"))

    out_dir = ensure_output_dir(output_dir, csv_path)

    metrics_to_draw = [metric] if metric in ("p50", "p99") else ["p50", "p99"]

    def line_style_for_workers(w: int) -> str:
        styles = ["-", "--", "-.", ":"]
        idx = all_workers.index(w) if w in all_workers else 0
        return styles[idx % len(styles)]

    def color_for_mode(m: str) -> str:
        palette = [
            "#1f77b4", "#ff7f0e", "#2ca02c", "#d62728",
            "#9467bd", "#8c564b", "#e377c2", "#7f7f7f", "#bcbd22", "#17becf",
        ]
        idx = all_modes.index(m) if m in all_modes else 0
        return palette[idx % len(palette)]

    # Faceting plans
    if facet == "none":
        facet_keys = [None]
    elif facet == "mode":
        facet_keys = [(m, None) for m in all_modes]
    elif facet == "workers":
        facet_keys = [(None, w) for w in all_workers]
    else:
        print(f"Unknown facet value: {facet}")
        return

    saved_paths: list[str] = []

    for metric_name in metrics_to_draw:
        for facet_key in facet_keys:
            facet_mode, facet_workers = (facet_key if facet_key is not None else (None, None))

            fig, ax = plt.subplots(figsize=(8, 5))

            # Build series to draw depending on overlay
            series_plotted = 0
            if overlay == "mode":
                # Lines: one per (mode, workers) where mode varies and workers may vary
                for (m, w), series in grouped.items():
                    if facet_mode is not None and m != facet_mode:
                        continue
                    if facet_workers is not None and w != facet_workers:
                        continue
                    xs = [pt["target_rps"] for pt in series if pt["target_rps"] is not None and pt.get(metric_name) is not None]
                    ys = [pt[metric_name] / unit_divisor for pt in series if pt["target_rps"] is not None and pt.get(metric_name) is not None]
                    if not xs:
                        continue
                    ax.plot(xs, ys, label=f"mode={m}, workers={w}", color=color_for_mode(m), linestyle=line_style_for_workers(w))
                    series_plotted += 1
            elif overlay == "workers":
                # Lines: one per (workers, mode)
                for (m, w), series in grouped.items():
                    if facet_mode is not None and m != facet_mode:
                        continue
                    if facet_workers is not None and w != facet_workers:
                        continue
                    xs = [pt["target_rps"] for pt in series if pt["target_rps"] is not None and pt.get(metric_name) is not None]
                    ys = [pt[metric_name] / unit_divisor for pt in series if pt["target_rps"] is not None and pt.get(metric_name) is not None]
                    if not xs:
                        continue
                    ax.plot(xs, ys, label=f"workers={w}, mode={m}", color=color_for_mode(m), linestyle=line_style_for_workers(w))
                    series_plotted += 1
            elif overlay == "both":
                for (m, w), series in grouped.items():
                    if facet_mode is not None and m != facet_mode:
                        continue
                    if facet_workers is not None and w != facet_workers:
                        continue
                    xs = [pt["target_rps"] for pt in series if pt["target_rps"] is not None and pt.get(metric_name) is not None]
                    ys = [pt[metric_name] / unit_divisor for pt in series if pt["target_rps"] is not None and pt.get(metric_name) is not None]
                    if not xs:
                        continue
                    ax.plot(xs, ys, label=f"{m}, w={w}", color=color_for_mode(m), linestyle=line_style_for_workers(w))
                    series_plotted += 1
            else:
                print(f"Unknown overlay value: {overlay}")
                plt.close(fig)
                return

            if series_plotted == 0:
                plt.close(fig)
                continue

            ax.set_xlabel("target_rps")
            ax.set_ylabel(f"{metric_name} latency ({unit_label})")
            title_parts = [f"{metric_name} vs target_rps"]
            if facet_mode is not None:
                title_parts.append(f"mode={facet_mode}")
            if facet_workers is not None:
                title_parts.append(f"workers={facet_workers}")
            ax.set_title(" | ".join(title_parts))
            ax.grid(True, linestyle=":", linewidth=0.5)
            ax.legend(loc="best", fontsize=8)

            out_dir_real = ensure_output_dir(output_dir, csv_path)
            facet_suffix = []
            if facet_mode is not None:
                facet_suffix.append(f"mode={facet_mode}")
            if facet_workers is not None:
                facet_suffix.append(f"workers={facet_workers}")
            suffix = "_" + "_".join(facet_suffix) if facet_suffix else ""
            png_name = f"lat_{metric_name}_overlay={overlay}{suffix}.png"
            out_path = os.path.join(out_dir_real, png_name)
            fig.tight_layout()
            fig.savefig(out_path, dpi=120)
            saved_paths.append(out_path)
            if show:
                plt.show(block=False)
                # Let the UI update briefly for interactive runs
                plt.pause(0.1)
            else:
                plt.close(fig)

    if saved_paths:
        print("Saved plots:")
        for p in saved_paths:
            print(f"  {p}")
    else:
        print("No plots generated (check filters and data).")


def main() -> int:
    base_dir = os.path.realpath(os.path.dirname(__file__))
    default_csv = discover_latest_results_csv(base_dir)

    parser = argparse.ArgumentParser(description="Plot Nightcore benchmark results")
    parser.add_argument("--csv", type=str, default=default_csv,
                        help="Path to results.csv (defaults to latest under benchmarks/experiments)")
    parser.add_argument("--metric", type=str, choices=["p50", "p99", "both"], default="both",
                        help="Latency metric to plot on Y axis")
    parser.add_argument("--overlay", type=str, choices=["mode", "workers", "both"], default="both",
                        help="Which dimension to overlay in a single figure")
    parser.add_argument("--facet", type=str, choices=["none", "mode", "workers"], default="none",
                        help="Create separate figures per mode or workers")
    parser.add_argument("--modes", type=str, nargs="*", default=None,
                        help="Filter to one or more modes (default: all)")
    parser.add_argument("--workers", type=int, nargs="*", default=None,
                        help="Filter to one or more worker counts (default: all)")
    parser.add_argument("--latency_unit", type=str, choices=["us", "ms"], default="us",
                        help="Latency unit to display (us or ms)")
    parser.add_argument("--output_dir", type=str, default=None,
                        help="Directory to save plots (default: <csv_dir>/plots/<timestamp>)")
    parser.add_argument("--show", action="store_true", help="Show plots interactively as well as saving")

    args = parser.parse_args()

    if not args.csv:
        print("Error: --csv not provided and no results.csv discovered under benchmarks/experiments.")
        return 2
    if not os.path.isfile(args.csv):
        print(f"Error: CSV file not found: {args.csv}")
        return 2

    t0 = time.time()
    plot_latencies(
        csv_path=args.csv,
        metric=args.metric,
        overlay=args.overlay,
        facet=args.facet,
        modes_filter=args.modes,
        workers_filter=args.workers,
        latency_unit=args.latency_unit,
        output_dir=args.output_dir,
        show=args.show,
    )
    t1 = time.time()
    print(f"Done in {t1 - t0:.2f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())



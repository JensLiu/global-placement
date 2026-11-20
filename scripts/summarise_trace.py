#!/usr/bin/env python3
"""Summarise and compare placement benchmark metrics (metrics.jsonl).

Prints, per trace, the input baseline and every post-solve frame: HPWL (all
signal nets), its ratio to the input placement, the quadratic objective,
density overflow, containment, centre-of-gravity error, movement and time.
With several traces, also prints a final-frame comparison.

  python3 scripts/summarise_trace.py build/traces/RUN
  python3 scripts/summarise_trace.py RUN_A RUN_B --plot comparison.png
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path


class MetricsError(ValueError):
    """Missing or malformed metrics.jsonl."""


def load_metrics(directory: Path):
    """Return (header/baseline, frames) from DIRECTORY/metrics.jsonl."""
    path = directory / "metrics.jsonl"
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise MetricsError(f"{path}: {error}") from error
    records = []
    for number, line in enumerate(lines, 1):
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError:
            if number == len(lines):
                break  # an interrupted final line is not a committed record
            raise MetricsError(f"{path}:{number}: malformed JSON")
    if not records or records[0].get("schema") != "global-placement.gordian-metrics":
        raise MetricsError(f"{path}: missing metrics header")
    if records[0].get("version") != 1:
        raise MetricsError(f"{path}: unsupported metrics version")
    return records[0], records[1:]


def row(record, baseline):
    hpwl = record["hpwl_um"]["all"]
    move = record["movement_um"]["from_previous_solve"]
    cog = record["cog_error_um"]
    label = "input" if record["phase"] == "input" else f"solve {record['iteration']}"
    return [label, f"{hpwl:,.0f}", f"{hpwl / baseline['hpwl_um']['all']:.3f}",
            f"{record['objective_um2']:,.0f}", f"{100 * record['density']['overflow']:.1f}%",
            f"{record['density']['max_bin_density']:.2f}", str(record["outside"]["die"]),
            "-" if record["outside"]["own_region"] is None else str(record["outside"]["own_region"]),
            "-" if cog is None else f"{cog:.1e}",
            "-" if move is None else f"{move['median']:.2f}",
            f"{record['seconds']['since_start']:.2f}"]


HEADER = ["frame", "HPWL um", "/input", "objective um2", "overflow", "max dens",
          "off-die", "off-region", "COG err um", "median move", "t (s)"]


COMPARE = ["trace", "HPWL@overflow", "final HPWL", "final overflow", "final off-die", "time (s)"]


def print_table(rows):
    widths = [max(len(r[i]) for r in [HEADER] + rows) for i in range(len(HEADER))]
    for r in [HEADER] + rows:
        print("  ".join(cell.rjust(width) for cell, width in zip(r, widths)))


def hpwl_at_overflow(baseline, solves, target):
    """HPWL / input HPWL where density overflow first reaches TARGET.

    Linear interpolation between the two solves that bracket TARGET; None if
    the run never spreads that far. This compares runs at equal spreading,
    which raw per-iteration HPWL does not (a clumped placement is short).
    """
    base = baseline["hpwl_um"]["all"]
    previous = None
    for record in solves:
        overflow, hpwl = record["density"]["overflow"], record["hpwl_um"]["all"]
        if overflow <= target:
            if previous is None or previous[0] == overflow:
                return hpwl / base
            o0, h0 = previous
            t = (o0 - target) / (o0 - overflow)
            return (h0 + t * (hpwl - h0)) / base
        previous = (overflow, hpwl)
    return None


def objective_monotone(solves):
    """Each level refines the previous constraints (sons' centres average to
    the parent centre), so the optimal objective must not decrease. A drop
    beyond rounding indicates an inaccurate solve or a constraint bug."""
    values = [r["objective_um2"] for r in solves]
    return all(b >= a * (1 - 1e-6) for a, b in zip(values, values[1:]))


def plot(traces, output: Path, target):
    """Six-panel dashboard against the solve index, one line per trace."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(2, 3, figsize=(15, 8.5))
    (hpwl_ax, over_ax, trade_ax), (dens_ax, out_ax, move_ax) = axes
    for name, baseline, solves in traces:
        if not solves:
            continue
        xs = [r["iteration"] for r in solves]
        base = baseline["hpwl_um"]["all"]
        ratio = [r["hpwl_um"]["all"] / base for r in solves]
        # Log axes: clamp at 0.01 % so a fully spread frame stays plottable.
        overflow = [max(100 * r["density"]["overflow"], 0.01) for r in solves]
        line, = hpwl_ax.plot(xs, ratio, marker="o", markersize=3, label=name)
        colour = line.get_color()
        over_ax.plot(xs, overflow, marker="o", markersize=3, color=colour)
        trade_ax.plot(overflow, ratio, marker="o", markersize=3, color=colour)
        at = hpwl_at_overflow(baseline, solves, target)
        if at is not None:
            trade_ax.scatter([100 * target], [at], marker="*", s=140, color=colour, zorder=5)
        dens_ax.plot(xs, [r["density"]["max_bin_density"] for r in solves],
                     marker="o", markersize=3, color=colour)
        out_ax.plot(xs, [r["outside"]["die"] for r in solves], marker="o", markersize=3,
                    color=colour)
        out_ax.plot(xs, [r["outside"]["own_region"] for r in solves], linestyle="--",
                    color=colour)
        moves = [(x, r["movement_um"]["from_previous_solve"]) for x, r in zip(xs, solves)
                 if r["movement_um"]["from_previous_solve"]]
        if moves:
            move_ax.plot([x for x, _ in moves], [m["median"] for _, m in moves],
                         marker="o", markersize=3, color=colour)
            move_ax.plot([x for x, _ in moves], [m["p95"] for _, m in moves],
                         linestyle="--", color=colour)
    hpwl_ax.axhline(1.0, color="0.5", linestyle=":", linewidth=1)
    hpwl_ax.set(title="HPWL / input HPWL", xlabel="Solve")
    over_ax.axhline(100 * target, color="0.5", linestyle=":", linewidth=1)
    over_ax.set(title="Density overflow (%, log)", xlabel="Solve", yscale="log")
    trade_ax.axvline(100 * target, color="0.5", linestyle=":", linewidth=1)
    trade_ax.set(title=f"Trade-off: HPWL vs overflow (★ at {100 * target:g}%)",
                 xlabel="Density overflow (%, log; better spreading to the right)",
                 ylabel="HPWL / input (lower is better)", xscale="log")
    trade_ax.invert_xaxis()
    dens_ax.axhline(1.0, color="0.5", linestyle=":", linewidth=1)
    dens_ax.set(title="Max bin density (cell area / free area)", xlabel="Solve", yscale="log")
    out_ax.set(title="Off die (solid) / outside own region (dashed)", xlabel="Solve")
    move_ax.set(title="Move since previous solve (µm): median / p95 (dashed)",
                xlabel="Solve", yscale="log")
    hpwl_ax.legend(fontsize=7)
    fig.tight_layout()
    fig.savefig(output, dpi=130)
    if output.suffix.lower() != ".pdf":
        fig.savefig(output.with_suffix(".pdf"))  # vector copy
    plt.close(fig)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("traces", nargs="+", type=Path, help="Trace directories")
    parser.add_argument("--plot", type=Path, help="Save a six-panel metrics dashboard to this image")
    parser.add_argument("--at-overflow", type=float, default=0.10,
                        help="Overflow at which runs are compared (fraction, default 0.10)")
    args = parser.parse_args(argv)
    loaded = []
    try:
        for directory in args.traces:
            header, frames = load_metrics(directory)
            solves = [f for f in frames if f["phase"] == "post_solve"]
            loaded.append((directory.name, header, solves))
            print(f"\n{directory}  ({header['run_id']})")
            print(f"  density bins {header['density']['bin_um']:.2f} µm, "
                  f"target {header['density']['target']:.2f}")
            print_table([row(header, header)] + [row(s, header) for s in solves])
            if not objective_monotone(solves):
                print("  WARNING: objective decreased between solves "
                      "(inaccurate solve or constraint bug)")
        print(f"\nComparison (HPWL / input at {100 * args.at_overflow:g}% overflow; final frame)")
        compare = []
        for name, header, solves in loaded:
            at = hpwl_at_overflow(header, solves, args.at_overflow)
            final = solves[-1] if solves else None
            compare.append([name, "not reached" if at is None else f"{at:.3f}",
                            "-" if not final else f"{final['hpwl_um']['all'] / header['hpwl_um']['all']:.3f}",
                            "-" if not final else f"{100 * final['density']['overflow']:.1f}%",
                            "-" if not final else str(final["outside"]["die"]),
                            "-" if not final else f"{final['seconds']['since_start']:.2f}"])
        widths = [max(len(r[i]) for r in compare + [COMPARE]) for i in range(len(COMPARE))]
        for r in [COMPARE] + compare:
            print("  ".join(cell.rjust(width) for cell, width in zip(r, widths)))
        if args.plot:
            plot(loaded, args.plot, args.at_overflow)
            print(f"\n{args.plot}")
    except (MetricsError, KeyError, TypeError) as error:
        parser.exit(1, f"Error: {error}\n")


if __name__ == "__main__":
    main()

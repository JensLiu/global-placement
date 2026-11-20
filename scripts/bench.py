#!/usr/bin/env python3
"""Fair placement benchmark against OpenROAD references (generic over designs).

Every placement -- OpenROAD references and our runs -- goes through the same
minimal pipeline on the same ORFS 3_2_place_iop netlist:

  global placement -> detailed_placement (legaliser only) -> measure
                   -> improve_placement + optimize_mirroring -> measure (secondary)

No port buffering, timing repair, or resizing: those change the netlist.

  reference  Run OpenROAD GPL references once per design; cached under
             build/reference/<platform>/<design>/<variant>/ and reused.
             Variants: gpl (wirelength-driven), gpl_routability.
  evaluate   Legalise and measure a trace's global_placement.odb
             (written by odb_hello --write-db) into TRACE/bench/.
  report     Scorecard: references and traces, ratios to the references.

  python3 scripts/bench.py reference --design picorv32
  python3 scripts/bench.py evaluate build/traces/RUN --design picorv32
  python3 scripts/bench.py report build/traces/RUN_A build/traces/RUN_B --design picorv32
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TCL = ROOT / "scripts" / "bench" / "bench.tcl"
RENDER_TCL = ROOT / "scripts" / "bench" / "render.tcl"
STAGES = ("global_placement", "legal")
# Artefacts a complete placement directory must hold (older caches are rebuilt).
REQUIRED = ("bench.json", "legal_nets.csv")
VARIANTS = {"gpl": "OpenROAD GPL", "gpl_routability": "OpenROAD GPL (routability)"}
# Shared, reusable reference cache: computed once per design, not per run.
REFERENCE_ROOT = ROOT / "build" / "reference"


class BenchError(RuntimeError):
    pass


class Design:
    """Locate a design's ORFS inputs and settings without hard-coding any design."""

    def __init__(self, name, platform, orfs):
        self.name, self.platform, self.orfs = name, platform, Path(orfs)
        self.input = self.orfs / "results" / platform / name / "base" / "3_2_place_iop.odb"
        self.log = self.orfs / "logs" / platform / name / "base" / "3_3_place_gp.log"
        self.platform_dir = self.orfs / "platforms" / platform
        if not self.input.exists():
            raise BenchError(f"{self.input} not found; run ORFS up to 3_2 for {name}")

    def density(self):
        """The target density ORFS's own 3_3 GPL used (GPL-0023), else 'auto'."""
        if self.log.exists():
            values = re.findall(r"GPL-0023\]\s*Placement target density:\s*([0-9.]+)",
                                self.log.read_text(errors="replace"))
            if values:
                return values[-1], f"ORFS log {self.log}"
        return "auto", "uniform density + 0.01 (no ORFS 3_3 log)"

    def routing_env(self):
        """Global-routing setup from the platform config (layers, FASTROUTE_TCL)."""
        config = self.platform_dir / "config.mk"
        env = {}
        if not config.exists():
            return env
        # Platform scripts such as fastroute.tcl read ORFS variables from the
        # environment, so export every simple `export KEY [?:]= VALUE`. Values
        # referring to other Make variables are kept only if they reduce to
        # $(PLATFORM_DIR)/$(PLATFORM).
        for key, value in re.findall(r"^export\s+(\w+)\s*[?:]?=\s*(.*?)\s*$",
                                     config.read_text(errors="replace"), re.M):
            value = value.replace("$(PLATFORM_DIR)", str(self.platform_dir))
            value = value.replace("$(PLATFORM)", self.platform)
            if "$(" not in value and "$$" not in value:
                env[key] = value
        low, high = env.get("MIN_ROUTING_LAYER"), env.get("MAX_ROUTING_LAYER")
        fastroute = env.get("FASTROUTE_TCL")
        if fastroute and Path(fastroute).exists():
            env["BENCH_FASTROUTE_TCL"] = fastroute  # sets layers and adjustments
        elif low and high:
            env["BENCH_ROUTING_LAYERS"] = f"{low}-{high}"
        return env


def sha256(path: Path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def openroad():
    binary = os.environ.get("OPENROAD") or shutil.which("openroad")
    if not binary:
        raise BenchError("openroad not found; set OPENROAD=/path/to/openroad")
    return binary


def run_tcl(env_extra, out_dir: Path):
    env = dict(os.environ, **env_extra, BENCH_OUT_DIR=str(out_dir))
    with (out_dir / "openroad.log").open("w") as log:
        status = subprocess.run([openroad(), "-no_init", "-exit", str(TCL)], env=env,
                                stdout=log, stderr=subprocess.STDOUT).returncode
    result = out_dir / "bench.json"
    if status != 0 or not result.exists():
        raise BenchError(f"OpenROAD failed (exit {status}); see {out_dir / 'openroad.log'}")
    return json.loads(result.read_text())


def render(out_dir: Path):
    """OpenROAD's own offscreen views + heatmap dumps; failures only warn."""
    for stage in STAGES:
        if not (out_dir / f"{stage}.odb").exists():
            continue
        env = dict(os.environ, BENCH_OUT_DIR=str(out_dir), BENCH_STAGE=stage,
                   BENCH_IMAGE_WIDTH=os.environ.get("BENCH_IMAGE_WIDTH", "2000"),
                   QT_QPA_PLATFORM=os.environ.get("QT_QPA_PLATFORM", "offscreen"))
        with (out_dir / f"render_{stage}.log").open("w") as log:
            status = subprocess.run([openroad(), "-gui", "-no_init", "-exit", str(RENDER_TCL)],
                                    env=env, stdout=log, stderr=subprocess.STDOUT).returncode
        if status != 0:
            print(f"  warning: OpenROAD rendering failed for {out_dir}/{stage} "
                  f"(see render_{stage}.log); images skipped", file=sys.stderr)


def rendered(out_dir: Path):
    return all((out_dir / "images" / f"{stage}_rudy.png").exists() for stage in STAGES)


def reference_dir(design: Design, variant):
    return REFERENCE_ROOT / design.platform / design.name / variant


def run_references(design: Design, force=False):
    digest = sha256(design.input)
    density, source = design.density()
    for variant in VARIANTS:
        out = reference_dir(design, variant)
        meta_path = out / "meta.json"
        complete = meta_path.exists() and all((out / name).exists() for name in REQUIRED)
        if complete and not force:
            meta = json.loads(meta_path.read_text())
            if meta.get("input_sha256") != digest:
                raise BenchError(f"{out}: cached reference was built from a different "
                                 f"{design.input.name}; rerun with --force")
            if not rendered(out):
                print(f"{VARIANTS[variant]}: cached; rendering images ...", flush=True)
                render(out)
            print(f"{VARIANTS[variant]}: cached ({out})")
            continue
        if out.exists():
            shutil.rmtree(out)
        out.mkdir(parents=True)
        env = dict(design.routing_env(), BENCH_IN=str(design.input), BENCH_MODE="gpl",
                   BENCH_DENSITY=density,
                   BENCH_ROUTABILITY="1" if variant == "gpl_routability" else "0")
        print(f"{VARIANTS[variant]}: running (density {density}) ...", flush=True)
        result = run_tcl(env, out)
        render(out)
        version = subprocess.run([openroad(), "-version"], capture_output=True, text=True).stdout.strip()
        meta = {"design": design.name, "platform": design.platform, "variant": variant,
                "input": str(design.input), "input_sha256": digest,
                "density": density, "density_source": source, "openroad_version": version}
        meta_path.write_text(json.dumps(meta, indent=2) + "\n")  # last: marks completion
        print(f"  legal HPWL {result.get('legal_hpwl_um', float('nan')):,.0f} µm "
              f"-> {out}")


def evaluate(trace: Path, design: Design):
    placed = trace / "global_placement.odb"
    if not placed.exists():
        raise BenchError(f"{placed} missing; run odb_hello ... --write-db {placed}")
    out = trace / "bench"
    if out.exists():
        shutil.rmtree(out)
    out.mkdir()
    # The legalise mode starts from our placement; keep a copy beside the
    # references' layout so both render identically.
    shutil.copyfile(placed, out / "global_placement.odb")
    result = run_tcl(dict(design.routing_env(), BENCH_IN=str(placed), BENCH_MODE="legalise"), out)
    render(out)
    meta = {"design": design.name, "platform": design.platform, "input_sha256": sha256(design.input)}
    (out / "meta.json").write_text(json.dumps(meta, indent=2) + "\n")
    print(f"legal HPWL {result.get('legal_hpwl_um', float('nan')):,.0f} µm -> {out}")


def gp_seconds_from_trace(trace: Path):
    """Our global-placement time: the tracer's last since_start (solver only)."""
    path = trace / "metrics.jsonl"
    if not path.exists():
        return None
    last = None
    for line in path.read_text().splitlines():
        try:
            last = json.loads(line)
        except json.JSONDecodeError:
            break
    return last["seconds"]["since_start"] if last else None


def read_csv_column(path: Path, column):
    import csv
    if not path.exists():
        return []
    with path.open() as stream:
        return [float(row[column]) for row in csv.DictReader(stream)]


def percentile(values, q):
    if not values:
        return None
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(q * (len(ordered) - 1) + 0.5))]


def congestion(directory: Path):
    """RUDY (OpenROAD's estimated routing congestion, %) of the legal placement."""
    values = read_csv_column(directory / "heatmaps" / "legal_rudy.csv", "value (%)")
    if not values:
        return None
    return {"p99": percentile(values, 0.99), "max": max(values),
            "over100": sum(v > 100 for v in values) / len(values), "values": values}


def placements(traces, design: Design):
    """References first, then traces, each as {label, dir, result, seconds}."""
    items = []
    for variant, label in VARIANTS.items():
        directory = reference_dir(design, variant)
        if (directory / "bench.json").exists():
            result = json.loads((directory / "bench.json").read_text())
            items.append({"label": label, "dir": directory, "result": result,
                          "seconds": result.get("gp_seconds"), "variant": variant})
    if not items:
        raise BenchError(f"no references for {design.name}; run: bench.py reference --design {design.name}")
    digest = sha256(design.input)
    for trace in traces:
        directory = trace / "bench"
        if not (directory / "bench.json").exists():
            raise BenchError(f"{directory}/bench.json missing; run: bench.py evaluate {trace} --design {design.name}")
        meta = json.loads((directory / "meta.json").read_text())
        if meta.get("design") != design.name or meta.get("input_sha256") != digest:
            raise BenchError(f"{trace} was benchmarked on a different design/input")
        items.append({"label": trace.name, "dir": directory,
                      "result": json.loads((directory / "bench.json").read_text()),
                      "seconds": gp_seconds_from_trace(trace), "variant": None})
    return items


def report(traces, design: Design):
    items = placements(traces, design)
    references = {item["variant"]: item["result"] for item in items if item["variant"]}

    def ratio(value, variant, key):
        ref = references.get(variant, {}).get(key)
        return "-" if value is None or not ref else f"{value / ref:.3f}"

    header = ["placement", "GP HPWL", "legal HPWL", "x GPL", "x GPL-R", "legalise dHPWL",
              "mean disp", "max disp", "legal", "RUDY p99", "RUDY>100%",
              "+DPO HPWL", "x GPL (DPO)", "GP s"]
    table = []
    for item in items:
        r, seconds = item["result"], item["seconds"]
        gp, legal, dpo = r.get("gp_hpwl_um"), r.get("legal_hpwl_um"), r.get("dpo_hpwl_um")
        disp = r.get("displacement_um") or {}
        rudy = congestion(item["dir"])
        table.append([item["label"], f"{gp:,.0f}" if gp else "-",
                      f"{legal:,.0f}" if legal else "FAILED",
                      ratio(legal, "gpl", "legal_hpwl_um"),
                      ratio(legal, "gpl_routability", "legal_hpwl_um"),
                      f"{100 * (legal / gp - 1):+.1f}%" if gp and legal else "-",
                      f"{disp['mean']:.2f}" if disp else "-", f"{disp['max']:.2f}" if disp else "-",
                      "yes" if r.get("legal") else "NO",
                      f"{rudy['p99']:.0f}%" if rudy else "-",
                      f"{100 * rudy['over100']:.1f}%" if rudy else "-",
                      f"{dpo:,.0f}" if dpo else "-",
                      ratio(dpo, "gpl", "dpo_hpwl_um"),
                      f"{seconds:.2f}" if seconds is not None else "-"])
    widths = [max(len(row[i]) for row in [header] + table) for i in range(len(header))]
    print(f"\n{design.platform}/{design.name}: HPWL and displacement in µm; x = legal HPWL ratio "
          f"to the reference (lower is better); RUDY = estimated routing congestion of the "
          f"legal placement (p99 of bins; share of bins above 100 %)")
    for row in [header] + table:
        print("  ".join(cell.rjust(width) if i else cell.ljust(width)
                        for i, (cell, width) in enumerate(zip(row, widths))))


LENGTH_BUCKETS = [(0, 5), (5, 20), (20, 50), (50, 100), (100, float("inf"))]


def visualise(traces, design: Design, output_dir: Path):
    """Side-by-side OpenROAD views and quantitative comparison charts."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.image as mpimg
    import matplotlib.pyplot as plt

    items = placements(traces, design)
    names = [item["label"] if item["variant"] else f"{item['label'][:28]}…"
             if len(item["label"]) > 29 else item["label"] for item in items]
    output_dir.mkdir(parents=True, exist_ok=True)

    # 1. OpenROAD's own views, one column per placement, identical colour scales.
    rows = [("legal_cells", "Legal placement (OpenROAD view)"),
            ("global_placement_density", "Global placement density, 0-100 %"),
            ("legal_rudy", "Legal: estimated congestion (RUDY), 0-120 %")]
    fig, axes = plt.subplots(len(rows), len(items), figsize=(4.2 * len(items), 4.4 * len(rows)),
                             squeeze=False)
    for r, (stem, title) in enumerate(rows):
        for c, item in enumerate(items):
            ax = axes[r][c]
            ax.axis("off")
            path = item["dir"] / "images" / f"{stem}.png"
            if path.exists():
                ax.imshow(mpimg.imread(path))
            else:
                ax.text(0.5, 0.5, "not rendered", ha="center", va="center")
            if r == 0:
                ax.set_title(names[c], fontsize=10)
        axes[r][0].text(-0.04, 0.5, title, transform=axes[r][0].transAxes, rotation=90,
                        ha="right", va="center", fontsize=10)
    fig.suptitle(f"{design.platform}/{design.name}: side-by-side OpenROAD views", fontsize=13)
    fig.tight_layout()
    layout = output_dir / "compare_layout.png"
    fig.savefig(layout, dpi=220)  # OpenROAD renders are raster; keep their detail
    plt.close(fig)

    # 2. Quantitative comparison.
    gpl = next(item["result"] for item in items if item["variant"] == "gpl")
    gpl_rudy = congestion(next(item["dir"] for item in items if item["variant"] == "gpl"))
    fig, ((bars, cdf), (tail, share)) = plt.subplots(2, 2, figsize=(14, 10))
    metrics = [("legal HPWL", lambda r, d: r.get("legal_hpwl_um"), gpl.get("legal_hpwl_um")),
               ("+DPO HPWL", lambda r, d: r.get("dpo_hpwl_um"), gpl.get("dpo_hpwl_um")),
               ("mean disp.", lambda r, d: (r.get("displacement_um") or {}).get("mean"),
                (gpl.get("displacement_um") or {}).get("mean")),
               ("max disp.", lambda r, d: (r.get("displacement_um") or {}).get("max"),
                (gpl.get("displacement_um") or {}).get("max")),
               ("RUDY p99", lambda r, d: (congestion(d) or {}).get("p99"),
                (gpl_rudy or {}).get("p99"))]
    width = 0.8 / len(items)
    for i, item in enumerate(items):
        values = []
        for _, get, ref in metrics:
            value = get(item["result"], item["dir"])
            values.append(value / ref if value and ref else 0)
        positions = [m + (i - (len(items) - 1) / 2) * width for m in range(len(metrics))]
        bars.bar(positions, values, width, label=names[i])
        for x, v in zip(positions, values):
            bars.text(x, v, f"{v:.2f}", ha="center", va="bottom", fontsize=7)
    bars.axhline(1.0, color="0.4", linestyle=":")
    bars.set_xticks(range(len(metrics)), [m[0] for m in metrics])
    bars.set(title="Ratio to OpenROAD GPL (1.0 = equal; lower is better)", yscale="log")
    bars.legend(fontsize=8)

    for i, item in enumerate(items):
        rudy = congestion(item["dir"])
        if rudy:
            values = sorted(rudy["values"])
            n = len(values)
            cdf.plot(values, [1 - k / n for k in range(n)], label=names[i])
    cdf.axvline(100, color="0.4", linestyle=":")
    cdf.set(title="Congestion (RUDY): share of bins at or above x (lower curve is better)",
            xlabel="RUDY congestion of a bin (%)", ylabel="share of bins", yscale="log")
    cdf.legend(fontsize=8)

    for i, item in enumerate(items):
        lengths = sorted(read_csv_column(item["dir"] / "legal_nets.csv", "hpwl_um"))
        if lengths:
            n = len(lengths)
            tail.plot(lengths, [1 - k / n for k in range(n)], label=names[i])
    tail.set(title="Net length distribution (legal): share of nets at least x long",
             xlabel="net HPWL (µm)", ylabel="share of nets", xscale="log", yscale="log")
    tail.legend(fontsize=8)

    labels = [f"{lo:g}-{hi:g}" if hi != float("inf") else f">{lo:g}" for lo, hi in LENGTH_BUCKETS]
    width = 0.8 / len(items)
    for i, item in enumerate(items):
        lengths = read_csv_column(item["dir"] / "legal_nets.csv", "hpwl_um")
        sums = [sum(v for v in lengths if lo <= v < hi) for lo, hi in LENGTH_BUCKETS]
        positions = [b + (i - (len(items) - 1) / 2) * width for b in range(len(LENGTH_BUCKETS))]
        share.bar(positions, [s / 1000 for s in sums], width, label=names[i])
    share.set_xticks(range(len(labels)), labels)
    share.set(title="Where the wirelength is: total HPWL by net length (legal)",
              xlabel="net HPWL bucket (µm)", ylabel="total HPWL (mm)")
    share.legend(fontsize=8)
    fig.suptitle(f"{design.platform}/{design.name}: quantitative comparison", fontsize=13)
    fig.tight_layout()
    charts = output_dir / "compare_metrics.png"
    fig.savefig(charts, dpi=110)
    fig.savefig(charts.with_suffix(".pdf"))  # vector copy
    plt.close(fig)
    print(f"{layout}\n{charts}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("reference", "evaluate", "report", "visualise"):
        p = sub.add_parser(name)
        p.add_argument("--design", required=True, help="ORFS design name, e.g. gcd, picorv32, bp")
        p.add_argument("--platform", default="nangate45")
        p.add_argument("--orfs", default=str(ROOT / "OpenROAD-flow-scripts" / "flow"),
                       help="ORFS flow directory (results/, logs/, platforms/)")
        if name == "reference":
            p.add_argument("--force", action="store_true", help="Rebuild cached references")
        else:
            p.add_argument("traces", nargs="*" if name in ("report", "visualise") else 1, type=Path)
        if name == "visualise":
            p.add_argument("--output", type=Path,
                           help="Default: the first trace's bench/ (or the reference cache)")
    args = parser.parse_args(argv)
    try:
        design = Design(args.design, args.platform, args.orfs)
        if args.command == "reference":
            run_references(design, args.force)
        elif args.command == "evaluate":
            evaluate(args.traces[0], design)
        elif args.command == "report":
            report(args.traces, design)
        else:
            output = args.output or (args.traces[0] / "bench" if args.traces
                                     else reference_dir(design, "gpl").parent)
            visualise(args.traces, design, output)
    except BenchError as error:
        parser.exit(1, f"Error: {error}\n")


if __name__ == "__main__":
    sys.exit(main())

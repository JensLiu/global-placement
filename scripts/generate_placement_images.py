#!/usr/bin/env python3
"""Generate images from a placement JSON trace without opening a window.

Export every completed frame to DIRECTORY/images/, as a PNG preview and vector
PDF by default. Frames render in separate processes (up to eight by default);
--jobs 1 selects serial rendering. Each frame is drawn once and saved in every
requested format. Existing images with the same names are replaced.
Requires Matplotlib. Version 3 names include the solve/partition phase.
"""
from __future__ import annotations

import argparse
from concurrent.futures import FIRST_COMPLETED, ProcessPoolExecutor, wait
import multiprocessing
import os
from pathlib import Path

from plot_placement_trace import PlacementTrace, TraceViewer, frame_stem, require


_worker = None


def default_jobs():
    """Respect CPU affinity, but cap automatic concurrency to bound image memory."""
    try:
        cpus = len(os.sched_getaffinity(0))
    except AttributeError:
        cpus = os.cpu_count() or 1
    return max(1, min(8, cpus))


def initialise_worker(design, modules, options):
    """Initialise once per process, without copying the full frame history."""
    global _worker
    # Rendering processes already provide CPU parallelism. Avoid multiplying
    # their count by a BLAS/OpenMP thread pool inherited from the placer run.
    for variable in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS",
                     "NUMEXPR_NUM_THREADS"):
        os.environ[variable] = "1"
    import matplotlib
    matplotlib.use("Agg")
    _worker = {"design": design, "modules": modules, "options": options, "viewer": None}


def close_worker():
    if _worker is not None and _worker["viewer"] is not None:
        import matplotlib.pyplot as plt
        plt.close(_worker["viewer"].fig)
        _worker["viewer"] = None


def render_frame(index, frame, metrics):
    """Render a single frame, keeping its original name, title and metrics."""
    options = _worker["options"]
    # Static viewports depend on die bounds, not on other frames. Retaining
    # just this frame avoids a complete trace copy in every spawned worker.
    trace = PlacementTrace(_worker["design"], _worker["modules"], [frame],
                           {0: metrics} if metrics is not None else {})
    viewer = _worker["viewer"]
    if viewer is None:
        viewer = TraceViewer(trace, 0, options.bbox, not options.no_colour,
                             options.drift, interactive=False, area=not options.no_area,
                             scale=options.scale)
        _worker["viewer"] = viewer
    else:
        viewer.trace = trace
        viewer.set_frame(0)
    paths = []
    for fmt in options.format:
        output = options.output_dir / f"{frame_stem(index, frame)}.{fmt}"
        viewer.fig.savefig(output, dpi=options.dpi)
        paths.append(output)
    return paths


def export_frames(trace, indices, options, jobs):
    """Bound in-flight frames to the worker count, including on older Python."""
    if jobs == 1:
        initialise_worker(trace.design, trace.modules, options)
        try:
            return {index: render_frame(index, trace.frames[index], trace.metrics.get(index))
                    for index in indices}
        finally:
            close_worker()
    results = {}
    # Spawn avoids inheriting Matplotlib state, GUI handles or thread locks.
    with ProcessPoolExecutor(max_workers=jobs, mp_context=multiprocessing.get_context("spawn"),
                             initializer=initialise_worker,
                             initargs=(trace.design, trace.modules, options)) as pool:
        remaining = iter(indices)
        pending = {}

        def submit(index):
            future = pool.submit(render_frame, index, trace.frames[index], trace.metrics.get(index))
            pending[future] = index

        for _ in range(jobs):
            submit(next(remaining))
        try:
            while pending:
                completed, _ = wait(pending, return_when=FIRST_COMPLETED)
                for future in completed:
                    index = pending.pop(future)
                    results[index] = future.result()
                    next_index = next(remaining, None)
                    if next_index is not None:
                        submit(next_index)
        except BaseException:
            for future in pending:
                future.cancel()
            raise
    return results


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="Trace containing design.json and iterations.jsonl")
    parser.add_argument("--output-dir", type=Path, help="Default: DIRECTORY/images")
    parser.add_argument("--frame", type=int, help="Export only this zero-based frame index")
    parser.add_argument("--jobs", "-j", type=int, default=0,
                        help="Worker processes: 0 = auto (up to 8), 1 = serial (default: 0)")
    parser.add_argument("--format", nargs="+", choices=("png", "pdf", "svg"),
                        default=["png", "pdf"],
                        help="Formats to save for each frame (default: png pdf)")
    parser.add_argument("--dpi", type=int, default=180)
    parser.add_argument("--bbox", action="store_true", help="Draw movable-cell footprints")
    parser.add_argument("--no-colour", action="store_true", help="Disable region colouring")
    parser.add_argument("--drift", action="store_true", help="Mark centres outside assigned regions")
    parser.add_argument("--scale", type=float,
                        help="Figure-size multiplier (default: grows with the number of cells)")
    parser.add_argument("--no-area", action="store_true",
                        help="Hide translucent area discs on partition frames (shown by default)")
    args = parser.parse_args(argv)
    try:
        require(args.dpi > 0, "DPI must be positive")
        require(args.jobs >= 0, "Jobs must be nonnegative (0 = auto)")
        trace = PlacementTrace.load(args.directory)
        if args.frame is not None:
            require(0 <= args.frame < len(trace.frames), "Frame index out of range")
        indices = list(range(len(trace.frames))) if args.frame is None else [args.frame]
        args.output_dir = args.output_dir or args.directory / "images"
        args.output_dir.mkdir(parents=True, exist_ok=True)
        args.format = list(dict.fromkeys(args.format))
        jobs = min(args.jobs or default_jobs(), len(indices))
        results = export_frames(trace, indices, args, jobs)
        # Keep stdout ordered, independent of process completion order.
        for index in indices:
            for path in results[index]:
                print(path)
    except (OSError, ValueError, ImportError, RuntimeError) as error:
        parser.exit(1, f"Error: {error}\n")


if __name__ == "__main__":
    main()

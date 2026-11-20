#!/usr/bin/env python3
"""Generate images from a placement JSON trace without opening a window.

By default export every completed frame to DIRECTORY/images/, both as a PNG
preview and as a vector PDF (sharp at any zoom, about 1 MB per picorv32 frame).
Version 3 traces use frame-NNNNNN-solve.* and frame-NNNNNN-partition.*; older
traces use iteration-NNNNNN.*. Use --format png (or several formats) to choose.
Requires Matplotlib. Existing images with the same names are replaced.
"""
from __future__ import annotations

import argparse
from pathlib import Path

from plot_placement_trace import PlacementTrace, TraceViewer, frame_stem, require


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="Trace containing design.json and iterations.jsonl")
    parser.add_argument("--output-dir", type=Path, help="Default: DIRECTORY/images")
    parser.add_argument("--frame", type=int, help="Export only this zero-based iteration")
    parser.add_argument("--format", nargs="+", choices=("png", "pdf", "svg"),
                        default=["png", "pdf"],
                        help="One or more formats; each frame is rendered once and saved "
                             "in all of them (default: png pdf)")
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
        trace = PlacementTrace.load(args.directory)
        if args.frame is not None:
            require(0 <= args.frame < len(trace.frames), "Frame index out of range")
        frames = range(len(trace.frames)) if args.frame is None else [args.frame]
        output_dir = args.output_dir or args.directory / "images"
        output_dir.mkdir(parents=True, exist_ok=True)
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        viewer = TraceViewer(trace, frames[0], args.bbox, not args.no_colour,
                             args.drift, interactive=False, area=not args.no_area,
                             scale=args.scale)
        try:
            for index in frames:
                if index != viewer.index:
                    viewer.set_frame(index)
                for fmt in args.format:
                    output = output_dir / f"{frame_stem(index, trace.frames[index])}.{fmt}"
                    viewer.fig.savefig(output, dpi=args.dpi)
                    print(output)
        finally:
            plt.close(viewer.fig)
    except (OSError, ValueError, ImportError) as error:
        parser.exit(1, f"Error: {error}\n")


if __name__ == "__main__":
    main()

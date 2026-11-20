#!/usr/bin/env python3
"""Open a placement trace in an interactive Matplotlib window.

Scroll to zoom at the pointer; use the toolbar to pan or box-zoom. Left/right
change iteration; Home or r resets the view. The viewport is retained when
changing iterations or display options. Requires Matplotlib and a GUI backend
such as TkAgg (Tk) or QtAgg (PyQt/PySide), plus a graphical desktop/display.
"""
from __future__ import annotations

import argparse
from pathlib import Path

from plot_placement_trace import PlacementTrace, TraceViewer, require


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="Trace containing design.json and iterations.jsonl")
    parser.add_argument("--frame", type=int, default=0, help="Initial zero-based iteration")
    parser.add_argument("--bbox", action="store_true", help="Show movable-cell footprints initially")
    parser.add_argument("--no-colour", action="store_true", help="Disable region colouring initially")
    parser.add_argument("--drift", action="store_true", help="Mark centres outside assigned regions")
    parser.add_argument("--no-area", action="store_true",
                        help="Hide translucent area discs on partition frames (shown by default)")
    parser.add_argument("--backend", help="Optional Matplotlib GUI backend, e.g. TkAgg or QtAgg")
    args = parser.parse_args(argv)
    try:
        trace = PlacementTrace.load(args.directory)
        require(0 <= args.frame < len(trace.frames), "Frame index out of range")
        import matplotlib
        if args.backend:
            matplotlib.use(args.backend)
        import matplotlib.pyplot as plt
        # Do not silently succeed without a window on a headless machine.
        from matplotlib.backends.registry import backend_registry
        _, gui = backend_registry.resolve_backend(matplotlib.get_backend())
        require(gui is not None,
                "No interactive GUI backend available. Use --backend TkAgg or QtAgg "
                "on a graphical desktop, or use generate_placement_images.py.")
        viewer = TraceViewer(trace, args.frame, args.bbox, not args.no_colour, args.drift,
                             area=not args.no_area)
        plt.show()
        # Keep the viewer (and its widget callbacks) alive until the window closes.
        return viewer
    except (OSError, ValueError, ImportError, RuntimeError) as error:
        parser.exit(1, f"Error: {error}\n")


if __name__ == "__main__":
    main()

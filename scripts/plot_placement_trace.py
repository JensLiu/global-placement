#!/usr/bin/env python3
"""Shared trace parser/renderer and backwards-compatible combined CLI.

Reads trace schema versions 1 (frame-local region IDs), 2 (run-scoped
region IDs with parent/depth lineage and per-level partition records) and 3
(version 2 plus a separate "post_partition" frame after each partitioning
step, showing the unchanged solved positions with the new regions).

Preferred entry points:
  python3 scripts/generate_placement_images.py trace/run-001 --bbox
  python3 scripts/view_placement_trace.py trace/run-001 --bbox
The original --save CLI remains available for existing callers.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import json
import math
from pathlib import Path
import warnings


class TraceError(ValueError):
    """Invalid or inconsistent trace data."""


def require(condition, message):
    if not condition:
        raise TraceError(message)


def number(value):
    return type(value) in (int, float) and math.isfinite(value)


def identifier(value):
    require(type(value) is int and value >= 0, "IDs/iterations must be nonnegative integers")
    return value


def vector(value, length):
    require(isinstance(value, list) and len(value) == length and all(map(number, value)),
            f"Expected {length} finite coordinates")
    return value


def bounds(value):
    vector(value, 4)
    require(value[0] <= value[2] and value[1] <= value[3], "Inverted bounds")
    return value


SUPPORTED_VERSIONS = {1: "frame", 2: "run", 3: "run"}  # version -> region_id_scope


def objects_by_id(records):
    require(isinstance(records, list), "Expected a list of records")
    result = {}
    for record in records:
        require(isinstance(record, dict), "Expected object record")
        key = identifier(record["id"])
        require(key not in result, f"Duplicate ID {key}")
        result[key] = record
    return result


def reject_constant(value):
    raise TraceError(f"Non-standard JSON constant: {value}")


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def decode(text):
    return json.loads(text, parse_constant=reject_constant, object_pairs_hook=unique_object)


@dataclass
class PlacementTrace:
    design: dict
    modules: dict
    frames: list
    metrics: dict = field(default_factory=dict)  # frame index -> metrics.jsonl record

    @property
    def version(self):
        return self.design["version"]

    @property
    def run_scoped_regions(self):
        """Version 2 region IDs persist across frames; version 1 IDs are frame-local."""
        return self.design["region_id_scope"] == "run"

    @classmethod
    def load(cls, directory: Path):
        try:
            design = decode((directory / "design.json").read_text(encoding="utf-8"))
            version = design["version"]
            require(design["schema"] == "global-placement.gordian-trace" and
                    type(version) is int and version in SUPPORTED_VERSIONS,
                    "Unsupported trace schema/version")
            require(isinstance(design["run_id"], str) and design["run_id"], "Missing run ID")
            require(isinstance(design["design"], str), "Invalid design name")
            require(design["region_id_scope"] == SUPPORTED_VERSIONS[version],
                    "Unsupported region ID scope")
            coords = design["coordinates"]
            require(coords["units"] == "dbu" and coords["cell_reference"] == "bbox_centre",
                    "Unsupported coordinate convention")
            require(number(coords["dbu_per_micron"]) and coords["dbu_per_micron"] > 0,
                    "Invalid DBU scale")
            bounds(design["core_bounds"])
            bounds(design["die_bounds"])
            modules = objects_by_id(design["modules"])
            for module in modules.values():
                require(isinstance(module["name"], str), "Invalid module name")
                require(module["kind"] in ("movable", "fixed", "io"), "Unknown module kind")
                box = bounds(module["bbox_offsets"])
                require(box[0] == -box[2] and box[1] == -box[3],
                        "Footprint offsets must be centred")
                vector(module["initial_centre"], 2)
            movable_ids = {key for key, module in modules.items() if module["kind"] == "movable"}
            frames = []
            # Read a finished trace. An interrupted final line is not a committed frame.
            with (directory / "iterations.jsonl").open(encoding="utf-8") as stream:
                for line_number, line in enumerate(stream, 1):
                    if not line.endswith("\n"):
                        warnings.warn(f"Ignoring incomplete final frame at line {line_number}")
                        break
                    frame = decode(line)
                    require(frame["run_id"] == design["run_id"], "Run ID mismatch")
                    if version >= 3:
                        require(identifier(frame["frame"]) == len(frames),
                                "Frames must be consecutive from zero")
                        require(frame["phase"] in ("post_solve", "post_partition"),
                                "Unsupported frame phase")
                        solves = sum(f["phase"] == "post_solve" for f in frames)
                        if frame["phase"] == "post_solve":
                            require(identifier(frame["iteration"]) == solves,
                                    "Solve iterations must be consecutive from zero")
                        else:
                            expected = solves - 1 if solves else None
                            require(frame["iteration"] == expected,
                                    "A partition frame must show the latest solve")
                    else:
                        require(identifier(frame["iteration"]) == len(frames),
                                "Iterations must be consecutive from zero")
                        require(frame["phase"] == "post_solve", "Unsupported frame phase")
                    regions = objects_by_id(frame["regions"])
                    for region in regions.values():
                        bounds(region["bounds"])
                    positions = objects_by_id(frame["modules"])
                    require(set(positions) == movable_ids, "Frame must contain every movable module exactly once")
                    for position in positions.values():
                        require(identifier(position["region_id"]) in regions, "Unknown region reference")
                        require(number(position["x"]) and number(position["y"]), "Nonfinite position")
                    if version >= 2:
                        check_hierarchy(frame, regions, positions, modules)
                    else:
                        frame["partitions"] = []
                    frames.append(frame)
            require(frames, "Trace has no completed frames")
            return cls(design, modules, frames, load_frame_metrics(directory, design))
        except (KeyError, TypeError, json.JSONDecodeError) as error:
            raise TraceError(f"Malformed trace: {error}") from error

    def die_limits(self):
        """Die and core bounds with a 4 % margin (static exports)."""
        boxes = [self.design["core_bounds"], self.design["die_bounds"]]
        xmin, ymin = min(b[0] for b in boxes), min(b[1] for b in boxes)
        xmax, ymax = max(b[2] for b in boxes), max(b[3] for b in boxes)
        pad = max(xmax - xmin, ymax - ymin) * 0.04
        return xmin - pad, xmax + pad, ymin - pad, ymax + pad

    def limits(self):
        """Fixed viewport across all frames, including out-of-region cells."""
        boxes = [self.design["core_bounds"], self.design["die_bounds"]]
        for module in self.modules.values():
            boxes.append(translated_bbox(module, *module["initial_centre"]))
        for frame in self.frames:
            boxes.extend(region["bounds"] for region in frame["regions"])
            boxes.extend(translated_bbox(self.modules[p["id"]], p["x"], p["y"])
                         for p in frame["modules"])
        xmin, ymin = min(b[0] for b in boxes), min(b[1] for b in boxes)
        xmax, ymax = max(b[2] for b in boxes), max(b[3] for b in boxes)
        pad = max(xmax - xmin, ymax - ymin, 1) * 0.04
        return xmin - pad, xmax + pad, ymin - pad, ymax + pad


def load_frame_metrics(directory: Path, design):
    """Optional benchmark metrics keyed by frame; problems only warn."""
    path = directory / "metrics.jsonl"
    if not path.exists():
        return {}
    result = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
        header = json.loads(lines[0]) if lines else {}
        if header.get("run_id") != design["run_id"]:
            warnings.warn("metrics.jsonl belongs to a different run; ignored")
            return {}
        for line in lines[1:]:
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                break  # interrupted final line
            if isinstance(record.get("frame"), int):
                result[record["frame"]] = record
    except (OSError, ValueError, AttributeError) as error:
        warnings.warn(f"metrics.jsonl unreadable: {error}")
        return {}
    return result


def metrics_label(record):
    """One compact title line from a metrics.jsonl record."""
    return (f"HPWL {record['hpwl_um']['all']:,.0f} µm; "
            f"density overflow {100 * record['density']['overflow']:.1f}% "
            f"(bins {record['density']['bin_um']:.1f} µm); off-die {record['outside']['die']}")


def module_area(module):
    a, b, c, d = module["bbox_offsets"]
    return (c - a) * (d - b)


def check_hierarchy(frame, regions, positions, modules):
    """Validate version 2 region lineage and this level's partition records."""
    for region in regions.values():
        parent = region["parent_id"]
        require(parent is None or identifier(parent) != region["id"], "Region is its own parent")
        identifier(region["depth"])
        require((parent is None) == (region["depth"] == 0), "Only the root region has no parent")
    members = {key: [] for key in regions}
    for position in positions.values():
        members[position["region_id"]].append(position["id"])
    partitions = frame["partitions"]
    require(isinstance(partitions, list), "Partitions must be a list")
    seen = set()
    for record in partitions:
        parent = identifier(record["parent_id"])
        require(parent not in seen, f"Region {parent} partitioned twice in one level")
        seen.add(parent)
        box = bounds(record["parent_bounds"])
        require(record["direction"] in ("vertical", "horizontal"), "Unknown cut direction")
        require(type(record["movable_count"]) is int and record["movable_count"] >= 0,
                "Invalid movable count")
        require(number(record["movable_area"]) and record["movable_area"] >= 0, "Invalid movable area")
        require(type(record["split"]) is bool, "Partition split flag must be boolean")
        if not record["split"]:
            require(not {"cut", "child_ids"} & record.keys(), "Unsplit region has cut data")
            require(parent in regions, f"Unsplit region {parent} missing from frame")
            require(regions[parent]["bounds"] == box, f"Unsplit region {parent} changed bounds")
            require(len(members[parent]) == record["movable_count"],
                    f"Region {parent} movable count disagrees with membership")
            continue
        cut = record["cut"]
        low, high = (0, 2) if record["direction"] == "vertical" else (1, 3)
        require(number(cut) and box[low] <= cut <= box[high], "Cut outside parent bounds")
        children = record["child_ids"]
        counts = record["child_movable_counts"]
        areas = record["child_movable_areas"]
        require(isinstance(children, list) and len(children) == 2 and
                isinstance(counts, list) and len(counts) == 2 and
                isinstance(areas, list) and len(areas) == 2, "A split must have two children")
        require(sum(counts) == record["movable_count"], "Child movable counts do not sum to parent")
        for child, count, area in zip(children, counts, areas):
            require(identifier(child) in regions, f"Child region {child} missing from frame")
            require(regions[child]["parent_id"] == parent, f"Region {child} has the wrong parent")
            require(len(members[child]) == count,
                    f"Region {child} movable count disagrees with membership")
            actual = sum(module_area(modules[key]) for key in members[child])
            require(number(area) and math.isclose(area, actual, rel_tol=1e-9, abs_tol=1e-6),
                    f"Region {child} movable area disagrees with footprints")


AUTO_SCALE_CELLS = 2000  # designs up to this many movable cells use the base size
MAX_AUTO_SCALE = 4.0


def auto_scale(trace):
    """Figure-size multiplier: sqrt(movable cells / 2000), clamped to [1, 4].

    Keeps the average spacing between cell markers roughly constant in pixels:
    gcd (505 cells) -> 1.0, picorv32 (16 592) -> 2.9, bp (~210 k) -> 4.0.
    """
    movable = sum(module["kind"] == "movable" for module in trace.modules.values())
    return min(max(math.sqrt(movable / AUTO_SCALE_CELLS), 1.0), MAX_AUTO_SCALE)


def frame_label(frame):
    """Human-readable frame description shared by titles and file names."""
    iteration = frame["iteration"]
    if frame["phase"] == "post_partition":
        if iteration is None:
            return "initial partition (before any solve)"
        return f"iteration {iteration}, after partitioning (regions for solve {iteration + 1})"
    if "frame" in frame:
        return f"iteration {iteration}, after solve"
    return f"GORDIAN iteration {iteration}"


def frame_stem(index, frame):
    """Image file stem; version 3 names include the phase."""
    if "frame" not in frame:
        return f"iteration-{index:06d}"
    phase = "partition" if frame["phase"] == "post_partition" else "solve"
    return f"frame-{index:06d}-{phase}"


def cut_segment(record):
    """Return a split's cut as ((x0, x1), (y0, y1)) clipped to its parent."""
    a, b, c, d = record["parent_bounds"]
    cut = record["cut"]
    if record["direction"] == "vertical":
        return (cut, cut), (b, d)
    return (a, c), (cut, cut)


def translated_bbox(module, x, y):
    a, b, c, d = module["bbox_offsets"]
    return [x + a, y + b, x + c, y + d]


def outside_region(position, region):
    a, b, c, d = region["bounds"]
    return not (a <= position["x"] <= c and b <= position["y"] <= d)


class TraceViewer:
    """Shared rendering for image export and interactive trace replay."""

    def __init__(self, trace, frame=0, bbox=False, colour=True, drift=False,
                 interactive=True, area=True, scale=None):
        import matplotlib.pyplot as plt
        from matplotlib.widgets import Button, CheckButtons, Slider

        self.trace = trace
        # Exports frame the die (outliers are drawn at the border); the window
        # keeps the full extent so outliers can be inspected by zooming out.
        self.viewport = trace.limits() if interactive else trace.die_limits()
        self.index = frame
        self.options = {"Bounding boxes": bbox, "Region colours": colour,
                        "Region boundaries": True,
                        # Off by default: labels clutter once regions multiply.
                        "Region labels": False,
                        "Outside centres": drift,
                        "Region centroids": False,
                        # Partition frames only: translucent discs sized by area.
                        "Area markers": area,
                        "Cut lines": trace.version >= 2}
        self.selected = None
        self._rendered = False
        # Static exports grow with the design so that dots stay distinguishable:
        # markers and fonts keep their physical size while the plot gets larger.
        self.scale = 1.0 if interactive else (scale or auto_scale(trace))
        # Larger exports get proportionally smaller markers so dots stay finer
        # than the spacing between cells (true footprints carry the detail).
        self.marker_factor = self.scale ** -1.5
        if interactive:
            self.fig, self.ax = plt.subplots(figsize=(11, 8))
        else:
            # Shape the figure like the plotted area so no space is wasted;
            # margins are fixed in inches (room for title, labels and legend).
            xmin, xmax, ymin, ymax = self.viewport
            aspect = (xmax - xmin) / max(ymax - ymin, 1)
            plot_h = 7.0 * self.scale
            plot_w = plot_h * aspect
            left, right, bottom, top = 0.9, 0.3, 0.7, 1.1
            width, height = plot_w + left + right, plot_h + bottom + top
            self.fig, self.ax = plt.subplots(figsize=(width, height))
        self.slider = None
        if not interactive:
            self.fig.subplots_adjust(left=left / width, right=1 - right / width,
                                     bottom=bottom / height, top=1 - top / height)
            self.render()
            return
        self.fig.subplots_adjust(left=0.09, right=0.77, bottom=0.24, top=0.90)
        self.controls = CheckButtons(self.fig.add_axes([0.79, 0.48, 0.20, 0.29]),
                                     list(self.options), list(self.options.values()))
        self.controls.on_clicked(self.toggle)
        self.previous = Button(self.fig.add_axes([0.09, 0.04, 0.12, 0.05]), "Previous")
        self.next = Button(self.fig.add_axes([0.23, 0.04, 0.12, 0.05]), "Next")
        self.previous.on_clicked(lambda _: self.move(-1))
        self.next.on_clicked(lambda _: self.move(1))
        self.slider = None
        if len(trace.frames) > 1:
            self.slider = Slider(self.fig.add_axes([0.14, 0.12, 0.59, 0.03]),
                                 "Iteration", 0, len(trace.frames) - 1,
                                 valinit=frame, valstep=1, valfmt="%0.0f")
            self.slider.on_changed(lambda value: self.set_frame(int(value)))
        self.info = self.fig.text(0.79, 0.43, "Click a centre\nto inspect its module.",
                                  va="top", fontsize=9)
        self.fig.text(0.79, 0.22, "Scroll: zoom at pointer\nToolbar: pan / zoom\nHome or r: reset view\n← / →: change iteration\n\nColours: assigned region\nArea discs (partition frames):\n  size ∝ area, overlap = mass\n" + ("IDs persist across frames.\nRed dashes: this level's cuts.\n" if trace.run_scoped_regions else "IDs are frame-local.\n") + "Centroids: + target, ○ actual\n\nOutside ≠ solver failure.\nOverlap ≠ legalised layout.",
                      fontsize=9, va="top")
        self.fig.canvas.mpl_connect("key_press_event", self.key)
        self.fig.canvas.mpl_connect("pick_event", self.pick)
        self.fig.canvas.mpl_connect("scroll_event", self.scroll)
        self.render()

    # Mean module area maps to this marker area (points squared). Discs are
    # schematic, not true scale: real footprints are far too small to see on a
    # whole-chip view. Use "Bounding boxes" for true-scale footprints.
    AREA_MARKER_MEAN_PT2 = 80.0
    AREA_MARKER_ALPHA = 0.10

    def draw_area_markers(self, positions, colours):
        """Translucent area-proportional discs; dense overlaps read as darker mass."""
        trace = self.trace
        scale = trace.design["coordinates"]["dbu_per_micron"]
        areas = [module_area(trace.modules[p["id"]]) for p in positions]
        mean = sum(areas) / len(areas) if areas else 0
        if mean <= 0:
            return
        self.ax.scatter([p["x"] / scale for p in positions], [p["y"] / scale for p in positions],
                        s=[self.AREA_MARKER_MEAN_PT2 * self.marker_factor * a / mean
                           for a in areas],
                        c=colours, alpha=self.AREA_MARKER_ALPHA, linewidths=0, zorder=3)

    def toggle(self, label):
        self.options[label] = not self.options[label]
        self.render()

    def set_frame(self, index):
        self.index = index
        self.render()

    def move(self, delta):
        index = max(0, min(len(self.trace.frames) - 1, self.index + delta))
        if self.slider is not None:
            self.slider.set_val(index)

    def key(self, event):
        if event.key in ("right", "left"):
            self.move(1 if event.key == "right" else -1)
        elif event.key in ("home", "r"):
            self._rendered = False
            self.render()

    def scroll(self, event):
        """Zoom about the pointer; preserve this viewport across frame changes."""
        if event.inaxes is not self.ax or event.xdata is None or event.ydata is None:
            return
        if event.button not in ("up", "down"):
            return
        factor = 0.8 if event.button == "up" else 1.25
        for get_limits, set_limits, centre in (
            (self.ax.get_xlim, self.ax.set_xlim, event.xdata),
            (self.ax.get_ylim, self.ax.set_ylim, event.ydata),
        ):
            low, high = get_limits()
            set_limits(centre + (low - centre) * factor,
                       centre + (high - centre) * factor)
        self.fig.canvas.draw_idle()

    def pick(self, event):
        toolbar = getattr(self.fig.canvas.manager, "toolbar", None)
        if toolbar is not None and toolbar.mode:
            return
        if event.artist is self.points and len(event.ind):
            self.selected = self.trace.frames[self.index]["modules"][event.ind[0]]["id"]
            self.render()

    def render(self):
        import matplotlib.pyplot as plt
        from matplotlib.collections import PatchCollection
        from matplotlib.patches import Rectangle
        from textwrap import fill

        ax, trace = self.ax, self.trace
        previous_limits = (ax.get_xlim(), ax.get_ylim()) if self._rendered else None
        ax.clear()
        scale = trace.design["coordinates"]["dbu_per_micron"]
        palette = plt.get_cmap("tab20")
        frame = trace.frames[self.index]
        regions = {r["id"]: r for r in frame["regions"]}
        region_colour = lambda key: palette((key % 10) * 2 + (key // 10) % 2) if self.options["Region colours"] else "C0"
        def rectangle(box, **kwargs):
            a, b, c, d = box
            return Rectangle((a / scale, b / scale), (c - a) / scale, (d - b) / scale, **kwargs)

        ax.add_patch(rectangle(trace.design["die_bounds"], fill=False, edgecolor="0.7", linestyle=":"))
        ax.add_patch(rectangle(trace.design["core_bounds"], fill=False, edgecolor="black", linewidth=1.3))
        if self.options["Region boundaries"]:
            for region in regions.values():
                ax.add_patch(rectangle(region["bounds"], fill=False,
                                       edgecolor=region_colour(region["id"]), linewidth=1.5))
                if self.options["Region labels"]:
                    a, b, _, _ = region["bounds"]
                    ax.annotate(f"R{region['id']}", (a / scale, b / scale), fontsize=8,
                                xytext=(3, 3), textcoords="offset points")
        partitions = frame["partitions"]
        splits = [record for record in partitions if record["split"]]
        if self.options["Cut lines"]:
            for index, record in enumerate(splits):
                xs, ys = cut_segment(record)
                ax.plot([x / scale for x in xs], [y / scale for y in ys], color="red",
                        linewidth=1.8, linestyle="--", zorder=6,
                        label=f"Cuts this level ({len(splits)})" if index == 0 else None)
        for kind, marker, facecolour, label in (
            ("fixed", "s", "0.55", "Fixed cells"),
            ("io", "D", "black", "Fixed IOs"),
        ):
            modules = [m for m in trace.modules.values() if m["kind"] == kind]
            if not modules:
                continue
            for module in modules:
                x, y = module["initial_centre"]
                ax.add_patch(rectangle(translated_bbox(module, x, y), facecolor="0.75",
                                       edgecolor="0.35", alpha=0.5))
            # Larger, outlined markers remain distinct from movable circles even
            # without region colours or when footprints are too small to see.
            ax.scatter([m["initial_centre"][0] / scale for m in modules],
                       [m["initial_centre"][1] / scale for m in modules],
                       marker=marker, facecolors=facecolour, edgecolors="white",
                       linewidths=0.7, s=48, zorder=5,
                       label=f"{label} ({len(modules)})")

        positions = frame["modules"]
        colours = [region_colour(p["region_id"]) for p in positions]
        if self.options["Bounding boxes"]:
            patches = [rectangle(translated_bbox(trace.modules[p["id"]], p["x"], p["y"]))
                       for p in positions]
            large = self.scale > 1.5
            ax.add_collection(PatchCollection(patches, facecolors=colours, edgecolors=colours,
                                               linewidths=0.3 if large else 0.6,
                                               alpha=0.45 if large else 0.25))
        area_view = self.options["Area markers"] and frame["phase"] == "post_partition"
        if area_view:
            self.draw_area_markers(positions, colours)
        self.points = ax.scatter([p["x"] / scale for p in positions],
                                 [p["y"] / scale for p in positions],
                                 c=colours, marker="o",
                                 s=max(0.8, (4 if area_view else 16) * self.marker_factor),
                                 picker=5, zorder=4,
                                 label=f"Movable cells ({len(positions)})")
        # Modules beyond the viewport are drawn on its border so they stay visible.
        vx0, vx1, vy0, vy1 = (v / scale for v in self.viewport)
        beyond = [p for p in positions
                  if not (vx0 <= p["x"] / scale <= vx1 and vy0 <= p["y"] / scale <= vy1)]
        if beyond:
            ax.scatter([min(max(p["x"] / scale, vx0), vx1) for p in beyond],
                       [min(max(p["y"] / scale, vy0), vy1) for p in beyond],
                       # Region colour (like every other movable cell); hollow
                       # triangles so they are never mistaken for IO diamonds.
                       marker="^", facecolors="none",
                       edgecolors=[region_colour(p["region_id"]) for p in beyond],
                       linewidths=1.4,
                       s=45, zorder=7, clip_on=False,
                       label=f"Movable cells beyond view, drawn at border ({len(beyond)})")
        ax.legend(loc="upper left", fontsize=8, framealpha=0.9)
        outside = [p for p in positions if outside_region(p, regions[p["region_id"]])]
        if self.options["Outside centres"] and outside:
            ax.scatter([p["x"] / scale for p in outside], [p["y"] / scale for p in outside],
                       marker="x", color="black", s=65, linewidths=1, zorder=5)
        if self.options["Region centroids"]:
            for region in regions.values():
                members = [p for p in positions if p["region_id"] == region["id"]]
                weighted = []
                for p in members:
                    a, b, c, d = trace.modules[p["id"]]["bbox_offsets"]
                    weighted.append(((c - a) * (d - b), p))
                area = sum(w for w, _ in weighted)
                if area <= 0:
                    continue
                cx = sum(w * p["x"] for w, p in weighted) / area
                cy = sum(w * p["y"] for w, p in weighted) / area
                a, b, c, d = region["bounds"]
                tx, ty = (a + c) / 2, (b + d) / 2
                ax.plot([tx / scale, cx / scale], [ty / scale, cy / scale], color="black", linewidth=1)
                ax.scatter([tx / scale], [ty / scale], marker="+", color="black", s=90, zorder=6)
                ax.scatter([cx / scale], [cy / scale], marker="o", facecolors="none",
                           edgecolors="black", s=90, zorder=6)

        selected = next((p for p in positions if p["id"] == self.selected), None)
        if selected is not None:
            module = trace.modules[self.selected]
            ax.scatter([selected["x"] / scale], [selected["y"] / scale],
                       s=130, facecolors="none", edgecolors="black", zorder=7)
            region = regions[selected["region_id"]]
            lineage = ""
            if "depth" in region:
                parent = region["parent_id"]
                parent_text = "none" if parent is None else f"R{parent}"
                lineage = f"depth {region['depth']}, parent {parent_text}\n"
            self.info.set_text(fill(module["name"], 24) +
                               f"\nID {self.selected} / R{selected['region_id']}\n" + lineage +
                               f"x = {selected['x'] / scale:.6g} µm\n"
                               f"y = {selected['y'] / scale:.6g} µm")
        xmin, xmax, ymin, ymax = self.viewport
        ax.set_xlim(xmin / scale, xmax / scale)
        ax.set_ylim(ymin / scale, ymax / scale)
        if previous_limits is not None:
            ax.set_xlim(previous_limits[0])
            ax.set_ylim(previous_limits[1])
        self._rendered = True
        ax.set_aspect("equal", adjustable="box")
        ax.set_xlabel("x (µm)")
        ax.set_ylabel("y (µm)")
        ax.set_title(f"{trace.design['design']} — {frame_label(frame)}\n"
                     f"{len(positions)} movable modules, {len(regions)} regions"
                     + (f" ({len(splits)}/{len(partitions)} split)" if partitions else "")
                     + f"; outside-region centres: {len(outside)}"
                     + (f"\n{metrics_label(trace.metrics[self.index])}"
                        if self.index in trace.metrics else ""))
        self.fig.canvas.draw_idle()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--frame", type=int, default=0)
    parser.add_argument("--bbox", action="store_true", help="Show movable-cell footprints initially")
    parser.add_argument("--no-colour", action="store_true", help="Disable membership colouring initially")
    parser.add_argument("--drift", action="store_true", help="Mark centres outside assigned regions")
    parser.add_argument("--no-area", action="store_true",
                        help="Hide translucent area discs on partition frames")
    parser.add_argument("--save", type=Path, help="Save a PNG/PDF/SVG without opening a window")
    args = parser.parse_args(argv)
    try:
        trace = PlacementTrace.load(args.directory)
        require(0 <= args.frame < len(trace.frames), "Frame index out of range")
        if args.save:
            import matplotlib
            matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        viewer = TraceViewer(trace, args.frame, args.bbox, not args.no_colour, args.drift,
                             area=not args.no_area)
        if args.save:
            viewer.fig.savefig(args.save, dpi=180)
            plt.close(viewer.fig)
        else:
            plt.show()
    except (OSError, ValueError, ImportError) as error:
        parser.exit(1, f"Error: {error}\n")


if __name__ == "__main__":
    main()

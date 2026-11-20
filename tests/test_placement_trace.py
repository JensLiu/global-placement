"""Cross-language fixture and headless replay checks (requires matplotlib).

Build placement_trace_smoke, then run:
  python3 -m unittest discover -s tests -p test_placement_trace.py
Override its executable using GP_TRACE_FIXTURE when testing another build.
"""
import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
import warnings

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("plot_placement_trace", ROOT / "scripts/plot_placement_trace.py")
viewer = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = viewer
spec.loader.exec_module(viewer)


class TraceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.temp.name) / "run"
        fixture = os.environ.get("GP_TRACE_FIXTURE", ROOT / "build/Release/placement_tracer_smoke")
        subprocess.run([str(fixture), str(cls.directory)], check=True, capture_output=True, text=True)
        cls.original_design = (cls.directory / "design.json").read_text()
        cls.original_frames = (cls.directory / "iterations.jsonl").read_text()
        os.environ.setdefault("MPLCONFIGDIR", str(Path(cls.temp.name) / "matplotlib"))
        import matplotlib
        matplotlib.use("Agg")

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        (self.directory / "design.json").write_text(self.original_design)
        (self.directory / "iterations.jsonl").write_text(self.original_frames)

    def write_frames(self, frames):
        (self.directory / "iterations.jsonl").write_text(
            "".join(json.dumps(frame) + "\n" for frame in frames))

    def test_cpp_round_trip(self):
        trace = viewer.PlacementTrace.load(self.directory)
        self.assertEqual(len(trace.frames), 2)
        self.assertEqual(trace.modules[0]["name"], 'cell"\\\nA')
        self.assertEqual(trace.modules[0]["bbox_offsets"], [-5, -10, 5, 10])
        p = trace.frames[1]["modules"][0]
        self.assertEqual(p["x"], 700.75)
        self.assertTrue(viewer.outside_region(p, trace.frames[1]["regions"][0]))
        self.assertEqual(viewer.translated_bbox(trace.modules[0], 700.75, 400.5),
                         [695.75, 390.5, 705.75, 410.5])

    def test_bad_frames(self):
        original = [json.loads(line) for line in self.original_frames.splitlines()]
        for mutation in ("run", "missing", "duplicate", "region", "nonfinite", "iteration"):
            with self.subTest(mutation=mutation):
                frames = copy.deepcopy(original)
                if mutation == "run": frames[0]["run_id"] = "wrong"
                if mutation == "missing": frames[0]["modules"].pop()
                if mutation == "duplicate": frames[0]["modules"].append(frames[0]["modules"][0])
                if mutation == "region": frames[0]["modules"][0]["region_id"] = 42
                if mutation == "nonfinite": frames[0]["modules"][0]["x"] = float("nan")
                if mutation == "iteration": frames[1]["iteration"] = 3
                self.write_frames(frames)
                with self.assertRaises(viewer.TraceError):
                    viewer.PlacementTrace.load(self.directory)

    def test_version3_phases(self):
        original = [json.loads(line) for line in self.original_frames.splitlines()]
        self.assertTrue(all("frame" in f for f in original))
        # Insert a partition frame (latest solve's positions, same regions).
        partition = copy.deepcopy(original[0])
        partition.update(phase="post_partition", partitions=[])
        frames = [original[0], partition, copy.deepcopy(original[1])]
        for index, frame in enumerate(frames):
            frame["frame"] = index
        self.write_frames(frames)
        trace = viewer.PlacementTrace.load(self.directory)
        self.assertEqual([viewer.frame_stem(i, f) for i, f in enumerate(trace.frames)],
                         ["frame-000000-solve", "frame-000001-partition", "frame-000002-solve"])
        self.assertIn("after partitioning (regions for solve 1)", viewer.frame_label(trace.frames[1]))
        for mutation in ("phase", "frame", "stale"):
            with self.subTest(mutation=mutation):
                bad = copy.deepcopy(frames)
                if mutation == "phase": bad[1]["phase"] = "pre_solve"
                if mutation == "frame": bad[2]["frame"] = 5
                if mutation == "stale": bad[1]["iteration"] = 1
                self.write_frames(bad)
                with self.assertRaises(viewer.TraceError):
                    viewer.PlacementTrace.load(self.directory)

    def test_area_markers_only_on_partition_frames(self):
        import matplotlib.pyplot as plt
        original = [json.loads(line) for line in self.original_frames.splitlines()]
        partition = copy.deepcopy(original[0])
        partition.update(phase="post_partition", partitions=[])
        frames = [original[0], partition]
        for index, frame in enumerate(frames):
            frame["frame"] = index
        self.write_frames(frames)
        trace = viewer.PlacementTrace.load(self.directory)
        def discs(plot):
            return [c for c in plot.ax.collections
                    if c is not plot.points and getattr(c, "get_alpha", lambda: None)() ==
                    viewer.TraceViewer.AREA_MARKER_ALPHA]
        plot = viewer.TraceViewer(trace, frame=0, interactive=False)
        self.assertTrue(plot.options["Area markers"])
        self.assertEqual(discs(plot), [])  # post_solve: none
        plot.set_frame(1)
        found = discs(plot)
        self.assertEqual(len(found), 1)
        sizes = found[0].get_sizes()
        areas = [viewer.module_area(trace.modules[p["id"]]) for p in trace.frames[1]["modules"]]
        mean = sum(areas) / len(areas)
        self.assertEqual(list(sizes), [viewer.TraceViewer.AREA_MARKER_MEAN_PT2 * a / mean for a in areas])
        plt.close(plot.fig)
        hidden = viewer.TraceViewer(trace, frame=1, interactive=False, area=False)
        self.assertEqual(discs(hidden), [])
        plt.close(hidden.fig)

    def metrics_record(self, frame, phase, iteration, hpwl):
        return {"frame": frame, "phase": phase, "iteration": iteration,
                "hpwl_um": {"all": hpwl, "modelled": hpwl}, "objective_um2": 1.0,
                "density": {"bin_um": 5.6, "target": 1.0, "overflow": 0.25,
                            "max_bin_density": 2.0},
                "outside": {"die": 3, "core": 4, "own_region": 1,
                            "own_region_area_fraction": 0.1},
                "movement_um": {"from_previous_solve": None,
                                "from_input": {"median": 1, "p95": 2, "max": 3}},
                "cog_error_um": 0.0 if phase == "post_solve" else None,
                "regions": {"count": 1, "median_size": 2, "singletons": 0},
                "seconds": {"since_previous": 0.1, "since_start": 0.2}}

    def write_metrics(self, run_id):
        header = self.metrics_record(None, "input", None, 2000.0)
        header.update(schema="global-placement.gordian-metrics", version=1, run_id=run_id)
        lines = [header, self.metrics_record(0, "post_solve", 0, 1500.0),
                 self.metrics_record(1, "post_solve", 1, 1000.0)]
        (self.directory / "metrics.jsonl").write_text(
            "".join(json.dumps(line) + "\n" for line in lines))

    def test_metrics_title_and_summary(self):
        import io
        import contextlib
        import matplotlib.pyplot as plt
        try:
            run_id = json.loads(self.original_design)["run_id"]
            self.write_metrics(run_id)
            trace = viewer.PlacementTrace.load(self.directory)
            self.assertEqual(sorted(trace.metrics), [0, 1])
            plot = viewer.TraceViewer(trace, frame=1, interactive=False)
            self.assertIn("HPWL 1,000 µm; density overflow 25.0%", plot.ax.get_title())
            plt.close(plot.fig)
            spec = importlib.util.spec_from_file_location(
                "summarise_trace", ROOT / "scripts/summarise_trace.py")
            summary = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(summary)
            header, frames = summary.load_metrics(self.directory)
            self.assertEqual(summary.row(frames[-1], header)[1:3], ["1,000", "0.500"])
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                summary.main([str(self.directory), str(self.directory),
                              "--plot", str(self.directory / "curves.png")])
            self.assertIn("Comparison", output.getvalue())
            self.assertTrue((self.directory / "curves.png").exists())
            # Interpolated HPWL at equal spreading, and the objective invariant.
            solves = [self.metrics_record(0, "post_solve", 0, 1500.0),
                      self.metrics_record(1, "post_solve", 1, 1000.0)]
            solves[0]["density"]["overflow"], solves[1]["density"]["overflow"] = 0.30, 0.10
            self.assertAlmostEqual(summary.hpwl_at_overflow(header, solves, 0.20), 0.625)
            self.assertIsNone(summary.hpwl_at_overflow(header, solves, 0.05))
            self.assertTrue(summary.objective_monotone(solves))
            solves[1]["objective_um2"] = 0.5
            self.assertFalse(summary.objective_monotone(solves))
            # A metrics file from another run is ignored, not trusted.
            self.write_metrics("other-run")
            with warnings.catch_warnings(record=True):
                warnings.simplefilter("always")
                self.assertEqual(viewer.PlacementTrace.load(self.directory).metrics, {})
        finally:
            for name in ("metrics.jsonl", "curves.png"):
                (self.directory / name).unlink(missing_ok=True)

    def test_bad_schema(self):
        design = json.loads(self.original_design)
        design["version"] = 99
        (self.directory / "design.json").write_text(json.dumps(design))
        with self.assertRaises(viewer.TraceError):
            viewer.PlacementTrace.load(self.directory)

    def test_truncated_final_frame(self):
        with (self.directory / "iterations.jsonl").open("a") as stream:
            stream.write('{"iteration":2')
        with warnings.catch_warnings(record=True) as caught:
            trace = viewer.PlacementTrace.load(self.directory)
        self.assertEqual(len(trace.frames), 2)
        self.assertEqual(len(caught), 1)

    def test_malformed_complete_frame(self):
        with (self.directory / "iterations.jsonl").open("a") as stream:
            stream.write('{broken}\n')
        with self.assertRaises(viewer.TraceError):
            viewer.PlacementTrace.load(self.directory)

    def test_render_and_controls(self):
        import matplotlib.pyplot as plt
        trace = viewer.PlacementTrace.load(self.directory)
        plot = viewer.TraceViewer(trace, bbox=True, drift=True)
        original_limits = plot.ax.get_xlim(), plot.ax.get_ylim()
        plot.move(1)
        self.assertEqual(plot.index, 1)
        self.assertEqual(original_limits, (plot.ax.get_xlim(), plot.ax.get_ylim()))
        self.assertAlmostEqual(plot.points.get_offsets()[0][0], 700.75 / 2000)
        for label in plot.options:
            plot.toggle(label)
        plot.selected = 0
        plot.render()
        self.assertIn("ID 0 / R1", plot.info.get_text())
        output = Path(self.temp.name) / "frame.png"
        plot.fig.savefig(output)
        self.assertGreater(output.stat().st_size, 1000)
        plt.close(plot.fig)

    def test_module_markers_and_legend(self):
        import matplotlib.pyplot as plt
        from matplotlib.markers import MarkerStyle
        trace = viewer.PlacementTrace.load(self.directory)
        for interactive in (False, True):
            with self.subTest(interactive=interactive):
                plot = viewer.TraceViewer(trace, colour=False, interactive=interactive)
                for frame in range(len(trace.frames)):
                    plot.set_frame(frame)
                    handles, labels = plot.ax.get_legend_handles_labels()
                    self.assertEqual(set(labels),
                                     {"Fixed cells (1)", "Fixed IOs (1)", "Movable cells (2)"})
                    expected = {"Fixed cells (1)": ("s", 1),
                                "Fixed IOs (1)": ("D", 1),
                                "Movable cells (2)": ("o", 2)}
                    for handle, label in zip(handles, labels):
                        marker, count = expected[label]
                        style = MarkerStyle(marker)
                        path = style.get_path().transformed(style.get_transform())
                        self.assertTrue((handle.get_paths()[0].vertices == path.vertices).all())
                        self.assertEqual(len(handle.get_offsets()), count)
                    self.assertEqual({t.get_text() for t in plot.ax.get_legend().get_texts()},
                                     set(labels))
                plt.close(plot.fig)

    def test_zoom_survives_frame_changes_and_toggles(self):
        import matplotlib.pyplot as plt
        trace = viewer.PlacementTrace.load(self.directory)
        plot = viewer.TraceViewer(trace)
        original = plot.ax.get_xlim(), plot.ax.get_ylim()
        plot.scroll(SimpleNamespace(inaxes=plot.ax, xdata=0.2, ydata=0.1, button="up"))
        zoomed = plot.ax.get_xlim(), plot.ax.get_ylim()
        self.assertLess(zoomed[0][1] - zoomed[0][0], original[0][1] - original[0][0])
        plot.move(1)
        plot.toggle("Bounding boxes")
        self.assertEqual(zoomed, (plot.ax.get_xlim(), plot.ax.get_ylim()))
        plot.key(SimpleNamespace(key="home"))
        self.assertEqual(original, (plot.ax.get_xlim(), plot.ax.get_ylim()))
        plt.close(plot.fig)

    def test_static_render_has_no_controls(self):
        import matplotlib.pyplot as plt
        trace = viewer.PlacementTrace.load(self.directory)
        plot = viewer.TraceViewer(trace, interactive=False)
        self.assertEqual(len(plot.fig.axes), 1)
        self.assertFalse(hasattr(plot, "controls"))
        plot.set_frame(1)
        plt.close(plot.fig)

    def run_script(self, name, *args, check=True):
        env = dict(os.environ, MPLBACKEND="Agg")
        return subprocess.run(
            [sys.executable, str(ROOT / "scripts" / name), *map(str, args)],
            env=env, check=check, capture_output=True, text=True)

    def test_image_export_cli_all_and_single_frames(self):
        output = Path(self.temp.name) / "export-all"
        self.run_script("generate_placement_images.py", self.directory,
                        "--output-dir", output, "--bbox", "--drift")
        self.assertEqual(len(list(output.glob("*.png"))), 2)
        for image in output.glob("*.png"):
            self.assertGreater(image.stat().st_size, 1000)
        single = Path(self.temp.name) / "export-single"
        self.run_script("generate_placement_images.py", self.directory,
                        "--output-dir", single, "--frame", 1, "--format", "svg")
        self.assertEqual([p.name for p in single.iterdir()], ["frame-000001-solve.svg"])

    def test_image_export_rejects_invalid_frame(self):
        result = self.run_script("generate_placement_images.py", self.directory,
                                 "--frame", 99, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Frame index out of range", result.stderr)

    def test_window_viewer_reports_missing_gui(self):
        result = self.run_script("view_placement_trace.py", self.directory,
                                 "--backend", "Agg", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("No interactive GUI backend", result.stderr)

    def test_single_frame_cli(self):
        self.write_frames([json.loads(self.original_frames.splitlines()[0])])
        output = Path(self.temp.name) / "single.png"
        viewer.main([str(self.directory), "--bbox", "--save", str(output)])
        self.assertGreater(output.stat().st_size, 1000)


if __name__ == "__main__":
    unittest.main()

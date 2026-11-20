"""Unit tests for scripts/bench.py (no OpenROAD needed).

  python3 -m unittest tests/test_bench.py
"""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("bench", ROOT / "scripts/bench.py")
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class BenchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        base = Path(self.temp.name)
        self.orfs = base / "flow"
        results = self.orfs / "results/plat/des/base"
        results.mkdir(parents=True)
        (results / "3_2_place_iop.odb").write_bytes(b"netlist")
        self.log = self.orfs / "logs/plat/des/base/3_3_place_gp.log"
        self.log.parent.mkdir(parents=True)
        platform = self.orfs / "platforms/plat"
        platform.mkdir(parents=True)
        (platform / "fastroute.tcl").write_text("# layers\n")
        (platform / "config.mk").write_text(
            "export MIN_ROUTING_LAYER = metal2\n"
            "export MAX_ROUTING_LAYER ?= metal10\n"
            "export FASTROUTE_TCL ?= $(PLATFORM_DIR)/fastroute.tcl\n"
            "export LIB_FILES = $(OBJECTS_DIR)/lib/x.lib\n")
        self.saved_root = bench.REFERENCE_ROOT
        bench.REFERENCE_ROOT = base / "reference"
        self.design = bench.Design("des", "plat", self.orfs)

    def tearDown(self):
        bench.REFERENCE_ROOT = self.saved_root
        self.temp.cleanup()

    def test_density_from_orfs_log_or_auto(self):
        self.assertEqual(self.design.density()[0], "auto")
        self.log.write_text("[INFO GPL-0023] Placement target density:       0.3000\n"
                            "[INFO GPL-0023] Placement target density:       0.6400\n")
        self.assertEqual(self.design.density()[0], "0.6400")  # last value wins

    def test_routing_env_is_generic(self):
        env = self.design.routing_env()
        self.assertEqual(env["MAX_ROUTING_LAYER"], "metal10")
        self.assertTrue(env["BENCH_FASTROUTE_TCL"].endswith("platforms/plat/fastroute.tcl"))
        self.assertNotIn("LIB_FILES", env)  # unresolved Make reference dropped

    def test_missing_input_is_reported(self):
        with self.assertRaises(bench.BenchError):
            bench.Design("absent", "plat", self.orfs)

    def write_result(self, directory, legal, dpo, extra=None):
        directory.mkdir(parents=True, exist_ok=True)
        result = {"gp_hpwl_um": legal * 0.9, "legal_hpwl_um": legal, "legal": True,
                  "dpo_hpwl_um": dpo, "gp_seconds": 1.0,
                  "displacement_um": {"total": 1, "mean": 0.5, "max": 2, "instances": 2}}
        result.update(extra or {})
        (directory / "bench.json").write_text(json.dumps(result))

    def test_report_ratios_and_input_check(self):
        for variant, legal in (("gpl", 1000.0), ("gpl_routability", 1250.0)):
            self.write_result(bench.reference_dir(self.design, variant), legal, legal * 0.95)
        trace = Path(self.temp.name) / "trace"
        self.write_result(trace / "bench", 2000.0, 1900.0)
        (trace / "bench/meta.json").write_text(json.dumps(
            {"design": "des", "input_sha256": bench.sha256(self.design.input)}))
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            bench.report([trace], self.design)
        row = [line for line in output.getvalue().splitlines() if line.startswith("trace")][0]
        self.assertIn("2.000", row)  # x GPL
        self.assertIn("1.600", row)  # x GPL-R
        (trace / "bench/meta.json").write_text(json.dumps({"design": "des", "input_sha256": "x"}))
        with self.assertRaises(bench.BenchError):
            bench.report([trace], self.design)


if __name__ == "__main__":
    unittest.main()

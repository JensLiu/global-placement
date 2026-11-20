"""Check Make configuration routing without configuring/building dependencies.

Run: python3 -m unittest discover -s tests -p test_make_build_variants.py
"""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class MakeBuildVariantsTest(unittest.TestCase):
    def dry_run(self, *args):
        # Do not inherit a caller's build overrides or recursive-make state.
        env = os.environ.copy()
        for key in (
            "DEBUG", "BUILD_TYPE", "BUILD_DIR", "MAKEFLAGS", "MFLAGS",
            "MAKEOVERRIDES", "CMAKE", "GDB", "PYTHON", "TRACE_DIR", "GP_TRACE_DIR",
            "RENDER_JOBS", "IMAGE_FORMATS", "DESIGN",
        ):
            env.pop(key, None)
        return subprocess.check_output(
            # '+' build recipes execute even with --dry-run; echo CMake so
            # these tests never touch a real build tree.
            ["make", "--dry-run", *args, "CMAKE=echo cmake", "GDB=gdb"],
            cwd=ROOT, env=env, text=True,
        )

    def assert_variant(self, output, variant, directory=None):
        directory = directory or f"build/{variant}"
        self.assertIn(f'-B "{directory}"', output)
        self.assertIn(f'-DCMAKE_BUILD_TYPE="{variant}"', output)
        self.assertIn(f'--build "{directory}"', output)

    def test_default_and_explicit_release(self):
        for args in ((), ("DEBUG=0",)):
            with self.subTest(args=args):
                self.assert_variant(self.dry_run(*args), "Release")

    def test_run_debug_and_release_use_distinct_executables(self):
        for debug, variant in (("0", "Release"), ("1", "Debug")):
            with self.subTest(debug=debug):
                output = self.dry_run("run", f"DEBUG={debug}")
                self.assert_variant(output, variant)
                self.assertIn(f'"build/{variant}/odb_hello"', output)
                other = "Release" if debug == "1" else "Debug"
                self.assertNotIn(f"build/{other}", output)

    def test_debugger_selects_debug_even_with_default_debug_flag(self):
        output = self.dry_run("run-debug", "DEBUG=0")
        self.assert_variant(output, "Debug")
        self.assertIn('gdb --quiet --args "build/Debug/odb_hello"', output)

    def test_explicit_build_type_selects_its_own_directory(self):
        self.assert_variant(
            self.dry_run("DEBUG=1", "BUILD_TYPE=RelWithDebInfo"),
            "RelWithDebInfo",
        )

    def test_explicit_build_directory_remains_supported(self):
        self.assert_variant(
            self.dry_run("DEBUG=1", "BUILD_DIR=custom build"),
            "Debug", "custom build",
        )

    def test_run_enables_json_collection_by_default(self):
        output = self.dry_run("run")
        self.assertIn('trace_dir="build/traces/picorv32-', output)
        self.assertIn('GP_TRACE_DIR="$trace_dir"', output)

    def test_run_accepts_explicit_trace_directory_and_opt_out(self):
        for directory in ("custom traces/new-run", ""):
            with self.subTest(directory=directory):
                output = self.dry_run("run", f"TRACE_DIR={directory}")
                self.assertIn(f'trace_dir="{directory}"', output)
                self.assertIn('GP_TRACE_DIR="$trace_dir"', output)

    def test_run_exports_images_from_same_trace(self):
        output = self.dry_run("run", "TRACE_DIR=custom traces/new-run",
                              "PYTHON=/custom/python")
        self.assertIn('"/custom/python" scripts/generate_placement_images.py "$trace_dir" --jobs "0" --bbox',
                      output)
        self.assertIn('|| exit $?;', output)

    def test_run_image_export_execution_and_failures(self):
        # Exercise the actual shell recipe without compiling or running placement.
        # Stub only the external programs; paths with spaces check shell quoting.
        for trace_enabled, placement_status, export_status in (
            (True, 0, 0), (False, 0, 0), (True, 7, 0), (True, 0, 9),
        ):
            with self.subTest(trace_enabled=trace_enabled,
                              placement_status=placement_status, export_status=export_status):
                with tempfile.TemporaryDirectory() as temp:
                    directory = Path(temp)
                    build = directory / "mock build"
                    build.mkdir()
                    placement_log = directory / "placement.log"
                    export_log = directory / "export.log"
                    placer = build / "odb_hello"
                    placer.write_text('#!/bin/sh\nprintf "%s\\n" "$GP_TRACE_DIR" > '
                                      f'"{placement_log}"\nexit {placement_status}\n')
                    placer.chmod(0o755)
                    python = directory / "mock python"
                    python.write_text('#!/bin/sh\nif [ "$1" = scripts/generate_placement_images.py ]; then\n'
                                      'printf "%s\\n" "$@" > '
                                      f'"{export_log}"\nfi\nexit {export_status}\n')
                    python.chmod(0o755)
                    trace = str(directory / "trace with spaces") if trace_enabled else ""
                    env = os.environ.copy()
                    for key in ("MAKEFLAGS", "MFLAGS", "MAKEOVERRIDES"):
                        env.pop(key, None)
                    result = subprocess.run(
                        ["make", "run", "CMAKE=true", f"BUILD_DIR={build}",
                         f"PYTHON={python}", f"TRACE_DIR={trace}", "RENDER_JOBS=3",
                         "IMAGE_FORMATS=png pdf"],
                        cwd=ROOT, env=env, text=True, capture_output=True,
                    )
                    self.assertEqual(placement_log.read_text(), trace + "\n")
                    should_export = trace_enabled and placement_status == 0
                    self.assertEqual(export_log.exists(), should_export)
                    if should_export:
                        self.assertEqual(export_log.read_text().splitlines(),
                                         ["scripts/generate_placement_images.py", trace,
                                          "--jobs", "3", "--bbox", "--format", "png", "pdf"])
                    success = placement_status == 0 and (not trace_enabled or export_status == 0)
                    self.assertEqual(result.returncode == 0, success, result.stderr)

    def test_render_jobs_are_forwarded(self):
        for target in ("run", "full"):
            for jobs in (0, 1, 8):
                with self.subTest(target=target, jobs=jobs):
                    output = self.dry_run(target, f"RENDER_JOBS={jobs}")
                    self.assertIn(f'scripts/generate_placement_images.py "$trace_dir" --jobs "{jobs}"',
                                  output)

    def test_clean_only_targets_selected_configuration(self):
        for debug, variant in (("0", "Release"), ("1", "Debug")):
            with self.subTest(debug=debug):
                output = self.dry_run("clean", f"DEBUG={debug}")
                self.assertIn(f'--build "build/{variant}" --target clean', output)
                other = "Release" if debug == "1" else "Debug"
                self.assertNotIn(f"build/{other}", output)


if __name__ == "__main__":
    unittest.main()

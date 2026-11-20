"""Run with: python3 -m unittest discover -s tests -p 'test_gdb_dap_bridge.py'."""

import importlib.util
import io
import json
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/gdb_dap_bridge.py"
spec = importlib.util.spec_from_file_location("bridge", SCRIPT)
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


class FramingTests(unittest.TestCase):
    def test_round_trip(self):
        stream = io.BytesIO()
        expected = {"type": "request", "arguments": {"args": ["a b", "λ"]}}
        bridge.write_message(stream, expected)
        stream.seek(0)
        self.assertEqual(bridge.read_message(stream), expected)
        self.assertIsNone(bridge.read_message(stream))

    def test_truncated_body(self):
        with self.assertRaises(ValueError):
            bridge.read_message(io.BytesIO(b"Content-Length: 20\r\n\r\n{}"))

    def test_invalid_length(self):
        with self.assertRaises(ValueError):
            bridge.read_message(io.BytesIO(b"Content-Length: -1\r\n\r\n"))


@unittest.skipUnless(shutil.which("gdb") and shutil.which("c++"), "Requires GDB with DAP and C++")
class GdbIntegrationTests(unittest.TestCase):
    def test_launch_uses_make_arguments(self):
        with tempfile.TemporaryDirectory(prefix="gdb dap test ") as directory:
            directory = Path(directory)
            source = directory / "main.cpp"
            program = directory / "test executable"
            source.write_text("int main(int argc, char **argv) { return argc == 2 ? 0 : 1; }\n")
            subprocess.run(["c++", "-g", "-O0", str(source), "-o", str(program)], check=True)
            server = subprocess.Popen(
                [sys.executable, str(SCRIPT), "--port", "0", "--", str(program), "design with spaces.odb"],
                cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            try:
                line = server.stdout.readline()
                self.assertIn("GDB DAP listening on", line)
                port = int(line.strip().rsplit(":", 1)[1])
                with socket.create_connection(("127.0.0.1", port), timeout=15) as connection:
                    with connection.makefile("rwb") as stream:
                        sequence = 0
                        events = []

                        def request(command, arguments=None):
                            nonlocal sequence
                            sequence += 1
                            bridge.write_message(stream, {"seq": sequence, "type": "request",
                                                        "command": command, "arguments": arguments or {}})
                            return sequence

                        def response(seq):
                            while True:
                                message = bridge.read_message(stream)
                                self.assertIsNotNone(message)
                                if message.get("type") == "event":
                                    events.append(message)
                                if message.get("type") == "response" and message.get("request_seq") == seq:
                                    self.assertTrue(message["success"], message)
                                    return message.get("body", {})

                        response(request("initialize", {"adapterID": "gdb", "linesStartAt1": True,
                                                        "columnsStartAt1": True, "pathFormat": "path"}))
                        launch = request("launch", {"program": "/wrong/program", "args": ["wrong"],
                                                    "cwd": "/wrong/cwd", "stopAtBeginningOfMainSubprogram": True})
                        response(request("configurationDone"))
                        response(launch)
                        # Launch responses can arrive before the asynchronous stop.
                        while not any(event.get("event") == "stopped" for event in events):
                            message = bridge.read_message(stream)
                            self.assertIsNotNone(message)
                            events.append(message)
                        threads = response(request("threads"))["threads"]
                        stack = response(request("stackTrace", {"threadId": threads[0]["id"]}))
                        frame = stack["stackFrames"][0]["id"]
                        for expression, expected in [("argc", "2"), ("argv[1]", "design with spaces.odb")]:
                            result = response(request("evaluate", {"expression": expression,
                                                                  "frameId": frame, "context": "watch"}))
                            self.assertIn(expected, result["result"])
                        # Confirm the stale launch cwd was replaced with Make's cwd.
                        result = response(request("evaluate", {"expression": "pwd", "context": "repl"}))
                        self.assertIn(str(ROOT), json.dumps(result))
                        response(request("disconnect", {"terminateDebuggee": True}))
                self.assertEqual(server.wait(timeout=10), 0, server.stderr.read())
            finally:
                if server.poll() is None:
                    server.terminate()
                    server.wait(timeout=10)
                server.stdout.close()
                server.stderr.close()


if __name__ == "__main__":
    unittest.main()

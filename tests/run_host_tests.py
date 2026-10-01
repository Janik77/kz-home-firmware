"""Compile and execute the production C parser, journal and command engine.

Examples: python tests/run_host_tests.py --cc gcc
          python tests/run_host_tests.py --cc path/to/zig cc
Only the NVS, clock and GPIO boundary is replaced by deterministic test doubles.
"""
import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--cc", nargs="+", default=["cc"])
args = parser.parse_args()
build = ROOT / "build" / "host-tests"
build.mkdir(parents=True, exist_ok=True)
exe = build / ("protocol_tests.exe" if os.name == "nt" else "protocol_tests")
sources = ["kz_relay/main/protocol.c", "kz_relay/main/journal.c",
           "kz_relay/main/command_engine.c", "tests/fake_platform.c", "tests/test_protocol.c"]
subprocess.run(args.cc + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-UNDEBUG",
    "-Itests/stubs", "-Ikz_relay/main", *sources, "-o", str(exe)], cwd=ROOT, check=True)
subprocess.run([str(exe)], cwd=ROOT, check=True)

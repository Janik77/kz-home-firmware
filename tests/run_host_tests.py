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
parser.add_argument("--cmake", default="cmake")
args = parser.parse_args()
build = ROOT / "build" / "host-tests"
build.mkdir(parents=True, exist_ok=True)
exe = build / ("protocol_tests.exe" if os.name == "nt" else "protocol_tests")
sources = ["kz_relay/main/protocol.c", "kz_relay/main/journal.c",
           "kz_relay/main/command_engine.c", "tests/fake_platform.c", "tests/test_protocol.c"]
subprocess.run(args.cc + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-UNDEBUG",
    "-Itests/stubs", "-Itests/board_stubs", "-Ikz_relay/main",
    "-DKZ_BOARD_esp32_wroom=1", "-DCONFIG_IDF_TARGET_ESP32=1",
    *sources, "-o", str(exe)], cwd=ROOT, check=True)
subprocess.run([str(exe)], cwd=ROOT, check=True)

# Compile the actual relay driver against recording GPIO stubs for both boards.
common = args.cc + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-UNDEBUG",
    "-Itests/stubs", "-Itests/board_stubs", "-Ikz_relay/main"]
boards = [("esp32_wroom", "ESP32"), ("esp32c6", "ESP32C6")]
for board, target in boards:
    definitions = [f"-DKZ_BOARD_{board}=1", f"-DCONFIG_IDF_TARGET_{target}=1"]
    for polarity in (0, 1):
        for override in ([], ["-DKZ_RELAY_GPIO=18"]):
            output = build / f"board_{board}_{polarity}_{bool(override)}{exe.suffix}"
            subprocess.run(common + definitions + [f"-DKZ_RELAY_ACTIVE_HIGH={polarity}"]
                + override + ["kz_relay/main/relay.c", "tests/test_board.c", "-o", str(output)],
                cwd=ROOT, check=True)
            subprocess.run([str(output)], check=True)

# Compile failures must come from our guards, rather than unrelated tool errors.
probe = build / "board_probe.c"
probe.write_text('#include "board.h"\n', encoding="utf-8")
classic = ["-DKZ_BOARD_esp32_wroom=1", "-DCONFIG_IDF_TARGET_ESP32=1"]
c6 = ["-DKZ_BOARD_esp32c6=1", "-DCONFIG_IDF_TARGET_ESP32C6=1"]
invalid = [([], "Select exactly one"),
    (classic + ["-DKZ_BOARD_esp32c6=1"], "Select exactly one"),
    (["-DKZ_BOARD_esp32_wroom=1", "-DCONFIG_IDF_TARGET_ESP32C6=1"], "requires the esp32 target"),
    (["-DKZ_BOARD_esp32c6=1", "-DCONFIG_IDF_TARGET_ESP32=1"], "requires the esp32c6 target"),
    (classic + ["-DKZ_RELAY_ACTIVE_HIGH=2"], "Relay polarity must be")]
invalid += [(classic + [f"-DKZ_RELAY_GPIO={pin}"], "Unsafe/unsupported")
    for pin in (-1, 0, 1, 2, 3, 5, 6, 11, 12, 15, 16, 17, 34, 39, 64)]
invalid += [(c6 + [f"-DKZ_RELAY_GPIO={pin}"], "Unsafe/unsupported")
    for pin in (4, 5, 8, 9, 12, 13, 15, 16, 17, 24, 30)]
for definitions, diagnostic in invalid:
    result = subprocess.run(common + definitions + ["-fsyntax-only", str(probe)],
        cwd=ROOT, capture_output=True, text=True)
    assert result.returncode != 0 and diagnostic in result.stderr, result.stderr
print(f"PASS: {len(invalid)} unsafe board/target/GPIO/polarity configurations rejected")

# Exercise the production CMake selector without invoking IDF or touching hardware.
selector = (ROOT / "kz_relay/boards/select.cmake").as_posix()
cmake_env = os.environ.copy()
cmake_env.pop("IDF_TARGET", None)
for board, target in boards:
    script = build / f"select_{board}.cmake"
    script.write_text(f'include("{selector}")\n'
        f'if(NOT IDF_TARGET STREQUAL "{target.lower()}")\n'
        'message(FATAL_ERROR "Wrong target")\nendif()\n'
        f'if(NOT KZ_RELAY_GPIO STREQUAL "{26 if board == "esp32_wroom" else 2}")\n'
        'message(FATAL_ERROR "Wrong default GPIO")\nendif()\n'
        'if(NOT KZ_RELAY_ACTIVE_HIGH STREQUAL "1")\n'
        'message(FATAL_ERROR "Wrong default polarity")\nendif()\n'
        'if(NOT SDKCONFIG STREQUAL "${CMAKE_BINARY_DIR}/sdkconfig")\n'
        'message(FATAL_ERROR "Shared generated configuration")\nendif()\n', encoding="utf-8")
    subprocess.run([args.cmake, f"-DKZ_BOARD={board}", "-P", str(script)],
        env=cmake_env, check=True)
subprocess.run([args.cmake, "-P", str(build / "select_esp32_wroom.cmake")],
    env=cmake_env, check=True) # No selection defaults to classic ESP32.
checks = [(["-DKZ_BOARD=unknown"], "Unsupported KZ_BOARD"),
    (["-DKZ_BOARD=esp32_wroom", "-DIDF_TARGET=esp32c6"], "disagree"),
    (["-DKZ_BOARD=esp32c6", "-DKZ_CONFIGURED_BOARD=esp32_wroom"], "separate build"),
    (["-DKZ_RELAY_ACTIVE_HIGH=ON"], "must be 0 or 1"),
    (["-DKZ_RELAY_GPIO=2;3"], "decimal integer"),
    (["-DSDKCONFIG=legacy/sdkconfig"], "SDKCONFIG must be inside")]
for options, diagnostic in checks:
    result = subprocess.run([args.cmake, *options, "-P", str(build / "select_esp32_wroom.cmake")],
        env=cmake_env, capture_output=True, text=True)
    assert result.returncode != 0 and diagnostic in result.stderr, result.stderr
result = subprocess.run([args.cmake, "-P", str(build / "select_esp32_wroom.cmake")],
    env={**cmake_env, "IDF_TARGET": "esp32c6"}, capture_output=True, text=True)
assert result.returncode != 0 and "environment IDF_TARGET disagree" in result.stderr, result.stderr
print("PASS: CMake board selection, target/build isolation and configuration validation")

"""Compile and execute the production C parser, journal and command engine.

Examples: python tests/run_host_tests.py --cc gcc
          python tests/run_host_tests.py --cc path/to/zig cc
Only the NVS, clock and GPIO boundary is replaced by deterministic test doubles.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--cc", nargs="+", default=["cc"])
parser.add_argument("--cmake", default="cmake")
parser.add_argument("--core-root", type=Path, help="Read-only Core checkout for actual schema/gateway tests")
parser.add_argument("--core-site-packages", type=Path, help="Existing compatible Core dependencies (no install)")
parser.add_argument("--services", action="store_true", help="Opt-in disposable Docker PostgreSQL + Mosquitto TLS checks")
parser.add_argument("--full-core-services", action="store_true", help="Also run full Core pytest inside the service fixture")
args = parser.parse_args()
if args.services and not args.core_root:
    parser.error("--services requires --core-root and existing Core Python dependencies")
if args.full_core_services and not args.services:
    parser.error("--full-core-services requires --services")
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
crypto = ["tests/control_crypto_host.c", "-lbcrypt" if os.name == "nt" else "-lcrypto"]
control_exe = build / ("control_tests" + exe.suffix)
subprocess.run(args.cc + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-UNDEBUG",
    "-Itests/stubs", "-Itests/board_stubs", "-Ikz_relay/main",
    "-DKZ_BOARD_esp32_wroom=1", "-DCONFIG_IDF_TARGET_ESP32=1",
    *sources[:3], "kz_relay/main/control.c", "tests/fake_platform.c", "tests/test_control.c",
    *crypto, "-o", str(control_exe)], cwd=ROOT, check=True)
subprocess.run([str(control_exe)], cwd=ROOT, check=True)
bridge = build / ("control_bridge" + exe.suffix)
subprocess.run(args.cc + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-UNDEBUG",
    "-Itests/stubs", "-Itests/board_stubs", "-Ikz_relay/main",
    "-DKZ_BOARD_esp32_wroom=1", "-DCONFIG_IDF_TARGET_ESP32=1",
    *sources[:3], "kz_relay/main/control.c", "tests/fake_platform.c", "tests/control_bridge.c",
    *crypto, "-o", str(bridge)], cwd=ROOT, check=True)
if args.core_root:
    subprocess.run([sys.executable, "-B", str(ROOT / "tests/test_control_integration.py"),
        "--core-root", str(args.core_root), "--bridge", str(bridge)], check=True)

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

# The real MQTT worker/callbacks, parser, journal and command engine; only SDK
# boundaries are fake. No broker, sockets, private files or production credentials.
for board, target in boards:
    output = build / f"mqtt_{board}{exe.suffix}"
    # config.h's embedded CA symbol uses the same GNU asm extension as ESP-IDF.
    subprocess.run(common + ["-std=gnu11", "-Itests/mqtt_stubs", "-DKZ_LOCAL_CONFIG=1",
        "-DCONFIG_MBEDTLS_HAVE_TIME_DATE=1", f"-DKZ_BOARD_{board}=1",
        f"-DCONFIG_IDF_TARGET_{target}=1", "tests/test_mqtt.c", *sources[:3],
        "tests/fake_platform.c", "kz_relay/main/control.c", *crypto, "-o", str(output)], cwd=ROOT, check=True)
    result = subprocess.run([str(output)], capture_output=True, text=True, check=True)
    messages = build / f"mqtt_{board}.jsonl"
    messages.write_text("\n".join(line.split("\t", 1)[1]
        for line in result.stdout.splitlines() if "\t" in line) + "\n", encoding="utf-8")
    print(result.stdout.splitlines()[-1])
    if args.core_root:
        command = [sys.executable, "-B", str(ROOT / "tests/test_core_compat.py"),
            "--core-root", str(args.core_root), "--messages", str(messages)]
        if args.core_site_packages:
            command += ["--site-packages", str(args.core_site_packages)]
        subprocess.run(command, check=True)
    else:
        print("SKIP: actual Core compatibility (supply --core-root and installed dependencies)")

if args.services:
    service_bridge = build / ("control_service_bridge" + exe.suffix)
    subprocess.run(common + ["-O1", "-DKZ_SERVICE_RANDOM=1", "-DKZ_BOARD_esp32_wroom=1",
        "-DCONFIG_IDF_TARGET_ESP32=1", *sources[:3], "kz_relay/main/control.c",
        "tests/fake_platform.c", "tests/control_service_bridge.c", *crypto,
        "-o", str(service_bridge)], cwd=ROOT, check=True)
    worker_bridge = build / ("worker_service_bridge" + exe.suffix)
    subprocess.run(common + ["-std=gnu11", "-O1", "-Itests/mqtt_stubs",
        "-DKZ_SERVICE_RANDOM=1", "-DKZ_LOCAL_CONFIG=1",
        "-DCONFIG_MBEDTLS_HAVE_TIME_DATE=1", "-DKZ_BOARD_esp32_wroom=1",
        "-DCONFIG_IDF_TARGET_ESP32=1", "tests/worker_service_bridge.c", *sources[:3],
        "tests/fake_platform.c", "kz_relay/main/control.c", *crypto,
        "-o", str(worker_bridge)], cwd=ROOT, check=True)
    command = [sys.executable, "-B", "-u", str(ROOT / "tests/service_integration.py"),
        "--core-root", str(args.core_root), "--bridge", str(service_bridge),
        "--worker-bridge", str(worker_bridge)]
    if args.full_core_services:
        command.append("--full-core")
    subprocess.run(command, cwd=ROOT, check=True)

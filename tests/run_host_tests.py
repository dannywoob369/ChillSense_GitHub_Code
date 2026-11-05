"""Run the production flow/JSON helpers without ESP-IDF: python tests/run_host_tests.py."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = shutil.which("gcc") or shutil.which("clang")
if not compiler:
    raise SystemExit("Install GCC or Clang to run the host tests.")
build = root / "build"
build.mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="chillsense-tests-", dir=build) as directory:
    executable = Path(directory) / "test.exe"
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(root / "main"),
                    str(root / "tests/test_flow_telemetry.c"),
                    "-o", str(executable)], check=True)
    result = subprocess.run([str(executable)], check=True, text=True, capture_output=True)
    normal, maximum = map(json.loads, result.stdout.splitlines())
    assert normal["flow_gpm"] == 10 and normal["pulse_count"] == 4
    assert normal["total_gallons"] == 4 and normal["uptime_ms"] == 306000
    assert normal["building_id"] == "building1" and normal["unit_id"] == "101"
    assert normal["supply_temp_f"] is None and normal["return_temp_f"] is None
    assert normal["temperature_valid"] is False and normal["pulse_queue_drops"] == 0
    assert maximum["pulse_count"] == 4294967295 and maximum["flow_gpm"] == 0
    assert len(maximum["device_id"]) == 32
print("PASS: flow timing, debounce, offline restart, identities, JSON and buffer bounds")

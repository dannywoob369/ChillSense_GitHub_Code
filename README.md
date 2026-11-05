# ChillSense ESP32 Submeter Firmware

ESP32-S3 firmware prototype for the ChillSense senior design project. The current firmware:

- connects to Wi-Fi,
- connects to an MQTT broker over TLS,
- publishes telemetry every 5 seconds,
- counts dry-contact pulses from the Dwyer WMT2 flow meter on GPIO4,
- estimates flow rate from the time between pulses,
- tracks total gallons,
- reports null supply/return temperatures until measurement is implemented.

## Development hardware

- **MCU:** ESP32-S3-DevKitC-1-N8R8
- **Flash:** 8 MB
- **PSRAM:** 8 MB
- **Flow meter:** Dwyer WMT2-A-C-02-1
- **Pulse output:** dry contact
- **Pulse scale:** provisionally 1 pulse = 1 gallon; validate against the actual meter
- **Pulse input:** GPIO4

## Required software

- Windows 10/11
- VS Code
- Espressif ESP-IDF VS Code extension
- **ESP-IDF v6.0.2**
- Silicon Labs CP210x USB-to-UART driver when Windows does not install it automatically
- Git

See [docs/ESP32_SETUP.md](docs/ESP32_SETUP.md) for a new-computer setup walkthrough.

## First-time setup on a computer

1. Clone this repository.
2. Open the repository root in VS Code.
3. Make sure ESP-IDF **v6.0.2** is selected.
4. Select target `esp32s3`.
5. Connect the board through its **UART** USB-C port.
6. Select the CP210x COM port.
7. Open **ESP-IDF: SDK Configuration Editor (menuconfig)**.
8. Under **Example Connection Configuration**, enter the local Wi-Fi SSID and password.
9. Under **Example Configuration**, set the MQTT broker URI, username/password, and building/unit/device IDs. Use unique IDs per installation.
10. Save, build, flash, and monitor.

The generated `sdkconfig` is intentionally ignored by Git because it may contain local Wi-Fi credentials.

## Build / flash / monitor

From the VS Code Command Palette:

1. `ESP-IDF: Build your project`
2. `ESP-IDF: Flash (UART) Your Project`
3. `ESP-IDF: Monitor Device`

Before flashing again, exit the monitor with:

```text
Ctrl + ]
```

Otherwise the monitor may keep the COM port open and cause flashing to fail.

## MQTT telemetry

Default topic:

```text
chillsense/building1/101/telemetry
```

Example payload:

```json
{
  "device_id": "chillsense_unit101",
  "building_id": "building1",
  "unit_id": "101",
  "flow_gpm": 0.00,
  "total_gallons": 0.00,
  "pulse_count": 0,
  "supply_temp_f": null,
  "return_temp_f": null,
  "temperature_valid": false,
  "uptime_ms": 5000,
  "pulse_queue_drops": 0
}
```

Public brokers are only for development testing. Do not send sensitive data through them. A private broker should be used for deployment.

See [docs/VALIDATION.md](docs/VALIDATION.md) for reset timing, restart behavior, remaining device checks, and dashboard acceptance criteria. The HTML dashboard/backend is not included in this repository.

Run the portable flow and telemetry regression tests with GCC or Clang installed:

```text
python tests/run_host_tests.py
```

## Flow-meter wiring

See [docs/FLOW_METER_WIRING.md](docs/FLOW_METER_WIRING.md).

## Repository notes

Do not commit:

- `sdkconfig`
- `build/`
- `managed_components/`
- passwords, private MQTT credentials, or other secrets

ESP-IDF recreates the generated files and downloads managed components from `main/idf_component.yml` / `dependencies.lock`.


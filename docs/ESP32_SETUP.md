# ESP32 Development Setup

This document describes how to reproduce the working ChillSense ESP32 development environment on another Windows computer.

## 1. Install VS Code and Git

Install Git and Visual Studio Code normally.

Open the repository in VS Code. VS Code should recommend the **Espressif ESP-IDF** extension because `.vscode/extensions.json` is included.

## 2. Install ESP-IDF v6.0.2

Use Espressif's ESP-IDF Installation Manager and install **ESP-IDF v6.0.2**.

The project was developed and tested against v6.0.2. Avoid silently upgrading the project to another major/minor IDF version until the firmware is retested.

## 3. USB-to-UART driver

Connect the ESP32-S3 using the USB-C connector labeled **UART**.

In Windows Device Manager, a working board should appear under **Ports (COM & LPT)** similar to:

```text
Silicon Labs CP210x USB to UART Bridge (COM7)
```

The COM number can be different on every PC.

If it instead appears under **Other devices** as:

```text
CP2102N USB to UART Bridge Controller
```

with a warning icon / Code 28, install the Silicon Labs CP210x VCP driver, then reconnect the board.

## 4. Open and configure the project

Open the repository root in VS Code.

Select:

- ESP-IDF version: `v6.0.2`
- target: `esp32s3`
- flash method: `UART`
- serial port: the CP210x COM port

## 5. Configure local Wi-Fi

Run:

```text
ESP-IDF: SDK Configuration Editor (menuconfig)
```

Open **Example Connection Configuration** and enter the Wi-Fi SSID/password for that computer's test network.

These values are saved into the local `sdkconfig`. `sdkconfig` is ignored by Git so credentials are not pushed to GitHub.

ESP32-S3 Wi-Fi uses 2.4 GHz.

## 6. Configure MQTT

In the same SDK Configuration Editor, open **Example Configuration**.

Set **MQTT broker URI**. The shared default is currently:

```text
mqtts://broker.emqx.io:8883
```

This is only a development/test broker. For deployment, replace it with the project's private/cloud broker and use appropriate credentials/security.

Set **MQTT username**, **MQTT password**, and the building/unit/device IDs under the same menu. Firmware uses these settings; no broker credentials are hard-coded. IDs must contain 1-32 letters, digits, underscores, or hyphens. Invalid IDs stop startup with an error.

Use the certificate bundle for the cloud broker, or explicitly select an appropriate embedded/override CA and matching broker URI. Keep the generated sdkconfig and build output private: they contain local credentials. Replace credentials previously exposed in repository history before using the private broker.

## 7. Build

Run:

```text
ESP-IDF: Build your project
```

If IntelliSense shows many false missing-header errors after a full clean, build the project first. The build regenerates ESP-IDF include paths and compile information.

## 8. Flash

Stop any active ESP-IDF monitor first:

```text
Ctrl + ]
```

Then run:

```text
ESP-IDF: Flash (UART) Your Project
```

If flashing reports that the port is in use, close any serial monitor or another VS Code window holding the COM port.

## 9. Monitor

Run:

```text
ESP-IDF: Monitor Device
```

The firmware should connect to Wi-Fi, connect to MQTT, and print telemetry/pulse activity.

## Optional environment check

Run from PowerShell at the repository root:

```powershell
.\scripts\check_environment.ps1
```


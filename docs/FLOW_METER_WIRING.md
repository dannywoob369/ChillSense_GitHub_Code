# Dwyer WMT2 Flow Meter Wiring

## Meter output

The Dwyer WMT2-A-C-02-1 provides a **dry-contact closure** pulse output.

The meter does not generate a powered logic voltage. Electrically, the two pulse wires behave like the two terminals of a switch. The ESP32 supplies the logic pull-up voltage.

The current firmware provisionally assumes:

```text
1 pulse = 1 gallon
```

## ESP32 pins

Use the J1 header:

- J1 pin 1 or 2: `3V3`
- J1 pin 4: `GPIO4`
- J1 pin 22: `GND`

## Recommended prototype wiring

```text
ESP32 3V3
    |
   10 kΩ
    |
    +------------- GPIO4
    |                |
    |             Meter wire A
    |                |
    |          [dry contact]
    |                |
ESP32 GND -------- Meter wire B
```

One meter wire is assigned to the GPIO signal node and the other is assigned to ESP32 ground. The meter itself is not inherently a powered "signal + ground" device.

When the contact is open, GPIO4 is pulled HIGH. When the meter contact closes, GPIO4 is pulled LOW. Firmware counts the HIGH-to-LOW transition as one pulse.

## Manual pulse simulation

Before connecting the meter, GPIO4 can be tested by briefly connecting:

```text
GPIO4 -> GND
```

Disconnect and reconnect to generate separate falling edges. The firmware's internal pull-up is enabled, so this direct test can be done without the external resistor; use the external 10 kΩ pull-up for the actual prototype wiring.

Never connect 5 V directly to GPIO4.

## Flow-rate calculation

Because the meter produces 1 gallon per pulse, the firmware estimates flow from the time between pulses:

```text
flow_gpm = 60 / seconds_between_pulses
```

Examples:

- 3 seconds between pulses -> 20 GPM
- 6 seconds -> 10 GPM
- 12 seconds -> 5 GPM
- 60 seconds -> 1 GPM

Validate this scale with the actual meter's documentation and a measured-volume test. The source previously noted that the observed meter pulsed faster than this assumption. Do not treat the scale or the 100 ms debounce as calibrated.

The no-pulse timeout is clamp(3 × previous interval, 3 seconds, 90 seconds). At the timeout, telemetry reports zero; the next accepted pulse counts volume but starts a new session at zero GPM. The following pulse establishes the new rate. See [VALIDATION.md](VALIDATION.md).


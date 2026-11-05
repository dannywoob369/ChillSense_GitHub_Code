# MQTT and dashboard integration

Firmware publishes non-retained QoS 1 JSON every 5 seconds while connected.

- Topic: `chillsense/<building_id>/<unit_id>/telemetry`
- Default: `chillsense/building1/101/telemetry`
- Identity defaults: building1 / 101 / chillsense_unit101
- Configure broker URI, username/password, identity, and TLS validation under **Example Configuration** in menuconfig. Wi-Fi is under **Example Connection Configuration**.
- The default public broker uses the CA bundle and empty credentials. Configure the private broker locally; replace credentials exposed in the prior commit.

The payload retains flow_gpm, total_gallons, pulse_count, supply_temp_f, and return_temp_f, and includes device_id, building_id, unit_id, uptime_ms and pulse_queue_drops. IDs are strings, including unit_id. Temperatures are JSON null and temperature_valid is false until actual measurements exist. The single thermistor health LED does not establish supply/return temperature values.

## Subscriber and browser

Use MQTTX with the same broker and credentials to subscribe to the exact topic above, or `chillsense/+/+/telemetry` when ACLs permit. Confirm all three identities match the installed unit.

This repository contains ESP32 firmware, not the HTML dashboard or its backend. A browser MQTT client needs the broker's **WSS** endpoint/path and separate subscriber permissions; it cannot use the device's `mqtts://...:8883` raw TCP endpoint. Obtain WSS settings from the broker configuration. Do not embed the device's publisher password in browser HTML. A backend bridge is another option.

The consumer must:
- accept null temperatures and show “unavailable” instead of 0°F or the previous demo values;
- route on matching building/unit/device IDs;
- tolerate QoS 1 redelivery, without summing duplicate pulse_count/total_gallons snapshots;
- treat counters and uptime as since boot, not persistent lifetime totals;
- mark readings stale when updates stop, and clear staleness on a fresh update.

`uptime_ms` helps identify repeats and device restarts; it is not a wall-clock timestamp or globally unique event ID. Production deduplication across boots needs additional session identity or backend handling.

The yellow LED and “Telemetry queued” log mean the local publish call accepted the message. Only a matching `MQTT_EVENT_PUBLISHED` message ID establishes the broker's QoS 1 acknowledgement. MQTTX receipt and the dashboard display must be checked separately. Nothing is published retained, so a new subscriber waits for the next live sample. During an outage the flow task keeps counting, but periodic offline samples are not buffered.


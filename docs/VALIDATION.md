# Reset and telemetry validation

## Review basis and fixes

Reviewed main at 57ab0e68b1a50015a656632a83af7911ed4f5582, including:
- 8121af9: adaptive timeout = clamp(3 × last pulse interval, 3 s, 90 s), checked only in the connected telemetry path.
- 57ab0e6: building/unit/device JSON identity and topic, plus a hard-coded private broker and credentials.

The prior reset changed only a telemetry copy. On restart the next pulse measured across the entire idle gap and published a false low GPM. The shared production helper now uses the same timeout for pulse acceptance and snapshots. At or beyond the timeout, the first returning pulse counts volume and starts a new two-pulse measurement session. This works even when MQTT was disconnected throughout the stop.

ISR timestamps now go into a 32-entry queue instead of collapsing multiple notifications into one task-time pulse. Pulse debounce uses accepted edge times; total volume is unaffected by a flow timeout. pulse_queue_drops exposes queue overflow, which requires investigation and may mean lost volume. The MQTT connected flag is atomic across tasks, and initialization failures are checked.

Broker configuration and certificate options are honored again. Configurable identity is validated before JSON/topic construction. Both snprintf results are checked before publishing. Simulated temperatures are replaced by null plus temperature_valid=false. The yellow LED indicates local publish acceptance, and broker acknowledgement is logged separately.

## Checks completed on host

Run `python tests/run_host_tests.py` with GCC or Clang. It compiles the same flow and telemetry helpers used by firmware with C11 and -Wall -Wextra -Werror. Checks cover:
- boot with no pulses and a pulse at timestamp zero;
- debounce rejection below 100 ms and acceptance at the boundary;
- 3 s floor, 90 s cap, exact expiry and 1 GPM operation;
- restart after a long stop without any telemetry calls;
- total pulse count preservation and a fractional gallons-per-pulse scale;
- identity restrictions, topic, actual JSON parsing, null temperatures, and buffer limits.

These tests do not compile ESP-IDF integration or validate GPIO, FreeRTOS scheduling, TLS, MQTT delivery, or a browser. The current environment has no active ESP-IDF toolchain or attached device session.

## Device procedure

1. In the project's ESP-IDF environment (documented v6.0.2), select esp32s3. Configure Wi-Fi, broker URI/credentials, TLS trust method, and unique building/unit/device IDs in menuconfig. Replace the private-broker password exposed in the prior repository history. Build with `idf.py build`; flash/monitor with `idf.py -p COMx flash monitor`. Record IDF version, board, configuration (without secrets), firmware revision and serial log.
2. Before using water-flow results, verify the meter model and pulse scale. Run a measured-volume test, compare accepted pulse count, and set GALLONS_PER_PULSE accordingly. Confirm 100 ms debounce does not reject genuine pulses at maximum flow; bounce edges inside that window should count once. Do not guess a new calibration from the old comment.
3. With no pulses, confirm MQTTX sees zero GPM, zero count/volume, null temperatures and false temperature_valid. Subscribe to `chillsense/building1/101/telemetry` for the default identity. Check approximately 5 s update cadence, matching identity in topic and payload, and a matching PUBACK message ID.
4. Simulate 3.3 V-safe dry-contact falling edges on GPIO4 (external 10 kΩ pull-up to 3V3; contact to ground). With provisional 1 gallon/pulse, first edge adds one gallon and reports zero GPM; two edges 6 s apart establish 10 GPM. Continue at 6 s intervals, then stop. First telemetry sample at or after 18 s since the last edge must report zero, normally within another 5 s. Count/volume must remain unchanged.
5. After a stop beyond timeout, generate one edge: count rises by one but GPM stays zero. A second edge 6 s later restores 10 GPM. Repeat while MQTT is disconnected during the stop; reconnect both before and after resumed pulses. The first resumed edge must never compute a rate across idle time.
6. Test the floor with 1 s intervals (60 GPM, zero after 3 s), a middle interval with 12 s (5 GPM, zero after 36 s), and the cap with 60 s (1 GPM, zero after 90 s). Allow up to one publish period of observation delay. At 30 s intervals, the timeout is also 90 s. Use the calibrated scale for real-meter expected GPM.
7. Interrupt broker/Wi-Fi access while pulses continue. After recovery, current counters must include offline pulses and fresh messages must resume with PUBACKs. They must not replay a backlog of offline periodic samples. Confirm pulse_queue_drops stays zero under representative flow, switch bounce, reconnects and logging load. Exercise wrong MQTT credentials and wrong CA separately: connection must fail without an apparent successful send.
8. Connect the actual dashboard via its backend or broker WSS endpoint. Confirm routing for the configured unit, unavailable temperatures, live flow/count/volume, no summing of duplicate QoS 1 snapshots, and a stale indicator after device/network loss. A new subscriber should receive the next live sample; no retained reading is sent.
9. Power-cycle: counters and uptime reset. Confirm the dashboard handles this explicitly. Lifetime volume persistence and durable event deduplication across boots are not implemented.

## Remaining limits

No actual supply/return temperature measurement exists, and the single ADC thermistor task only detects plausible raw values. Two real temperature inputs, conversion/calibration and failure handling require separate implementation and hardware validation.

The 90 s cap cannot measure a steady pulse period of 90 s or more with the new session policy; revise it if the calibrated meter must support that rate. Stop detection within a timeout is inherently ambiguous. Totals use a uint32 pulse counter and floating-point gallon output, so lifetime accounting requires a separate persistence/precision design.

Dashboard HTML/backend and broker ACL/WSS configuration are outside this repository. End-to-end delivery cannot be claimed until steps 7-8 pass with those systems.

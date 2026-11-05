#ifndef CHILLSENSE_TELEMETRY_H
#define CHILLSENSE_TELEMETRY_H

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "flow_meter.h"

// Identity is used in JSON strings and MQTT topic segments.
static inline bool valid_identity(const char *value)
{
    size_t length = strlen(value);
    if (length == 0 || length > 32) return false;
    for (size_t i = 0; i < length; i++)
    {
        char c = value[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}


static inline int telemetry_payload(char *payload, size_t size,
                                    const char *device_id, const char *building_id,
                                    const char *unit_id, const flow_meter_t *meter,
                                    float gallons_per_pulse, int64_t now_us,
                                    unsigned queue_drops)
{
    return snprintf(payload, size,
        "{"
        "\"device_id\":\"%s\","
        "\"building_id\":\"%s\","
        "\"unit_id\":\"%s\","
        "\"flow_gpm\":%.2f,"
        "\"total_gallons\":%.2f,"
        "\"pulse_count\":%" PRIu32 ","
        "\"supply_temp_f\":null,"
        "\"return_temp_f\":null,"
        "\"temperature_valid\":false,"
        "\"uptime_ms\":%" PRIi64 ","
        "\"pulse_queue_drops\":%u"
        "}",
        device_id, building_id, unit_id, flow_current_gpm(meter, now_us),
        meter->total_pulses * gallons_per_pulse, meter->total_pulses,
        now_us / 1000, queue_drops);
}

static inline int telemetry_topic(char *topic, size_t size,
                                  const char *building_id, const char *unit_id)
{
    return snprintf(topic, size, "chillsense/%s/%s/telemetry", building_id, unit_id);
}

#endif

#include <assert.h>
#include <limits.h>
#include <math.h>
#include "flow_meter.h"
#include "telemetry.h"

static void near(float actual, float expected)
{
    assert(fabsf(actual - expected) < 0.001f);
}

int main(void)
{
    flow_meter_t meter = {0};
    near(flow_current_gpm(&meter, 0), 0);
    // First edge at boot must count; bounce must not.
    assert(flow_accept_pulse(&meter, 0, 1.0f));
    assert(!flow_accept_pulse(&meter, 99999, 1.0f));
    near(flow_current_gpm(&meter, 100000), 0);
    assert(flow_accept_pulse(&meter, 6000000, 1.0f));
    near(flow_current_gpm(&meter, 6000000), 10);
    near(flow_current_gpm(&meter, 23999999), 10);
    near(flow_current_gpm(&meter, 24000000), 0);
    assert(meter.total_pulses == 2);

    // Simulate no telemetry or MQTT during the stop. First resumed pulse is volume only.
    assert(flow_accept_pulse(&meter, 300000000, 1.0f));
    near(flow_current_gpm(&meter, 300000000), 0);
    assert(meter.interval_us == 0 && meter.total_pulses == 3);
    assert(flow_accept_pulse(&meter, 306000000, 1.0f));
    near(flow_current_gpm(&meter, 306000000), 10);

    // Floor, cap, exact boundary, low flow and a single isolated pulse.
    assert(flow_timeout_us(100000) == 3000000);
    assert(flow_timeout_us(6000000) == 18000000);
    assert(flow_timeout_us(30000000) == 90000000);
    assert(flow_timeout_us(INT64_MAX) == 90000000);
    flow_meter_t slow = {0};
    assert(flow_accept_pulse(&slow, 1000000, 1.0f));
    assert(flow_accept_pulse(&slow, 61000000, 1.0f));
    near(flow_current_gpm(&slow, 150999999), 1);
    near(flow_current_gpm(&slow, 151000000), 0);
    assert(flow_accept_pulse(&slow, 151000000, 1.0f));
    near(slow.flow_gpm, 0);
    flow_meter_t fast = {0};
    assert(flow_accept_pulse(&fast, 1, 0.1f));
    assert(flow_accept_pulse(&fast, 100001, 0.1f));
    near(fast.flow_gpm, 60);
    near(flow_current_gpm(&fast, 3100000), 60);
    near(flow_current_gpm(&fast, 3100001), 0);
    flow_meter_t single = {0};
    assert(flow_accept_pulse(&single, 1, 1));
    assert(flow_accept_pulse(&single, 90000001, 1));
    near(single.flow_gpm, 0);

    // Reject topic wildcards, separators and JSON injection before MQTT startup.
    assert(valid_identity("building1") && valid_identity("unit_101-A"));
    assert(!valid_identity("") && !valid_identity("a/b"));
    assert(!valid_identity("+") && !valid_identity("#"));
    assert(!valid_identity("a\"b") && !valid_identity("a\\b"));
    assert(!valid_identity("123456789012345678901234567890123"));

    char payload[512], topic[128], tiny[2];
    int n = telemetry_payload(payload, sizeof(payload), "chillsense_unit101",
                              "building1", "101", &meter, 1, 306000000, 0);
    assert(n > 0 && (size_t)n < sizeof(payload));
    puts(payload);
    assert(telemetry_topic(topic, sizeof(topic), "building1", "101") > 0);
    assert(strcmp(topic, "chillsense/building1/101/telemetry") == 0);
    assert(telemetry_payload(tiny, sizeof(tiny), "d", "b", "u",
                             &meter, 1, 306000000, 0) >= (int)sizeof(tiny));
    assert(telemetry_topic(tiny, sizeof(tiny), "b", "u") >= (int)sizeof(tiny));
    const char *max_id = "12345678901234567890123456789012";
    meter.total_pulses = UINT32_MAX;
    n = telemetry_payload(payload, sizeof(payload), max_id, max_id, max_id,
                          &meter, 1, INT64_MAX, UINT_MAX);
    assert(n > 0 && (size_t)n < sizeof(payload));
    puts(payload);
    n = telemetry_topic(topic, sizeof(topic), max_id, max_id);
    assert(n > 0 && (size_t)n < sizeof(topic));
    return 0;
}

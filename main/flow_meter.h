#ifndef CHILLSENSE_FLOW_METER_H
#define CHILLSENSE_FLOW_METER_H

#include <stdbool.h>
#include <stdint.h>

#define DEBOUNCE_TIME_US 100000LL
#define FLOW_MIN_TIMEOUT_US 3000000LL
#define FLOW_TIMEOUT_US 90000000LL

typedef struct {
    uint32_t total_pulses;
    bool has_pulse;
    int64_t last_pulse_us;
    int64_t interval_us;
    float flow_gpm;
} flow_meter_t;

static inline int64_t flow_timeout_us(int64_t interval_us)
{
    int64_t timeout = interval_us < FLOW_TIMEOUT_US / 3
                          ? interval_us * 3 : FLOW_TIMEOUT_US;
    return timeout < FLOW_MIN_TIMEOUT_US ? FLOW_MIN_TIMEOUT_US : timeout;
}

static inline bool flow_is_stale(const flow_meter_t *meter, int64_t now_us)
{
    return !meter->has_pulse || meter->interval_us <= 0 ||
           now_us - meter->last_pulse_us >= flow_timeout_us(meter->interval_us);
}

// Both pulse acceptance and telemetry use the same timeout, even while offline.
static inline bool flow_accept_pulse(flow_meter_t *meter, int64_t pulse_us,
                                     float gallons_per_pulse)
{
    int64_t gap = pulse_us - meter->last_pulse_us;
    if (meter->has_pulse && gap < DEBOUNCE_TIME_US) {
        return false;
    }

    // A stopped session needs two fresh pulses. Never measure across idle time.
    int64_t timeout = meter->interval_us > 0
                          ? flow_timeout_us(meter->interval_us) : FLOW_TIMEOUT_US;
    if (meter->has_pulse && gap < timeout) {
        meter->interval_us = gap;
        meter->flow_gpm = gallons_per_pulse * 60000000.0f / (float)gap;
    } else {
        meter->interval_us = 0;
        meter->flow_gpm = 0.0f;
    }
    meter->has_pulse = true;
    meter->last_pulse_us = pulse_us;
    meter->total_pulses++;
    return true;
}

static inline float flow_current_gpm(const flow_meter_t *meter, int64_t now_us)
{
    return flow_is_stale(meter, now_us) ? 0.0f : meter->flow_gpm;
}

#endif

#include "guardian/guardian.h"

#include <string.h>

#define GUARDIAN_DEFAULT_ATTEMPTS 2U
#define GUARDIAN_DEFAULT_PULSES GUARDIAN_MAX_RECOVERY_PULSES
#define GUARDIAN_DEFAULT_PULSE_US 5U
#define GUARDIAN_DEFAULT_SETUP_US 5U

static bool platform_complete(const guardian_platform_t *p)
{
    return p != NULL && p->sample_lines != NULL &&
           p->enter_gpio_open_drain != NULL && p->drive_scl_low != NULL &&
           p->release_scl != NULL && p->drive_sda_low != NULL &&
           p->release_sda != NULL && p->delay_us != NULL &&
           p->restore_i2c_peripheral != NULL;
}

static guardian_device_health_t *find_device(guardian_t *g, uint8_t address)
{
    size_t i;
    for (i = 0U; i < GUARDIAN_MAX_DEVICES; ++i) {
        if (g->devices[i].registered && g->devices[i].address_7bit == address) {
            return &g->devices[i];
        }
    }
    return NULL;
}

static const guardian_device_health_t *find_device_const(const guardian_t *g,
                                                           uint8_t address)
{
    size_t i;
    for (i = 0U; i < GUARDIAN_MAX_DEVICES; ++i) {
        if (g->devices[i].registered && g->devices[i].address_7bit == address) {
            return &g->devices[i];
        }
    }
    return NULL;
}

guardian_result_t guardian_init(guardian_t *g, const guardian_platform_t *p)
{
    if (g == NULL || !platform_complete(p)) {
        return GUARDIAN_EINVAL;
    }
    memset(g, 0, sizeof(*g));
    g->platform = *p;
    g->max_recovery_attempts = GUARDIAN_DEFAULT_ATTEMPTS;
    g->max_clock_pulses = GUARDIAN_DEFAULT_PULSES;
    g->recovery_pulse_us = GUARDIAN_DEFAULT_PULSE_US;
    g->recovery_setup_delay_us = GUARDIAN_DEFAULT_SETUP_US;
    return GUARDIAN_OK;
}

guardian_result_t guardian_configure_recovery(guardian_t *g,
                                               uint8_t attempts,
                                               uint8_t pulses,
                                               uint32_t pulse_us,
                                               uint32_t setup_us)
{
    if (g == NULL || attempts == 0U || pulses == 0U ||
        pulses > GUARDIAN_MAX_RECOVERY_PULSES ||
        pulse_us == 0U || setup_us == 0U) {
        return GUARDIAN_EINVAL;
    }
    if (g->recovery_in_progress) {
        return GUARDIAN_EBUSY;
    }
    g->max_recovery_attempts = attempts;
    g->max_clock_pulses = pulses;
    g->recovery_pulse_us = pulse_us;
    g->recovery_setup_delay_us = setup_us;
    return GUARDIAN_OK;
}

guardian_result_t guardian_register_device(guardian_t *g,
                                            const guardian_device_config_t *c)
{
    size_t i;
    if (g == NULL || c == NULL || c->address_7bit < 0x08U ||
        c->address_7bit > 0x77U || c->failure_threshold == 0U ||
        c->offline_threshold < c->failure_threshold) {
        return GUARDIAN_EINVAL;
    }
    if (find_device(g, c->address_7bit) != NULL) {
        return GUARDIAN_EINVAL;
    }
    for (i = 0U; i < GUARDIAN_MAX_DEVICES; ++i) {
        if (!g->devices[i].registered) {
            guardian_device_health_t *d = &g->devices[i];
            memset(d, 0, sizeof(*d));
            d->registered = true;
            d->address_7bit = c->address_7bit;
            d->failure_threshold = c->failure_threshold;
            d->offline_threshold = c->offline_threshold;
            d->retry_interval_ticks = c->retry_interval_ticks;
            d->last_result = GUARDIAN_LAST_NONE;
            d->state = GUARDIAN_DEVICE_HEALTHY;
            ++g->device_count;
            return GUARDIAN_OK;
        }
    }
    return GUARDIAN_EFULL;
}

guardian_result_t guardian_get_device(const guardian_t *g,
                                       uint8_t address,
                                       guardian_device_health_t *out)
{
    const guardian_device_health_t *found;
    if (g == NULL || out == NULL) {
        return GUARDIAN_EINVAL;
    }
    found = find_device_const(g, address);
    if (found == NULL) {
        return GUARDIAN_ENOTFOUND;
    }
    *out = *found;
    return GUARDIAN_OK;
}

guardian_result_t guardian_record_transaction(guardian_t *g, uint8_t address,
                                               guardian_transaction_result_t result,
                                               uint32_t now)
{
    guardian_device_health_t *d;
    guardian_device_state_t old_state;
    if (g == NULL || result < GUARDIAN_LAST_SUCCESS ||
        result > GUARDIAN_LAST_RECOVERY_FAILED) {
        return GUARDIAN_EINVAL;
    }
    d = find_device(g, address);
    if (d == NULL) {
        return GUARDIAN_ENOTFOUND;
    }
    old_state = d->state;
    ++d->transaction_count;
    d->last_attempt_tick = now;
    d->last_result = result;
    d->retry_scheduled = false;

    if (result == GUARDIAN_LAST_SUCCESS) {
        ++d->success_count;
        d->consecutive_failures = 0U;
        d->state = GUARDIAN_DEVICE_HEALTHY;
    } else {
        ++d->failure_count;
        if (d->consecutive_failures < UINT8_MAX) {
            ++d->consecutive_failures;
        }
        if (d->consecutive_failures >= d->offline_threshold) {
            d->state = GUARDIAN_DEVICE_OFFLINE;
        } else if (d->consecutive_failures >= d->failure_threshold) {
            d->state = GUARDIAN_DEVICE_DEGRADED;
        }
        d->retry_scheduled = d->state != GUARDIAN_DEVICE_HEALTHY;
    }
    if (old_state != d->state && d->state == GUARDIAN_DEVICE_DEGRADED) {
        g->events |= GUARDIAN_EVENT_DEVICE_DEGRADED;
    } else if (old_state != d->state && d->state == GUARDIAN_DEVICE_OFFLINE) {
        g->events |= GUARDIAN_EVENT_DEVICE_OFFLINE;
    }
    return GUARDIAN_OK;
}

guardian_result_t guardian_diagnose_bus(guardian_t *g, guardian_bus_state_t *out)
{
    bool sda_high = false;
    bool scl_high = false;
    if (g == NULL || out == NULL) {
        return GUARDIAN_EINVAL;
    }
    if (!g->platform.sample_lines(g->platform.context, &sda_high, &scl_high)) {
        *out = GUARDIAN_BUS_UNKNOWN;
        return GUARDIAN_EIO;
    }
    if (sda_high && scl_high) {
        *out = GUARDIAN_BUS_IDLE;
    } else if (!sda_high && scl_high) {
        *out = GUARDIAN_BUS_SDA_LOW;
    } else if (sda_high && !scl_high) {
        *out = GUARDIAN_BUS_SCL_LOW;
    } else {
        *out = GUARDIAN_BUS_BOTH_LOW;
    }
    if (*out != GUARDIAN_BUS_IDLE) {
        g->events |= GUARDIAN_EVENT_BUS_FAULT;
    }
    return GUARDIAN_OK;
}

static bool call_bool(bool (*fn)(void *), void *context)
{
    return fn(context);
}

static bool bus_pulse(guardian_t *g)
{
    void *ctx = g->platform.context;
    uint8_t pulse;
    for (pulse = 0U; pulse < g->max_clock_pulses; ++pulse) {
        if (!call_bool(g->platform.drive_scl_low, ctx)) {
            return false;
        }
        g->platform.delay_us(ctx, g->recovery_pulse_us);
        if (!call_bool(g->platform.release_scl, ctx)) {
            return false;
        }
        g->platform.delay_us(ctx, g->recovery_pulse_us);
        {
            bool sda = false;
            bool scl = false;
            if (!g->platform.sample_lines(ctx, &sda, &scl)) {
                return false;
            }
            if (!scl) {
                /* An SCL-low target prevents safe clocking; fail boundedly. */
                return false;
            }
            if (sda) {
                break;
            }
        }
    }
    {
        bool sda = false;
        bool scl = false;
        if (!g->platform.sample_lines(ctx, &sda, &scl) || !sda || !scl) {
            return false;
        }
    }
    /* Generate STOP: SDA low while SCL low, release SCL, then release SDA. */
    if (!call_bool(g->platform.drive_scl_low, ctx) ||
        !call_bool(g->platform.drive_sda_low, ctx)) {
        return false;
    }
    g->platform.delay_us(ctx, g->recovery_pulse_us);
    if (!call_bool(g->platform.release_scl, ctx)) {
        return false;
    }
    g->platform.delay_us(ctx, g->recovery_pulse_us);
    if (!call_bool(g->platform.release_sda, ctx)) {
        return false;
    }
    g->platform.delay_us(ctx, g->recovery_pulse_us);
    {
        bool sda = false;
        bool scl = false;
        return g->platform.sample_lines(ctx, &sda, &scl) && sda && scl;
    }
}

guardian_result_t guardian_recover_bus(guardian_t *g, uint32_t now,
                                        guardian_bus_state_t *out_final)
{
    guardian_bus_state_t initial;
    guardian_bus_state_t final_state = GUARDIAN_BUS_UNKNOWN;
    bool success = false;
    uint8_t attempt;
    (void)now;

    if (g == NULL || out_final == NULL) {
        return GUARDIAN_EINVAL;
    }
    if (g->recovery_in_progress) {
        return GUARDIAN_EBUSY;
    }
    if (guardian_diagnose_bus(g, &initial) != GUARDIAN_OK) {
        *out_final = GUARDIAN_BUS_UNKNOWN;
        return GUARDIAN_EIO;
    }
    if (initial == GUARDIAN_BUS_IDLE) {
        *out_final = initial;
        return GUARDIAN_OK;
    }
    if (initial == GUARDIAN_BUS_SCL_LOW || initial == GUARDIAN_BUS_BOTH_LOW) {
        /* Do not force clocks when SCL is not high; a target may be stretching. */
        *out_final = initial;
        g->events |= GUARDIAN_EVENT_RECOVERY_FAILED;
        ++g->recovery_failures;
        return GUARDIAN_EUNSAFE;
    }

    g->recovery_in_progress = true;
    g->events |= GUARDIAN_EVENT_RECOVERY_STARTED;
    for (attempt = 0U; attempt < g->max_recovery_attempts; ++attempt) {
        ++g->recovery_attempts;
        if (!call_bool(g->platform.enter_gpio_open_drain, g->platform.context)) {
            break;
        }
        g->platform.delay_us(g->platform.context, g->recovery_setup_delay_us);
        if (bus_pulse(g)) {
            success = true;
            break;
        }
    }
    if (!call_bool(g->platform.restore_i2c_peripheral, g->platform.context)) {
        success = false;
    }
    if (guardian_diagnose_bus(g, &final_state) != GUARDIAN_OK) {
        final_state = GUARDIAN_BUS_UNKNOWN;
        success = false;
    }
    g->recovery_in_progress = false;
    *out_final = final_state;

    if (success && final_state == GUARDIAN_BUS_IDLE) {
        ++g->recovery_successes;
        g->events |= GUARDIAN_EVENT_RECOVERY_SUCCEEDED;
        return GUARDIAN_OK;
    }
    ++g->recovery_failures;
    g->events |= GUARDIAN_EVENT_RECOVERY_FAILED;
    return GUARDIAN_EIO;
}

bool guardian_device_retry_due(const guardian_device_health_t *d, uint32_t now)
{
    if (d == NULL || !d->registered || !d->retry_scheduled ||
        d->state == GUARDIAN_DEVICE_HEALTHY) {
        return false;
    }
    return (uint32_t)(now - d->last_attempt_tick) >= d->retry_interval_ticks;
}

uint32_t guardian_get_events(const guardian_t *g)
{
    return g == NULL ? GUARDIAN_EVENT_NONE : g->events;
}

void guardian_clear_events(guardian_t *g, uint32_t events)
{
    if (g != NULL) {
        g->events &= ~events;
    }
}

const char *guardian_result_name(guardian_result_t r)
{
    switch (r) {
        case GUARDIAN_OK: return "OK";
        case GUARDIAN_EINVAL: return "INVALID_ARGUMENT";
        case GUARDIAN_ENOTFOUND: return "NOT_FOUND";
        case GUARDIAN_EFULL: return "CAPACITY_FULL";
        case GUARDIAN_EBUSY: return "BUSY";
        case GUARDIAN_EIO: return "IO_ERROR";
        case GUARDIAN_EUNSAFE: return "UNSAFE_TO_RECOVER";
        default: return "UNKNOWN_RESULT";
    }
}

const char *guardian_bus_state_name(guardian_bus_state_t s)
{
    switch (s) {
        case GUARDIAN_BUS_IDLE: return "IDLE";
        case GUARDIAN_BUS_SDA_LOW: return "SDA_LOW";
        case GUARDIAN_BUS_SCL_LOW: return "SCL_LOW";
        case GUARDIAN_BUS_BOTH_LOW: return "SDA_AND_SCL_LOW";
        case GUARDIAN_BUS_UNKNOWN: return "UNKNOWN";
        default: return "INVALID_BUS_STATE";
    }
}

const char *guardian_device_state_name(guardian_device_state_t s)
{
    switch (s) {
        case GUARDIAN_DEVICE_HEALTHY: return "HEALTHY";
        case GUARDIAN_DEVICE_DEGRADED: return "DEGRADED";
        case GUARDIAN_DEVICE_OFFLINE: return "OFFLINE";
        default: return "INVALID_DEVICE_STATE";
    }
}

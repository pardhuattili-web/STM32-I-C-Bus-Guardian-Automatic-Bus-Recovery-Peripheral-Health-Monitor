#ifndef GUARDIAN_GUARDIAN_H
#define GUARDIAN_GUARDIAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GUARDIAN_MAX_DEVICES 16U
#define GUARDIAN_MAX_RECOVERY_PULSES 9U

typedef enum {
    GUARDIAN_OK = 0,
    GUARDIAN_EINVAL,
    GUARDIAN_ENOTFOUND,
    GUARDIAN_EFULL,
    GUARDIAN_EBUSY,
    GUARDIAN_EIO,
    GUARDIAN_EUNSAFE
} guardian_result_t;

typedef enum {
    GUARDIAN_BUS_IDLE = 0,
    GUARDIAN_BUS_SDA_LOW,
    GUARDIAN_BUS_SCL_LOW,
    GUARDIAN_BUS_BOTH_LOW,
    GUARDIAN_BUS_UNKNOWN
} guardian_bus_state_t;

typedef enum {
    GUARDIAN_DEVICE_HEALTHY = 0,
    GUARDIAN_DEVICE_DEGRADED,
    GUARDIAN_DEVICE_OFFLINE
} guardian_device_state_t;

typedef enum {
    GUARDIAN_LAST_NONE = 0,
    GUARDIAN_LAST_SUCCESS,
    GUARDIAN_LAST_NACK,
    GUARDIAN_LAST_TIMEOUT,
    GUARDIAN_LAST_BUS_ERROR,
    GUARDIAN_LAST_RECOVERY_FAILED
} guardian_transaction_result_t;

typedef enum {
    GUARDIAN_EVENT_NONE = 0U,
    GUARDIAN_EVENT_BUS_FAULT = 1U << 0,
    GUARDIAN_EVENT_RECOVERY_STARTED = 1U << 1,
    GUARDIAN_EVENT_RECOVERY_SUCCEEDED = 1U << 2,
    GUARDIAN_EVENT_RECOVERY_FAILED = 1U << 3,
    GUARDIAN_EVENT_DEVICE_DEGRADED = 1U << 4,
    GUARDIAN_EVENT_DEVICE_OFFLINE = 1U << 5
} guardian_event_t;

typedef struct {
    /* Return true and write sampled pin levels on success. */
    bool (*sample_lines)(void *context, bool *sda_high, bool *scl_high);
    /* All recovery callbacks must be bounded and run with exclusive bus ownership. */
    bool (*enter_gpio_open_drain)(void *context);
    bool (*drive_scl_low)(void *context);
    bool (*release_scl)(void *context);
    bool (*drive_sda_low)(void *context);
    bool (*release_sda)(void *context);
    void (*delay_us)(void *context, uint32_t microseconds);
    bool (*restore_i2c_peripheral)(void *context);
    void *context;
} guardian_platform_t;

typedef struct {
    uint8_t address_7bit;
    uint8_t failure_threshold;
    uint8_t offline_threshold;
    uint32_t retry_interval_ticks;
} guardian_device_config_t;

typedef struct {
    bool registered;
    uint8_t address_7bit;
    uint8_t failure_threshold;
    uint8_t offline_threshold;
    uint8_t consecutive_failures;
    uint32_t retry_interval_ticks;
    uint32_t last_attempt_tick;
    uint32_t transaction_count;
    uint32_t success_count;
    uint32_t failure_count;
    guardian_transaction_result_t last_result;
    guardian_device_state_t state;
    bool retry_scheduled;
} guardian_device_health_t;

typedef struct {
    guardian_platform_t platform;
    guardian_device_health_t devices[GUARDIAN_MAX_DEVICES];
    size_t device_count;
    uint32_t events;
    uint32_t recovery_attempts;
    uint32_t recovery_successes;
    uint32_t recovery_failures;
    uint8_t max_recovery_attempts;
    uint8_t max_clock_pulses;
    uint32_t recovery_pulse_us;
    uint32_t recovery_setup_delay_us;
    bool recovery_in_progress;
} guardian_t;

/* Initialize with a complete platform adapter. No dynamic allocation is used. */
guardian_result_t guardian_init(guardian_t *guardian,
                                const guardian_platform_t *platform);

/* Tune conservative bounds; zero/invalid values are rejected. */
guardian_result_t guardian_configure_recovery(guardian_t *guardian,
                                               uint8_t max_attempts,
                                               uint8_t max_clock_pulses,
                                               uint32_t pulse_us,
                                               uint32_t setup_delay_us);

/* Addresses are unshifted 7-bit I²C addresses; reserved addresses are rejected. */
guardian_result_t guardian_register_device(guardian_t *guardian,
                                            const guardian_device_config_t *config);
guardian_result_t guardian_get_device(const guardian_t *guardian,
                                       uint8_t address_7bit,
                                       guardian_device_health_t *out_health);

/* Record the completion of a transaction. Tick is a wrapping monotonic counter. */
guardian_result_t guardian_record_transaction(guardian_t *guardian,
                                               uint8_t address_7bit,
                                               guardian_transaction_result_t result,
                                               uint32_t now_tick);

/* Ask the platform to sample SDA/SCL and classify their idle state. */
guardian_result_t guardian_diagnose_bus(guardian_t *guardian,
                                         guardian_bus_state_t *out_state);

/*
 * Attempt a bounded 9-clock-style recovery sequence. Caller must quiesce I²C/DMA,
 * own the bus exclusively, and invoke this from a context safe for GPIO operations.
 */
guardian_result_t guardian_recover_bus(guardian_t *guardian,
                                        uint32_t now_tick,
                                        guardian_bus_state_t *out_final_state);

/* Retry a registered degraded/offline device only after its cooldown has elapsed. */
bool guardian_device_retry_due(const guardian_device_health_t *health,
                               uint32_t now_tick);

/* Event flags are latched until cleared. */
uint32_t guardian_get_events(const guardian_t *guardian);
void guardian_clear_events(guardian_t *guardian, uint32_t events_to_clear);
const char *guardian_result_name(guardian_result_t result);
const char *guardian_bus_state_name(guardian_bus_state_t state);
const char *guardian_device_state_name(guardian_device_state_t state);

#ifdef __cplusplus
}
#endif
#endif

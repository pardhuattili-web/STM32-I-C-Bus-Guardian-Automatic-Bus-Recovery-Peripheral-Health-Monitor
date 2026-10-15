#include "guardian/guardian.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { \
    (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
    return false; \
} } while (0)

typedef struct {
    bool sda;
    bool scl;
    bool fail_sample;
    bool fail_restore;
    unsigned pulses;
    unsigned delay_calls;
    unsigned gpio_entries;
    unsigned restore_calls;
} fake_bus_t;

static bool sample(void *ctx, bool *sda, bool *scl)
{
    fake_bus_t *b = (fake_bus_t *)ctx;
    if (b->fail_sample) return false;
    *sda = b->sda; *scl = b->scl;
    return true;
}
static bool enter_gpio(void *ctx) { ((fake_bus_t *)ctx)->gpio_entries++; return true; }
static bool drive_scl(void *ctx) { ((fake_bus_t *)ctx)->scl = false; return true; }
static bool release_scl(void *ctx)
{
    fake_bus_t *b = (fake_bus_t *)ctx;
    b->scl = true;
    if (!b->sda) {
        ++b->pulses;
        /* Simulate a peripheral releasing SDA after three recovery clocks. */
        if (b->pulses >= 3U) b->sda = true;
    }
    return true;
}
static bool drive_sda(void *ctx) { ((fake_bus_t *)ctx)->sda = false; return true; }
static bool release_sda(void *ctx) { ((fake_bus_t *)ctx)->sda = true; return true; }
static void delay_us(void *ctx, uint32_t us) { (void)us; ((fake_bus_t *)ctx)->delay_calls++; }
static bool restore(void *ctx)
{
    fake_bus_t *b = (fake_bus_t *)ctx;
    ++b->restore_calls;
    return !b->fail_restore;
}

static bool setup(guardian_t *g, fake_bus_t *bus)
{
    guardian_platform_t p;
    memset(&p, 0, sizeof(p));
    p.sample_lines = sample; p.enter_gpio_open_drain = enter_gpio;
    p.drive_scl_low = drive_scl; p.release_scl = release_scl;
    p.drive_sda_low = drive_sda; p.release_sda = release_sda;
    p.delay_us = delay_us; p.restore_i2c_peripheral = restore; p.context = bus;
    return guardian_init(g, &p) == GUARDIAN_OK;
}

static bool test_init_validation(void)
{
    guardian_t g;
    fake_bus_t b = { true, true, false, false, 0U, 0U, 0U, 0U };
    guardian_platform_t p;
    memset(&p, 0, sizeof(p));
    CHECK(guardian_init(NULL, &p) == GUARDIAN_EINVAL);
    CHECK(guardian_init(&g, &p) == GUARDIAN_EINVAL);
    CHECK(setup(&g, &b));
    CHECK(guardian_configure_recovery(&g, 0U, 9U, 5U, 5U) == GUARDIAN_EINVAL);
    CHECK(guardian_configure_recovery(&g, 2U, 10U, 5U, 5U) == GUARDIAN_EINVAL);
    return true;
}

static bool test_line_diagnosis(void)
{
    guardian_t g; guardian_bus_state_t state;
    fake_bus_t b = { true, true, false, false, 0U, 0U, 0U, 0U };
    CHECK(setup(&g, &b));
    CHECK(guardian_diagnose_bus(&g, &state) == GUARDIAN_OK && state == GUARDIAN_BUS_IDLE);
    b.sda = false; b.scl = true;
    CHECK(guardian_diagnose_bus(&g, &state) == GUARDIAN_OK && state == GUARDIAN_BUS_SDA_LOW);
    b.sda = true; b.scl = false;
    CHECK(guardian_diagnose_bus(&g, &state) == GUARDIAN_OK && state == GUARDIAN_BUS_SCL_LOW);
    b.sda = false; b.scl = false;
    CHECK(guardian_diagnose_bus(&g, &state) == GUARDIAN_OK && state == GUARDIAN_BUS_BOTH_LOW);
    CHECK((guardian_get_events(&g) & GUARDIAN_EVENT_BUS_FAULT) != 0U);
    b.fail_sample = true;
    CHECK(guardian_diagnose_bus(&g, &state) == GUARDIAN_EIO && state == GUARDIAN_BUS_UNKNOWN);
    return true;
}

static bool test_device_health_and_cooldown(void)
{
    guardian_t g; guardian_device_health_t h;
    fake_bus_t b = { true, true, false, false, 0U, 0U, 0U, 0U };
    guardian_device_config_t c = { 0x48U, 2U, 4U, 10U };
    CHECK(setup(&g, &b));
    CHECK(guardian_register_device(&g, &c) == GUARDIAN_OK);
    CHECK(guardian_register_device(&g, &c) == GUARDIAN_EINVAL);
    CHECK(guardian_register_device(&g, &(guardian_device_config_t){0x03U, 1U, 2U, 1U}) == GUARDIAN_EINVAL);
    CHECK(guardian_record_transaction(&g, 0x48U, GUARDIAN_LAST_TIMEOUT, 100U) == GUARDIAN_OK);
    CHECK(guardian_get_device(&g, 0x48U, &h) == GUARDIAN_OK);
    CHECK(h.state == GUARDIAN_DEVICE_HEALTHY && h.consecutive_failures == 1U);
    CHECK(guardian_record_transaction(&g, 0x48U, GUARDIAN_LAST_NACK, 101U) == GUARDIAN_OK);
    CHECK(guardian_get_device(&g, 0x48U, &h) == GUARDIAN_OK);
    CHECK(h.state == GUARDIAN_DEVICE_DEGRADED && h.retry_scheduled);
    CHECK(!guardian_device_retry_due(&h, 110U));
    CHECK(guardian_device_retry_due(&h, 111U));
    CHECK(guardian_record_transaction(&g, 0x48U, GUARDIAN_LAST_BUS_ERROR, 112U) == GUARDIAN_OK);
    CHECK(guardian_record_transaction(&g, 0x48U, GUARDIAN_LAST_BUS_ERROR, 113U) == GUARDIAN_OK);
    CHECK(guardian_get_device(&g, 0x48U, &h) == GUARDIAN_OK && h.state == GUARDIAN_DEVICE_OFFLINE);
    CHECK((guardian_get_events(&g) & GUARDIAN_EVENT_DEVICE_OFFLINE) != 0U);
    CHECK(guardian_record_transaction(&g, 0x48U, GUARDIAN_LAST_SUCCESS, 120U) == GUARDIAN_OK);
    CHECK(guardian_get_device(&g, 0x48U, &h) == GUARDIAN_OK);
    CHECK(h.state == GUARDIAN_DEVICE_HEALTHY && h.consecutive_failures == 0U);
    CHECK(h.success_count == 1U && h.failure_count == 4U && h.transaction_count == 5U);
    CHECK(guardian_get_device(&g, 0x49U, &h) == GUARDIAN_ENOTFOUND);
    return true;
}

static bool test_successful_bounded_recovery(void)
{
    guardian_t g; guardian_bus_state_t state;
    fake_bus_t b = { false, true, false, false, 0U, 0U, 0U, 0U };
    CHECK(setup(&g, &b));
    CHECK(guardian_recover_bus(&g, 0U, &state) == GUARDIAN_OK);
    CHECK(state == GUARDIAN_BUS_IDLE);
    CHECK(b.pulses == 4U && b.gpio_entries == 1U && b.restore_calls == 1U); /* Includes STOP's SCL transition in fake counter. */
    CHECK(g.recovery_attempts == 1U && g.recovery_successes == 1U);
    CHECK((guardian_get_events(&g) & GUARDIAN_EVENT_RECOVERY_SUCCEEDED) != 0U);
    return true;
}

static bool test_refuse_clocking_when_scl_low(void)
{
    guardian_t g; guardian_bus_state_t state;
    fake_bus_t b = { true, false, false, false, 0U, 0U, 0U, 0U };
    CHECK(setup(&g, &b));
    CHECK(guardian_recover_bus(&g, 0U, &state) == GUARDIAN_EUNSAFE);
    CHECK(state == GUARDIAN_BUS_SCL_LOW && b.gpio_entries == 0U && b.pulses == 0U);
    CHECK(g.recovery_failures == 1U);
    return true;
}

static bool test_failure_paths_and_events(void)
{
    guardian_t g; guardian_bus_state_t state;
    fake_bus_t b = { false, true, false, true, 0U, 0U, 0U, 0U };
    CHECK(setup(&g, &b));
    CHECK(guardian_recover_bus(&g, 0U, &state) == GUARDIAN_EIO);
    CHECK(b.restore_calls == 1U);
    CHECK(guardian_get_events(&g) & GUARDIAN_EVENT_RECOVERY_FAILED);
    guardian_clear_events(&g, GUARDIAN_EVENT_RECOVERY_FAILED);
    CHECK((guardian_get_events(&g) & GUARDIAN_EVENT_RECOVERY_FAILED) == 0U);
    b.fail_sample = true;
    CHECK(guardian_recover_bus(&g, 0U, &state) == GUARDIAN_EIO && state == GUARDIAN_BUS_UNKNOWN);
    return true;
}

static bool test_counter_wrap_retry(void)
{
    guardian_t g; guardian_device_health_t h;
    fake_bus_t b = { true, true, false, false, 0U, 0U, 0U, 0U };
    guardian_device_config_t c = { 0x50U, 1U, 2U, 8U };
    CHECK(setup(&g, &b));
    CHECK(guardian_register_device(&g, &c) == GUARDIAN_OK);
    CHECK(guardian_record_transaction(&g, 0x50U, GUARDIAN_LAST_TIMEOUT, UINT32_MAX - 3U) == GUARDIAN_OK);
    CHECK(guardian_get_device(&g, 0x50U, &h) == GUARDIAN_OK && h.state == GUARDIAN_DEVICE_DEGRADED);
    CHECK(!guardian_device_retry_due(&h, 2U));
    CHECK(guardian_device_retry_due(&h, 4U));
    return true;
}

int main(void)
{
    unsigned passed = 0U;
    const struct { const char *name; bool (*fn)(void); } tests[] = {
        {"init validation", test_init_validation},
        {"line diagnosis", test_line_diagnosis},
        {"device health and cooldown", test_device_health_and_cooldown},
        {"successful bounded recovery", test_successful_bounded_recovery},
        {"refuse recovery while SCL low", test_refuse_clocking_when_scl_low},
        {"failure paths and events", test_failure_paths_and_events},
        {"counter wrap cooldown", test_counter_wrap_retry}
    };
    size_t i;
    for (i = 0U; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (!tests[i].fn()) return EXIT_FAILURE;
        (void)printf("PASS: %s\n", tests[i].name);
        ++passed;
    }
    (void)printf("%u/%u tests passed\n", passed,
                 (unsigned)(sizeof(tests) / sizeof(tests[0])));
    return EXIT_SUCCESS;
}

#include "guardian/guardian.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    bool sda_high;
    bool scl_high;
    unsigned pulses;
} fake_bus_t;

static bool sample(void *ctx, bool *sda, bool *scl)
{
    fake_bus_t *b = (fake_bus_t *)ctx;
    *sda = b->sda_high;
    *scl = b->scl_high;
    return true;
}
static bool enter_gpio(void *ctx) { (void)ctx; return true; }
static bool drive_scl_low(void *ctx) { ((fake_bus_t *)ctx)->scl_high = false; return true; }
static bool release_scl(void *ctx)
{
    fake_bus_t *b = (fake_bus_t *)ctx;
    b->scl_high = true;
    if (!b->sda_high && ++b->pulses >= 4U) b->sda_high = true;
    return true;
}
static bool drive_sda_low(void *ctx) { ((fake_bus_t *)ctx)->sda_high = false; return true; }
static bool release_sda(void *ctx) { ((fake_bus_t *)ctx)->sda_high = true; return true; }
static void delay_us(void *ctx, uint32_t us) { (void)ctx; (void)us; }
static bool restore(void *ctx) { (void)ctx; return true; }

int main(void)
{
    guardian_t guardian;
    guardian_bus_state_t bus_state;
    guardian_device_health_t health;
    guardian_platform_t platform;
    guardian_device_config_t device = { 0x68U, 2U, 4U, 100U };
    fake_bus_t bus = { true, true, 0U };

    memset(&platform, 0, sizeof(platform));
    platform.sample_lines = sample;
    platform.enter_gpio_open_drain = enter_gpio;
    platform.drive_scl_low = drive_scl_low;
    platform.release_scl = release_scl;
    platform.drive_sda_low = drive_sda_low;
    platform.release_sda = release_sda;
    platform.delay_us = delay_us;
    platform.restore_i2c_peripheral = restore;
    platform.context = &bus;

    if (guardian_init(&guardian, &platform) != GUARDIAN_OK ||
        guardian_register_device(&guardian, &device) != GUARDIAN_OK) {
        (void)fprintf(stderr, "Guardian initialization failed\n");
        return 1;
    }

    (void)printf("I2C Guardian fault-injection demo\n");
    (void)guardian_diagnose_bus(&guardian, &bus_state);
    (void)printf("Initial bus: %s\n", guardian_bus_state_name(bus_state));

    /* Simulate SDA held low by a peripheral until four SCL pulses. */
    bus.sda_high = false;
    (void)guardian_diagnose_bus(&guardian, &bus_state);
    (void)printf("Injected fault: %s\n", guardian_bus_state_name(bus_state));

    if (guardian_recover_bus(&guardian, 42U, &bus_state) == GUARDIAN_OK) {
        (void)printf("Recovery: SUCCESS, pulses=%u, final=%s\n",
                     bus.pulses, guardian_bus_state_name(bus_state));
    } else {
        (void)printf("Recovery: FAILED, final=%s\n",
                     guardian_bus_state_name(bus_state));
    }

    (void)guardian_record_transaction(&guardian, 0x68U,
                                      GUARDIAN_LAST_TIMEOUT, 100U);
    (void)guardian_record_transaction(&guardian, 0x68U,
                                      GUARDIAN_LAST_TIMEOUT, 101U);
    (void)guardian_get_device(&guardian, 0x68U, &health);
    (void)printf("Device 0x%02X state after two failures: %s\n",
                 health.address_7bit, guardian_device_state_name(health.state));
    (void)printf("Latched event mask: 0x%02lX\n",
                 (unsigned long)guardian_get_events(&guardian));
    return 0;
}

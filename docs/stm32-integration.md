# STM32 integration checklist

This guide defines the adapter responsibilities. It is not a drop-in HAL driver: exact implementation depends on STM32 family, Cube HAL version, bus instance, pin mapping, RTOS setup, and electrical design.

## Platform callbacks

Implement the callbacks in `guardian_platform_t`:

- `sample_lines`: read SDA/SCL with GPIO input sampling and return false on invalid configuration.
- `enter_gpio_open_drain`: first quiesce the I²C peripheral and confirm no DMA/interrupt-driven transfer is in progress; configure the SCL/SDA pins as open-drain GPIO without driving either line high.
- `drive_scl_low` / `release_scl`: pull clock low or release it. “Release” must mean high-impedance/open-drain released, not actively driven high.
- `drive_sda_low` / `release_sda`: equivalent open-drain behavior for data.
- `delay_us`: provide calibrated timing appropriate for the target and bus speed. Do not assume a loop count is portable across clock rates or optimization levels.
- `restore_i2c_peripheral`: restore alternate-function pin configuration and reset/reinitialize the HAL I²C state as appropriate for the specific family. Return false if restoration failed.

The callbacks should be short and bounded. Do not call blocking high-level HAL transactions from inside the callbacks.

## Integration sequence

1. Configure bus pull-ups and validate normal bus timing before enabling auto-recovery.
2. Make one application component the bus owner. For FreeRTOS, acquire the same mutex that protects normal transactions; do not attempt recovery from an interrupt handler.
3. On a HAL timeout/bus error, stop scheduling new transfers and wait for the active transaction to be safely aborted or completed.
4. If safe, sample line levels and call `guardian_recover_bus`.
5. Inspect its return code and final line state; do not silently ignore `GUARDIAN_EUNSAFE`.
6. If the bus becomes idle, clear relevant HAL error flags using the family-specific procedure and reinitialize peripherals that may have lost protocol state.
7. Record per-device transaction outcomes and export the event bitmask through the product's logging mechanism.
8. Resume traffic only after the HAL instance and target devices are in known states.

## Electrical / safety notes

- I²C lines use open-drain signaling and pull-ups. Never configure SCL/SDA as push-pull outputs for this recovery method.
- Check voltage domains, pull-up rail, sink current, rise time, and pin alternate-function details against the board design.
- If SCL stays low, a target may be clock-stretching or the line may be shorted. This core declines clock-pulse recovery in that state.
- GPIO clock pulsing cannot repair a short, missing peripheral power, damaged pin, or a target that never releases SCL.
- Some STM32 families have peripheral-specific BUSY clearing or reset procedures. Use the relevant reference manual and HAL implementation rather than copying register sequences from another family.

## Deliverables before claiming hardware support

- [ ] Pin and peripheral configuration for a named board/MCU.
- [ ] Compile-tested CubeIDE project or a target build preset.
- [ ] Integration code with timeout, abort, and HAL state restoration paths.
- [ ] Logic-analyzer capture of ordinary transfer and recovery waveform.
- [ ] Tests with SDA held low, SCL held low, NACK, bus timeout, and device removal.
- [ ] Confirmation that the application cannot race a normal transfer against recovery.

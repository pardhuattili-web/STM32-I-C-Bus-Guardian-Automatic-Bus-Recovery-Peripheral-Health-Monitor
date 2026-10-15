# Architecture

## Design goals

The library is a small, allocation-free C11 policy core. Platform-specific GPIO/HAL behavior is injected with callbacks so the state logic can be built and fault-tested on a host without claiming that simulated pin behavior proves an STM32 target works.

## Data model

- `guardian_t`: platform callbacks, fixed-capacity device registry, event flags, and recovery counters.
- `guardian_device_health_t`: 7-bit address, consecutive failures, totals, last result/tick, health state, and retry scheduling.
- `guardian_platform_t`: sampling and bounded control of SCL/SDA, small timing delays, and peripheral restoration.

No heap allocation, hidden background task, or implicit reset is used.

## Transaction health state

Each completed transaction is recorded by `guardian_record_transaction`.

- Success increments the success count, clears consecutive failures, and returns the record to `HEALTHY`.
- Failure increments the failure count with saturation for consecutive failures.
- At the configured failure threshold the device becomes `DEGRADED`.
- At the offline threshold it becomes `OFFLINE`.
- Events are bit flags latched until application code explicitly clears them.
- Cooldown timing uses unsigned subtraction, so a monotonic 32-bit tick counter may wrap as long as a cooldown is less than half the counter range.

This is bookkeeping, not an I²C address scan. Register only known devices to avoid unnecessary bus traffic.

## Bus diagnosis

The adapter samples SDA and SCL:
- both high: `IDLE`
- SDA low / SCL high: `SDA_LOW`
- SDA high / SCL low: `SCL_LOW`
- both low: `SDA_AND_SCL_LOW`
- callback failure: `UNKNOWN`

The model assumes bus-idle observations with compatible pull-ups. A transient sample during an active transfer is not sufficient to diagnose a stuck bus; the caller must quiesce transactions first.

## Recovery state flow

1. Sample and classify the bus.
2. Return immediately if the bus is idle.
3. Reject recovery when SCL is low; the target may be stretching the clock or the line may be electrically held low.
4. Enter GPIO open-drain mode under exclusive bus ownership.
5. Toggle SCL low/released with bounded delays, checking SDA after each released pulse, for no more than the configured limit (at most nine pulses).
6. Once SDA releases, form STOP by pulling SDA low while SCL is low, releasing SCL, then releasing SDA.
7. Restore the I²C peripheral even when a pulse operation fails, and sample final line state.
8. Latch success/failure events and increment counters.

The application is responsible for serializing access to the bus; `recovery_in_progress` is a defensive reentrancy check, not a cross-thread mutex.

## Important implementation limits

- A platform callback must return within a bounded period. A blocking callback can defeat the recovery bound.
- The core does not itself abort HAL/DMA transactions, reset an I²C peripheral, or reinitialize devices.
- Recovery is permitted for the SDA-low case only; an SCL-low line is reported as unsafe to pulse.
- A STOP/recovery result reports line state, not whether every device's internal protocol state is known.
- Recovery statistics count core attempts; the board adapter should separately log GPIO errors, HAL error flags, and actual pulse timing when needed.

## Extension points

A concrete integration can add:
- a board-specific STM32 HAL adapter and compile target;
- FreeRTOS mutex/task ownership and timeout coordination;
- per-device reinitialization callbacks;
- structured event logging with timestamps;
- a board test using deliberate SDA hold-low injection.

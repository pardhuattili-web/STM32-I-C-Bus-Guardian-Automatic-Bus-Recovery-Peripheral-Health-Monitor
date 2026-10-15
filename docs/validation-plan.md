# Validation plan

## Host checks available now

Run from repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGUARDIAN_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/guardian_sim
```

The current test suite covers API validation, line classification, per-device thresholds, latched events, bounded SDA-low recovery, safe refusal when SCL is low, callback failures, and wrapping tick arithmetic.

## Hardware bench matrix

| Case | Injection / setup | Expected behavior | Evidence |
|---|---|---|---|
| Normal idle | Both lines released | Diagnose idle; no GPIO recovery | SDA/SCL capture |
| Target NACK | Address a disconnected target | Record transaction failure; apply threshold policy | UART/log output |
| SDA stuck low | Hold SDA low with safe test fixture or target fault mode | Up to configured clock pulses, STOP after SDA releases, return bus to AF mode | Logic analyzer pulse count and STOP |
| SDA remains low | Force SDA low beyond pulse limit | Bounded failure; restore adapter state; report non-idle final bus | Log + waveform |
| SCL held low | Hold SCL low | Return unsafe without attempting clock pulses | No extra SCL pulses |
| Both lines low | Controlled safe fault injection | Report unsafe state; no recovery pulses | Waveform |
| HAL timeout | Induce transfer timeout | Application quiesces bus before guardian call | HAL state and event log |
| Callback error | Test adapter reports failure | Bounded failure path and restore attempt | Fault log |
| Device recovery | Bus released, device requires re-init | Application reinitializes the affected target | Functional readback |
| Repeated failures | Repeat NACK/timeout beyond thresholds | HEALTHY -> DEGRADED -> OFFLINE | Event log |

## Acceptance criteria

- Host build is warning-clean with warnings treated as errors.
- All deterministic tests pass in Release builds.
- Every recovery path has finite attempt/pulse bounds and restores peripheral mode when GPIO mode was entered.
- An SCL-low bus never triggers recovery clocks.
- No recovery runs while a normal transaction, DMA transfer, or another bus owner is active.
- Hardware support is claimed only after the named target builds and the waveform/fault matrix is recorded.

## Report template

Record MCU/board, Cube HAL version, compiler version, I²C timing, pull-up values, injected fault, configured pulse/retry bounds, recovery result, observed pulse count, restoration result, and link to the capture. Mark each result as **host simulation** or **physical hardware**.

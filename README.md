# STM32 I²C Bus Guardian
### Automatic bus recovery and peripheral health monitoring

A portable Embedded C diagnostic core for detecting stuck I²C lines, tracking peripheral health, and coordinating bounded recovery attempts without rebooting the entire application.

> **Project status:** host-buildable reference implementation with a simulator and unit tests. STM32 HAL callbacks and validation on a physical target are integration work; do not treat the host simulator as hardware validation.

## What it demonstrates

- **Line-state diagnosis:** distinguish a healthy idle bus from SDA-stuck-low, SCL-stuck-low, and both-lines-low conditions.
- **Bounded recovery policy:** request GPIO clock pulses and a STOP condition through a platform interface; cap pulse count and retry attempts.
- **Peripheral health registry:** identify devices by 7-bit address, track successes/failures, last result, and consecutive failures.
- **Escalation without a system reboot:** mark a device degraded/offline after configurable failures and publish diagnostic events.
- **Time-safe retry policy:** caller supplies monotonic ticks; retry intervals use wrap-safe unsigned subtraction.
- **Portable, allocation-free core:** no STM32 HAL dependency in the policy layer; hardware actions are injected through callbacks.
- **Host validation:** CMake build, test executable, fault-injection simulator, and GitHub Actions CI.

## Repository map

```text
include/guardian/guardian.h   Public API, states, result codes, platform callbacks
src/guardian.c                Bus diagnosis, peripheral tracking, bounded recovery
simulator/main.c              Fake platform and injected stuck-line scenarios
tests/test_guardian.c         Deterministic tests, including counter wrap
docs/architecture.md          State flow, contracts, limitations
docs/stm32-integration.md     STM32 HAL/GPIO integration checklist
docs/validation-plan.md        Bench test matrix and acceptance criteria
CMakeLists.txt                Host build and CTest
.github/workflows/ci.yml      Warning-clean build, tests, simulator
```

## Build and run (host)

Requirements: CMake 3.16+, a C11 compiler, and CTest.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/guardian_sim
```

Expected test result: all unit tests pass. The simulator prints a healthy-bus check, a stuck-SDA diagnosis, and a bounded recovery attempt with the fake platform returning the bus to idle.

## Quick integration model

1. Initialize the guardian with a platform adapter implementing line sampling, GPIO-mode transition, SCL pulse, STOP generation, and restoration to peripheral mode.
2. Register each known peripheral with its 7-bit address and failure threshold.
3. After a transaction failure, call the diagnostic API with the current monotonic tick.
4. Execute recovery only when the platform confirms the bus is safe to manipulate and the system has exclusive bus ownership.
5. Reinitialize affected devices after successful recovery; record and escalate persistent failures rather than resetting the whole MCU automatically.

See [STM32 integration notes](docs/stm32-integration.md) and [validation plan](docs/validation-plan.md).

## Recovery safety contract

- **Never pulse a bus concurrently with an active I²C peripheral/DMA transaction.** The application must quiesce the controller and ensure exclusive ownership before recovery.
- Recovery callbacks must be bounded and must not block indefinitely.
- Open-drain signaling and pull-up sizing must match the board's electrical design.
- A clock-pulse recovery can help when a target holds SDA low; it cannot fix a short, a target holding SCL low indefinitely, missing power, or damaged hardware.
- The portable core requests actions; the board adapter is responsible for honoring electrical timing, interrupt/RTOS constraints, and STM32 HAL state cleanup.
- A successful bus recovery does not guarantee that a peripheral's internal state has been reset. Reinitialize or power-cycle the peripheral only according to its own datasheet and board design.

## Scope and limitations

This repository provides the policy engine, deterministic host tests, simulator, build setup, and integration guidance. It does **not** claim to include a compile-ready STM32CubeIDE project, a specific STM32 HAL adapter, oscilloscope captures, or physical-board validation. Those depend on the exact MCU family, I²C instance, board wiring, RTOS policy, and peripheral set.

## Suggested demo

Run `guardian_sim` and explain how line sampling identifies a fault, how retries are bounded, and how a recovery result affects the device health record. For a hardware demo, log SDA/SCL before and after recovery and capture SCL pulse count and STOP generation on a logic analyzer.

## License

MIT — see [LICENSE](LICENSE).

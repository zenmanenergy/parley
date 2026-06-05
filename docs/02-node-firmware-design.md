# Node Firmware Design

This document describes how individual ESP32 nodes are structured at the firmware level. It covers the universal template that every node runs, the flash partition layout, the boot logic, and the plugin interface that allows per-peripheral code to integrate cleanly.

Read [01-system-architecture.md](01-system-architecture.md) first for context on how nodes fit into the larger Parley system.

## The Universal Template

Every node in the system runs the same firmware scaffolding. The only difference between an IMU node, a GPS node, and a motor node is the application-specific code for that peripheral. Everything else — WiFi connectivity, MQTT client, OTA updates, watchdog, factory fallback, heartbeat, message protocol — is shared.

This is the central design decision at the node level. Its consequences:

- **Shared bugs are fixed once.** A WiFi reconnection issue gets debugged in the template, and every node benefits from the fix.
- **New nodes are easy to add.** The integration work is writing application logic, not rebuilding infrastructure.
- **Behavior is consistent.** Every node recovers the same way, reports health the same way, accepts updates the same way.
- **AI agents can reason about nodes uniformly.** When generating firmware for a new peripheral, the agent is filling in a known shape, not designing from scratch.

The template should be written carefully and treated as the foundation of the system. It is the most important code in the project, even though it does nothing visible — it is what makes everything else possible.

## Flash Partition Layout

The system targets ESP32 modules with at least 16 MB of flash. ESP32-S3 modules in the N16R8 configuration (16 MB flash, 8 MB PSRAM) are the recommended choice. The partition layout:

| Partition | Size | Purpose |
|-----------|------|---------|
| Bootloader | ~64 KB | ESP32 second-stage bootloader; never modified after initial flash |
| Partition table | ~4 KB | Maps the rest of the flash; never modified after initial flash |
| NVS | 24 KB | Non-volatile storage: WiFi credentials, node ID, config, boot counter |
| Factory partition | 2 MB | Minimal recovery firmware; never updated after initial flash |
| OTA app slot A | 4 MB | Application firmware, slot A |
| OTA app slot B | 4 MB | Application firmware, slot B |
| LittleFS | ~5 MB | Local filesystem for logs, config files, persistent calibration data |

A few notes on the choices.

**Why 16 MB modules instead of 4 MB.** The default ESP32 modules ship with 4 MB of flash, which forces uncomfortable compromises: smaller A/B slots, no factory partition, no filesystem. The cost difference for 16 MB modules is small in single-quantity purchases and the architectural benefit is large. The partition layout depends on this — changing partition layout later requires USB reflashing every existing board, which defeats the wireless-only operational goal.

**Why a factory partition at all.** The factory partition is a minimal, never-changed firmware that exists solely to recover the node when both A and B slots are broken. Its existence is what makes the system fully recoverable over WiFi. See [03-recovery-and-resilience.md](03-recovery-and-resilience.md) for the full rationale.

**Why LittleFS instead of SPIFFS.** SPIFFS is the older default but is no longer recommended by Espressif. LittleFS is more robust against power-loss corruption, supports proper directories, and has better wear leveling.

**The NVS partition size.** 24 KB is larger than the default. NVS holds WiFi credentials, MQTT broker information, the node's assigned ID and capabilities, calibration values, the boot counter for recovery logic, and any per-node persistent state. 24 KB provides comfortable headroom; smaller sizes become tight as features accumulate.

## Three Firmware Roles

A node has three distinct firmware images it might run, each with different responsibilities and lifecycles.

### Application Firmware (slots A and B)

The application firmware is what the node runs during normal operation. It contains the universal template plus the application logic for the specific peripheral on this node. This firmware is updated frequently — every time peripheral logic changes, a bug is fixed, or a feature is added.

A and B are functionally identical at any point in time, but they are updated alternately. New firmware always goes to the inactive slot. The active slot is preserved as a rollback target until the new firmware proves itself.

### Factory Firmware

A minimal firmware that runs only when the node has fallen back to recovery mode. Its responsibilities are deliberately narrow:

- Connect to the gateway's WiFi AP using credentials stored in NVS.
- Connect to MQTT.
- Announce itself on `system/discovery` with status indicating factory mode.
- Subscribe to its OTA topic and accept new application firmware to slot A.
- Feed the watchdog.

That is the entire program. No sensor reading, no actuator control, no complex logic. The smaller and simpler this firmware is, the less likely it is to fail. After initial provisioning, this firmware is never updated. It should be treated as ROM.

If a bug is later discovered in factory firmware, the recovery path is to USB-flash the affected boards. Updating the factory partition over the air would destroy the safety guarantees that justify having a factory partition in the first place.

### Bootloader

The ESP32 second-stage bootloader is the lowest software layer. It reads the partition table, decides which app slot to boot based on OTA state, and validates basic image integrity. It is set once at initial flashing and not modified under normal operation.

The bootloader supports A/B switching natively through Espressif's OTA APIs. Application firmware uses these APIs to mark itself valid (after passing validation) or implicitly trigger a rollback (by not marking itself valid before the rollback timer expires).

## Boot Logic

When a node powers on, the following sequence executes:

1. **Bootloader runs.** Reads the partition table, checks OTA state to determine which app slot is active, validates the image, jumps to it.

2. **Application firmware (or factory firmware) starts.** The template's early-boot code runs.

3. **Boot counter check.** The template reads a boot counter from NVS. If it exceeds the threshold (default: 5), the template sets the boot partition to factory and reboots. This is the autonomous fallback to factory mode when both A and B slots are misbehaving.

4. **Reset reason check.** The template calls `esp_reset_reason()` to determine why the chip restarted. If the reset was due to a brownout or external reset (not a software fault), the boot counter is not incremented — these are not firmware failures. If it was due to a watchdog timeout, software panic, or stack overflow, the counter increments.

5. **Increment boot counter.** The counter is bumped (subject to the reset-reason filter above) and written back to NVS.

6. **Initialize hardware and connectivity.** WiFi connects, MQTT connects, the application's `setup_peripheral()` runs.

7. **Start the validation timer.** The template starts a timer (default: 60 seconds). The firmware must mark itself valid within this window or it will be rolled back on the next reboot.

8. **Validation gate.** When the application reports itself healthy through a defined callback, the template marks the firmware as valid via `esp_ota_mark_app_valid_cancel_rollback()`.

9. **Reset boot counter.** After an additional stability period (default: 5 minutes of healthy operation), the boot counter is reset to zero. This prevents slow-burn failures from accumulating across many short successful runs.

10. **Normal operation begins.** The template's main loop runs: feed the watchdog, call `loop_peripheral()`, publish heartbeats, handle MQTT messages.

This sequence is the same on every node. The application code does not have to think about it — it implements the plugin interface and trusts the template to handle the rest.

## The Plugin Interface

The template exposes a small interface that application code implements. This is the contract between shared infrastructure and peripheral-specific code.

Four required functions:

```cpp
// Called once at startup, after WiFi and MQTT are connected.
// Initialize hardware, register MQTT topics, set up timers.
void setup_peripheral();

// Called repeatedly from the main loop.
// Read sensors, command actuators, publish data.
// Must be non-blocking — return quickly to let the template do its work.
void loop_peripheral();

// Called by the template to check whether the peripheral is healthy.
// Return true to allow the validation gate to mark firmware as valid.
// Return false if the peripheral did not initialize correctly.
bool peripheral_health_check();

// Called when an MQTT command arrives on this node's command topics.
// The template handles topic routing; this is the dispatch hook.
void peripheral_handle_command(const char* channel, const uint8_t* payload, size_t len);
```

Several optional functions the template calls if implemented:

```cpp
// Called when entering shutdown — chance to safely stop motors, close files, etc.
void peripheral_shutdown();

// Called periodically to publish status beyond the standard heartbeat.
void peripheral_publish_status();

// Called when the network connection is lost and regained.
void peripheral_on_disconnect();
void peripheral_on_reconnect();
```

The template provides utilities the application can use:

```cpp
// Publish data to a topic relative to this node's namespace.
void node_publish(const char* channel, const uint8_t* payload, size_t len);
void node_publish_string(const char* channel, const char* str);
void node_publish_json(const char* channel, const JsonDocument& doc);

// Subscribe to a command channel under this node's namespace.
void node_subscribe(const char* channel);

// Log to the system log topic (visible to the Pi for debugging in real time).
void node_log(LogLevel level, const char* fmt, ...);

// Log to local persistent storage (survives rollback, used for post-crash diagnosis).
// See 10-local-logging.md for full details on the logging subsystem.
void node_log_local(LogLevel level, const char* tag, const char* fmt, ...);

// Access NVS for peripheral-specific persistent state.
bool node_nvs_get(const char* key, void* buf, size_t* len);
bool node_nvs_set(const char* key, const void* buf, size_t len);
```

This interface is deliberately small. The goal is that most peripheral integrations only need to implement `setup_peripheral()`, `loop_peripheral()`, and `peripheral_handle_command()`. The optional functions exist for cases that need them.

## Functions Per Node

The default is one peripheral per node. This default exists because it simplifies reasoning, isolates failures, and keeps each node's firmware small enough for an AI agent to work with effectively during conversational development.

The default has real costs: more boards, more power wiring, higher BOM. Where consolidation is appropriate, it should be used.

Consolidation is appropriate when functions are inherently coupled. Examples:

- A motor and its dedicated encoder. The encoder feeds the motor's local control loop; they are tied by physics.
- A multi-axis sensor breakout (IMU with accelerometer, gyroscope, and magnetometer on a single chip). They share the same I2C bus, the same data lines, and the same operational context.
- A display and its dedicated input buttons, where the buttons exist to control the display.

Consolidation is not appropriate when functions are independent. A motor and an unrelated temperature sensor should not share a node — they have no operational relationship, and combining them creates failure interdependence with no benefit.

The general rule: consolidate when functions are coupled, separate when they are independent. The cost of getting this wrong in one direction is complexity; in the other direction it is hardware count. Both costs are real.

When consolidating, the application code still uses the plugin interface — it just initializes multiple peripherals in `setup_peripheral()` and handles them all in `loop_peripheral()`. The template does not need to know how many peripherals a node manages.

## Application Logic Patterns

Several patterns recur across peripheral types and are worth standardizing.

### Sensor Pattern

A sensor node reads from hardware and publishes data. Typical structure:

```cpp
void setup_peripheral() {
    sensor.begin(I2C_PIN_SDA, I2C_PIN_SCL);
    last_publish = millis();
}

void loop_peripheral() {
    if (millis() - last_publish >= PUBLISH_INTERVAL_MS) {
        SensorReading r = sensor.read();
        node_publish_json("data", r.toJson());
        last_publish = millis();
    }
}

bool peripheral_health_check() {
    return sensor.isConnected() && sensor.lastReadValid();
}
```

Key details: non-blocking reads, periodic publish on a timer, health check verifies hardware presence and recent valid data.

### Actuator Pattern

An actuator node receives commands and drives hardware:

```cpp
void setup_peripheral() {
    motor.begin(MOTOR_PIN_A, MOTOR_PIN_B);
    motor.stop();
    node_subscribe("cmd");
}

void peripheral_handle_command(const char* channel, const uint8_t* payload, size_t len) {
    if (strcmp(channel, "cmd") == 0) {
        MotorCommand cmd = parseMotorCommand(payload, len);
        motor.setSpeed(cmd.speed);
        node_publish_json("ack", cmd.toAckJson());
    }
}

bool peripheral_health_check() {
    return motor.isEnabled() && !motor.hasFault();
}
```

Key details: command-driven rather than polled, acknowledges commands by publishing back, health check verifies hardware is responsive.

### Closed-Loop Pattern

For peripherals that need local feedback control (motor with encoder, servo with position sensing), the loop runs at a higher rate than the publish rate:

```cpp
void loop_peripheral() {
    // Fast control loop (e.g., 1 kHz)
    if (micros() - last_control >= CONTROL_INTERVAL_US) {
        float position = encoder.read();
        float output = pid.compute(target_position, position);
        motor.setOutput(output);
        last_control = micros();
    }

    // Slower telemetry publish (e.g., 10 Hz)
    if (millis() - last_publish >= PUBLISH_INTERVAL_MS) {
        node_publish_json("data", makeStatusJson());
        last_publish = millis();
    }
}
```

Key details: control loop runs locally on the ESP32 at high rate; telemetry publishes at a much lower rate. The Pi never participates in the control loop — it only sets targets and observes status.

## What Lives Where

A useful guideline: anything that should be true for every node lives in the template. Anything specific to a peripheral lives in application code.

In the template:

- WiFi connection, reconnection, credential handling
- MQTT client, broker connection, topic subscription management
- OTA update reception and partition switching
- Watchdog setup and feeding
- Boot counter and recovery cascade
- Validation gate and rollback marking
- Heartbeat publishing
- System log publishing (real-time MQTT logging)
- Local log file management on LittleFS (persistent across rollbacks; see [10-local-logging.md](10-local-logging.md))
- NVS access utilities
- The main loop structure

In application code:

- Hardware initialization for the specific peripheral
- Reading sensors or driving actuators
- Per-peripheral message format definitions
- Per-peripheral command handling
- Per-peripheral validation criteria (returned via `peripheral_health_check()`)

When the line is unclear, default to keeping things in application code. Moving something to the template later is easy; pulling it back out is harder.

## Build and Flash Workflow

The project uses PlatformIO with multiple environments — one per node type — sharing a common library directory for the template:

```
project/
├── platformio.ini
├── lib/
│   └── node_template/      # Universal template, used by all environments
│       ├── node_template.h
│       ├── node_template.cpp
│       ├── ota.cpp
│       ├── recovery.cpp
│       └── ...
├── src/
│   ├── imu_node/
│   │   └── main.cpp        # Application code for IMU
│   ├── gps_node/
│   │   └── main.cpp        # Application code for GPS
│   ├── motor_node/
│   │   └── main.cpp        # Application code for motor
│   └── factory/
│       └── main.cpp        # Factory firmware
├── partitions.csv          # Custom partition table
└── ...
```

`platformio.ini` defines an environment per node type. Building a specific node:

```
pio run -e imu_node
```

Uploading to a specific board:

```
pio run -e imu_node -t upload --upload-port /dev/esp32-imu
```

`/dev/esp32-imu` is a symlink defined by udev rules that maps each board's USB serial number to a stable name. This is essential during initial provisioning — relying on `/dev/ttyUSB0` versus `/dev/ttyUSB1` causes confusion as boards are connected and disconnected.

Once nodes have factory firmware installed and have joined the system, OTA updates replace USB uploads as the normal path. USB is reserved for initial provisioning and emergency recovery.

In the Parley operational model, compilation happens on the Pi (or on a separate build host); the resulting binaries are then distributed to nodes over the WiFi network via the gateway. The AI agent triggers builds and pushes as part of the conversational workflow.

## Versioning

Every firmware build embeds a version string. This appears in heartbeat messages, log output, and is recorded when the Pi pushes updates. The version combines:

- A semantic version number bumped manually in the source.
- The git commit hash of the build.
- The build timestamp.

Example: `1.4.2-7c3a4b1-2026-04-15T10:23:00Z`

Knowing exactly which firmware is running on which node is essential during debugging. Task summaries reference firmware versions explicitly. The git hash means firmware can be rebuilt from the repository at any point, which matters for reproducibility.

## Testing

Per-node unit testing on an ESP32 is awkward but worthwhile for the template. Application code is harder to test off-hardware because it interacts with physical peripherals.

A reasonable approach:

- **Template unit tests run on host.** ESP32 APIs are mocked. The recovery logic, validation gate, and message routing are tested. PlatformIO's test framework supports this.
- **Application integration tests run on hardware.** With the actual peripheral connected, a defined sequence of operations is executed and the expected MQTT messages are verified.
- **System tests run with the full robot.** Multi-node scenarios. These are mostly manual but should be documented as task summaries when run.

The investment in tests pays off most for the template, because template bugs affect every node. Application code is small enough that careful review and live testing usually suffice.

## Stability of the Plugin Interface

When an AI agent generates application code for a new peripheral (during the registration workflow described in [05-registration-workflow.md](05-registration-workflow.md)), the generated code follows the patterns above. The plugin interface is what the agent is filling in; the template is what the agent is plugging into.

This means the plugin interface should be stable. Changes to the interface are expensive — they invalidate every node's existing code and require the agent to be re-prompted with the new interface. The interface should be treated as a public API: extend additively, do not change existing signatures, and version the interface if breaking changes become unavoidable.

A clear, stable, well-documented plugin interface is also what makes generated code reliable. The narrower and better-defined the contract, the better the generations.

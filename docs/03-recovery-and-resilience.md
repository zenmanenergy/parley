# Recovery and Resilience

This document describes how Parley nodes recover from software failures without human intervention. The goal is straightforward: a node that crashes, hangs, or receives broken firmware should return to a working state on its own — no cables to plug in, no buttons to press.

Read [01-system-architecture.md](01-system-architecture.md) and [02-node-firmware-design.md](02-node-firmware-design.md) first for context on the overall system and how individual nodes are structured.

## What the System Tolerates

The recovery architecture handles these failure modes autonomously:

- A new firmware update has a bug that prevents normal operation.
- A node hangs in a way that requires a reboot.
- A node crashes repeatedly in a boot loop.
- Both A and B firmware slots are broken simultaneously.
- A node's WiFi connection drops temporarily.
- The Pi reboots while peripheral nodes are running.
- The gateway reboots briefly.
- A power glitch causes a brownout reboot.

The only failure modes that require human intervention are physical hardware failures (a sensor breaks, a wire comes loose, a board dies) and bugs in factory firmware itself, which is why factory firmware is treated as immutable after initial flashing.

## The Core Principle: Layered Simplicity

Recovery is built as a stack of layers. The crucial design rule: **every layer is simpler and more reliable than the layer above it.** This layering is what makes the whole stack robust.

From most complex to simplest:

1. **Application firmware** — does the actual work, complex, occasionally has bugs
2. **Watchdog** — kicks in when application firmware hangs
3. **A/B rollback** — reverts to previous firmware when new firmware fails to validate
4. **Boot counter and factory mode** — falls back to minimal firmware when both A and B are broken
5. **Factory firmware** — minimal "phone home" code, treated as immutable
6. **Bootloader** — set once at initial flashing, never changes

Each layer catches failures the layer above could not handle. The application is allowed to be complex and occasionally buggy because the lower layers are simple and reliable. The system's resilience comes from this layering, not from making any individual layer perfect.

## Layer 1: Hardware Watchdog

The ESP32 hardware watchdog timer is enabled at startup with a timeout of approximately 30 seconds. The application firmware "feeds" the watchdog from its main loop during normal operation.

If application firmware hangs — infinite loop, deadlock, blocking on a non-responsive peripheral — the watchdog stops being fed and triggers a chip reset. No software involvement; the chip simply restarts.

Several details matter for correctness:

**Feed location.** The watchdog is fed from the template's main loop, not from application code. This means an application bug in `loop_peripheral()` that hangs that function will trigger a watchdog reset. If application code were responsible for feeding directly, a buggy application could continue feeding from a hung state and never trigger recovery.

**Feed cadence.** Once per main loop iteration is appropriate for a 30-second timeout. The main loop should run many thousands of iterations per second under normal conditions.

**After watchdog reset.** The reset reason is captured via `esp_reset_reason()`. The boot counter increments on the next boot. If hangs recur, this eventually triggers fallback to factory mode.

**Distinguishing watchdog from brownout.** Both cause resets, but a brownout is not a firmware failure. The boot logic checks the reset reason and only increments the counter for software-attributable failures. Brownouts and external resets do not count.

## Layer 2: A/B Slot Rollback with Validation Gate

When new firmware is OTA-pushed to a node, it goes to the inactive slot. The bootloader is told to boot from this new slot on the next restart. The previous firmware remains untouched in the other slot as a rollback target.

The new firmware has a bounded window — default 60 seconds — to prove itself by calling `esp_ota_mark_app_valid_cancel_rollback()`. Until this happens, the bootloader's rollback timer is running. If the firmware reboots without marking itself valid, the bootloader reverts to the previous slot on the next boot.

### The Validation Gate

Validation is a defined check the firmware must pass before marking itself valid. The standard validation requires:

1. WiFi connected to the gateway's AP
2. MQTT connected to the broker
3. Successful publish of an "alive" message to `system/discovery`
4. The application's `peripheral_health_check()` returns true

All four conditions must hold. If they do not hold within the validation window, rollback fires automatically on the next reboot.

### Rules for Validation Criteria

The validation gate must be strict enough to catch real failures but not so strict that working firmware fails for unrelated reasons. This is a design tension worth addressing explicitly.

Bad validation criteria (too strict):

- "Sensor must be reading and producing valid data within 10 seconds." A temporarily disconnected sensor is a separate problem from broken firmware. Do not conflate them.
- "Must achieve a GPS fix within 30 seconds." GPS fix time depends on environment, not firmware quality.
- "Must successfully execute a motor calibration sequence." Calibration is an application-level concern, not a firmware health check.

Good validation criteria:

- Network is up.
- Broker is reachable.
- Basic message round-trip works.
- Hardware initialization did not error out (the I2C bus enumerated, the UART opened, etc.).

The principle: validate things the firmware itself controls. Do not validate against external dependencies that might fail for unrelated reasons.

The application's `peripheral_health_check()` should return true if hardware initialization succeeded — not if the peripheral is currently producing good data. A loose IMU sensor is not a firmware failure; it is a hardware problem that needs human attention.

### Rollback in Action

A typical rollback sequence:

1. The Pi pushes new firmware to slot B (slot A was active).
2. The Pi sends a command via the OTA topic to switch to B and reboot.
3. The node restarts; the bootloader switches to B.
4. New firmware boots and starts the 60-second validation timer.
5. Firmware fails to validate — perhaps a null pointer dereference 10 seconds in causes a panic and reboot.
6. The bootloader sees the firmware never marked itself valid and reverts to slot A.
7. Slot A firmware boots normally and marks itself valid (it is the previous, working version).
8. The node reports back over MQTT that a rollback occurred and its current version is X.
9. The Pi logs the failure for human and AI review.

The observable result is "the update failed and was rolled back," not "the node is bricked." This is the central win of A/B partitioning.

### Post-Rollback Diagnostics

The recovery cascade returns the node to a working state, but on its own it does not explain why the new firmware failed. The Pi sees "the node rolled back from version X to version X-1" but typically receives only fragmentary log lines that made it through MQTT before the crash.

Parley addresses this gap with local logging: each node writes a structured log file to LittleFS during operation. Because LittleFS lives in its own partition and is not touched by OTA updates, log entries written by one firmware version survive when a different firmware version takes over.

After a rollback, the recovered firmware reads the failed firmware's log file and publishes its contents to the Pi. The AI agent now has real diagnostic material to reason about — what the failed firmware was doing in the moments before its crash, which subsystem produced the last error, where logging stopped if the firmware silently hung.

See [10-local-logging.md](10-local-logging.md) for the full design of the logging subsystem and post-rollback reporting.

## Layer 3: Boot Counter and Factory Mode

When both A and B are broken — perhaps rollback itself failed, or validation logic had a bug, or the previous firmware has a latent issue that only manifests now — Layer 2 has hit its limit and Layer 3 takes over.

A counter in NVS tracks consecutive boots that did not reach a stable state. It increments on every boot (subject to the brownout filter described below) and resets to zero only after the firmware has been running healthily for a defined stability period (default: 5 minutes).

When the counter exceeds a threshold (default: 5), the early-boot code sets the boot partition to the factory partition and reboots:

```cpp
if (boot_counter > BOOT_COUNTER_THRESHOLD) {
    nvs_set_u32(handle, "boot_count", 0);  // reset the counter
    esp_ota_set_boot_partition(factory_partition);
    esp_restart();
}
```

The next boot lands in factory firmware, which is minimal and known to work. The node is now in recovery mode, ready to accept new application firmware.

### Why a Counter Instead of Immediate Fallback

Falling back to factory mode after a single failed boot would be too aggressive. Some failures are transient: a brownout, a watchdog reset due to network blocking, a single panic from a race condition that will not recur. These do not justify the disruption of falling back to factory mode. Five consecutive failures, by contrast, suggests something is genuinely wrong with both A and B slots.

The threshold of 5 is a starting point; tune based on observed behavior. If false fallbacks are common, raise it. If real fallbacks take too long to trigger, lower it.

### Why Reset After Stability, Not Immediately

The counter resets after 5 minutes of operation rather than immediately on validation because validation is an early gate and some failures only manifest later. Consider firmware that boots fine, validates, runs for 90 seconds, then crashes on a specific sensor input. Without the stability requirement, the counter would reset to zero after each successful validation, never accumulating, and the node would loop forever between "boot, validate, crash 90 seconds in" without ever escalating to factory mode.

The stability period catches slow-burn failures. After 5 minutes of healthy operation, the firmware is reasonably likely to actually be working.

## Layer 4: Factory Firmware

Factory firmware is the recovery anchor. When a node boots into factory mode, it does only the following:

1. Initialize WiFi using credentials in NVS.
2. Connect to the gateway's AP.
3. Connect to the MQTT broker.
4. Announce itself on `system/discovery` with status `factory_mode`.
5. Subscribe to its OTA topic.
6. When new firmware arrives, write it to slot A and reboot.
7. Feed the watchdog while waiting.

That is the entire program. No sensor reading, no actuator control, no business logic. The firmware is small and simple enough that bugs are extraordinarily unlikely.

### Factory Firmware Is Immutable

After initial provisioning, factory firmware is never updated. This rule is non-negotiable.

The reasoning: factory firmware is the safety net for application firmware failures. If factory firmware itself can fail, the safety net is gone. The only way to keep it reliable is to never change it.

This means that if a bug is later discovered in factory firmware, the only fix is USB-flashing every affected board. That is a real cost. Minimizing the risk:

- Keep factory firmware as small and simple as possible.
- Test it thoroughly during initial development.
- Review it carefully before deployment.
- Resist all temptation to add features over time.

The discipline of never updating factory firmware is what makes the system work. Breaking that discipline destroys the recovery guarantees.

### When the Node Cannot Reach the Gateway

Factory firmware needs network connectivity to do its job. If the gateway is down, factory firmware cannot recover. This is acceptable: the gateway is the system's recovery anchor (see [04-gateway-node.md](04-gateway-node.md)), and if it is down, the system as a whole is down.

When the gateway returns, nodes in factory mode reconnect automatically and announce themselves. The Pi, when it is up, sees the announcements and can push fresh firmware.

A node in factory mode waiting for the gateway is not bricked — it is patient. This is an important distinction. The system tolerates extended outages of the gateway or Pi without losing peripheral nodes permanently.

### WiFi Credential Failures

A subtle failure mode: if a firmware update somehow corrupts the NVS-stored WiFi credentials, factory firmware cannot connect either, since it reads the same NVS.

The mitigation: factory firmware has fallback credentials hardcoded at build time. The gateway's SSID and password are baked in as the fallback network. NVS-stored credentials are tried first; if connection fails after some attempts, factory falls back to the hardcoded values.

This means the gateway's SSID and password are effectively a system constant. Changing them requires reflashing every node's factory partition. They should not be changed casually.

## Layer 5: The Reset Reason Filter

Not every reset should count as a firmware failure for the boot counter. When the boot logic runs, it calls `esp_reset_reason()` to determine why the chip restarted:

| Reset reason | Counts toward boot counter? | Reasoning |
|--------------|------------------------------|-----------|
| `ESP_RST_POWERON` | No (counter resets to 0) | Fresh power-on, treat as clean start |
| `ESP_RST_EXT` | No | External reset (reset button or similar) |
| `ESP_RST_SW` | Yes | Software-requested reboot — treat as suspicious |
| `ESP_RST_PANIC` | Yes | Software panic — firmware failure |
| `ESP_RST_INT_WDT` | Yes | Interrupt watchdog — firmware misbehaved |
| `ESP_RST_TASK_WDT` | Yes | Task watchdog — firmware hung |
| `ESP_RST_WDT` | Yes | Other watchdog — firmware hung |
| `ESP_RST_DEEPSLEEP` | No | Intentional sleep wake-up |
| `ESP_RST_BROWNOUT` | No | Power problem, not firmware problem |
| `ESP_RST_SDIO` | No | Hardware-level reset, unrelated to firmware |

This filter prevents the recovery cascade from triggering on power glitches, manual resets during debugging, or normal sleep cycles.

## Heartbeats and Pi-Side Observability

The recovery cascade is autonomous on the node side, but the Pi observes it. Every node — in normal operation or factory mode — publishes a periodic heartbeat to `nodes/<node_id>/status`. The heartbeat includes:

- Current firmware version (or `factory` if in factory mode)
- Uptime
- Boot counter value
- Recent reset reason
- WiFi RSSI
- Free memory

The Pi watches these heartbeats. If a node's heartbeat goes silent for more than 2 minutes, the Pi flags it. If a node appears in factory mode, the Pi flags it — and may proactively push the last-known-good firmware. If a node's boot counter is climbing, the Pi flags it as flaky.

This gives humans and the AI agent visibility into recovery behavior. Most of the time, recovery happens silently. When patterns emerge — a node falling back to factory three times in a week — they are surfaced for investigation.

## What Is Not Recoverable Without USB

To be honest about the limits, several failure modes do require physical access:

**Hardware failures.** A blown chip, a fractured solder joint, a shorted regulator. Software cannot fix these.

**Bootloader corruption.** Extremely rare — the bootloader is in a protected flash region and OTA never writes to it. But not impossible. A corrupted bootloader requires USB to reflash.

**Partition table corruption.** Same as bootloader — never written by OTA, but USB recovery is needed if it somehow gets corrupted.

**Bugs in factory firmware.** As discussed above. The discipline of never updating factory firmware means a factory bug is permanent until the affected boards are USB-reflashed.

**WiFi credentials that match nothing.** If both the NVS-stored credentials and the hardcoded fallback fail to connect (the gateway is unreachable or fundamentally broken), recovery has no path home. This should not happen if the hardcoded fallback is the stable gateway SSID, but it is a theoretical limit.

These are narrow cases. The large majority of software failures — application firmware bugs, broken updates, hung loops — are recoverable autonomously.

## Recovery Flow Summary

A reference for the full failure-to-recovery flow:

1. New firmware is pushed to slot B.
2. Node reboots into slot B.
3. **Case 1: firmware crashes during validation window.** Watchdog or panic causes reboot. Bootloader sees no validation, rolls back to slot A. ✓ Recovered.
4. **Case 2: firmware boots but never validates.** 60 seconds elapse without `mark_app_valid_cancel_rollback()`. Bootloader rolls back on the next reboot. ✓ Recovered.
5. **Case 3: firmware validates, then crashes 2 minutes later.** Boot counter increments. After 5 such occurrences, fallback to factory. ✓ Recovered.
6. **Case 4: rollback target (slot A) is also broken.** Boot counter accumulates failures across both slots. Threshold reached, factory mode triggered. Pi observes factory-mode heartbeat and pushes a known-good firmware. ✓ Recovered.
7. **Case 5: factory firmware cannot connect to network.** Falls back to hardcoded credentials. Connects to gateway's AP. ✓ Recovered.
8. **Case 6: gateway is down.** Node waits in factory mode. When the gateway returns, recovery resumes. ✓ Recovered (eventually).

In all of these cases, no human plugs anything in. The system handles its own failures.

## Tuning Parameters

The system has several parameters that may need tuning based on observed behavior:

| Parameter | Default | Effect |
|-----------|---------|--------|
| Watchdog timeout | 30 seconds | Lower = faster hang detection, higher false-positive risk |
| Validation window | 60 seconds | Lower = faster failure detection, less time for legitimate slow startup |
| Boot counter threshold | 5 | Lower = faster factory fallback, more false fallbacks |
| Stability period | 5 minutes | Lower = counter resets faster, slow-burn failures harder to detect |
| Heartbeat silence alert | 2 minutes | Lower = more sensitive, more false alerts |
| Hardcoded credential fallback delay | 30 seconds | Lower = faster fallback, less time for transient WiFi issues |

These are starting points. Observed behavior in deployment will reveal which need adjustment.

## Why This Matters

A robot that requires physical intervention every time something goes wrong is exhausting to operate. Layering recovery eliminates the small, frequent, frustrating interventions and reserves human attention for problems that actually require it.

Robust recovery is also what makes the conversational extension model practical. Without it, every firmware iteration would risk bricking a node and forcing a hardware reset. With it, iteration is safe: push new firmware, see what happens, push a fix if it does not work, knowing that the worst case is "the node falls back to a previous version" rather than "the node requires physical access to fix."

Recovery is not a feature added on top of the architecture. It is the foundation that makes the rest of the workflow possible.

## Honest Limits

The recovery cascade is designed carefully but has not been stress-tested across the full space of possible failure modes. Several things to be aware of:

- **Some failures are not catchable at this level.** A firmware bug that successfully passes validation, runs healthily for 6 minutes (past the stability period that resets the boot counter), and then enters a degraded but not crash-inducing state may not trigger any of the recovery layers. Observability (heartbeats, anomaly detection on the Pi) catches such cases, but not autonomously.

- **The validation gate can be wrong.** Strict criteria cause false rollbacks of good firmware. Loose criteria miss real bugs. Tuning is ongoing.

- **Recovery depends on the gateway being up.** Multiple cascade layers ultimately funnel into "phone home over WiFi." If the gateway is broken, recovery has no path home until the gateway returns.

- **The cascade does not address application-level safety.** A motor that runs at full speed because the recovery cascade kept rebooting and the application kept setting that as its initial state is technically recovered but practically dangerous. Application code is responsible for safe initial states; the recovery cascade does not enforce them.

These are not flaws in the design — they are the limits of what autonomous software recovery can achieve. Combined with human and AI observability, they yield a system that handles routine failures gracefully without requiring constant attention.

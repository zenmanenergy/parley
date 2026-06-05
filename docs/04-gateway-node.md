# The Gateway Node

This document describes the dedicated infrastructure ESP32 that bridges the Raspberry Pi and the swarm of peripheral nodes. The gateway is not a peripheral — it is critical system infrastructure with its own design considerations.

Read [01-system-architecture.md](01-system-architecture.md) for context on where the gateway fits in the larger Parley system.

## What the Gateway Is

A single ESP32 (recommended: ESP32-S3 with 16 MB flash and native USB) physically connected to the Raspberry Pi via USB. The gateway is the Pi's wireless arm — the Pi has no WiFi of its own in the default deployment, and all communication with peripheral nodes flows through the gateway.

Conceptually, the Pi is the brain, peripheral nodes are the body, and the gateway is the spinal cord connecting them. The gateway-Pi pair is the indivisible core of the system.

## Responsibilities

The gateway has four well-defined responsibilities, and only these four.

### 1. WiFi Access Point

The gateway broadcasts an SSID that all peripheral nodes connect to. This is the swarm's network. It exists independently of any external WiFi infrastructure — no router, no hotspot, no internet connection required. The robot operates wherever it is taken, and the swarm continues to function.

The AP runs on the gateway's WiFi radio in AP mode. ESP32 supports up to 8 simultaneous client connections by default, sufficient for a swarm at the scale Parley targets (one robot with roughly 10 nodes). Systems significantly larger than this would benefit from an external AP, but the on-chip AP is adequate for typical deployments.

### 2. MQTT Bridge

The gateway relays MQTT traffic between peripheral nodes over WiFi and the Pi over USB. The MQTT broker itself runs on the Pi (typically Mosquitto). The gateway is a transport bridge, not a service host.

Bridging works in both directions. Peripheral nodes publish to topics; their messages flow over WiFi to the gateway, then over USB to the Pi's broker. Subscribers on the Pi or on other peripheral nodes receive these messages through the broker. Commands from the Pi flow the same way in reverse.

Two reasonable implementations exist:

**Option A: Two MQTT instances.** The gateway runs both an MQTT broker (for peripheral nodes) and a client connection to the Pi's broker, bridging traffic between them with topic filtering.

**Option B: Single broker with USB transport.** The Pi's Mosquitto is configured to accept connections over a USB-serial transport. The gateway acts as a forwarder, accepting WiFi MQTT connections and tunneling them to the Pi's broker over USB.

Option B is simpler and is the recommended approach. It keeps the Pi as the single source of truth for all MQTT state, and avoids the complexity of running a real broker on a memory-constrained ESP32.

### 3. Provisioning Anchor

When a new peripheral node joins the system for the first time, it has factory firmware but no node ID, no application firmware, and no system context. The gateway is the entry point for provisioning.

The flow:

- New node connects to the gateway's AP using factory firmware's hardcoded credentials.
- Node announces itself on `system/discovery` with a "needs provisioning" status, identifying itself by MAC address.
- The Pi sees the announcement and the AI agent begins the registration conversation (see [05-registration-workflow.md](05-registration-workflow.md)).
- During registration, the gateway facilitates the conversation: relaying messages, pushing firmware to the new node, watching for the node to come back online with its assigned identity.
- After provisioning, the new node has an assigned node ID, application firmware, and a defined role.

The gateway does not make decisions during provisioning. It is the conduit through which provisioning happens; the Pi and the AI agent do the actual work.

### 4. Diagnostic Console

The Pi has direct serial access to the gateway over USB. This means the Pi can read the gateway's serial output and send commands to it without going through MQTT or WiFi. This is the system's last-resort diagnostic channel: if all wireless is broken, the Pi-gateway link still works.

What gets exposed on this console:

- Boot messages (always visible during gateway startup)
- Error and warning logs from the gateway itself
- A simple text-based command interface for diagnostic queries: list connected clients, dump current network state, force reboot, etc.

The console is read-only for normal operation but supports a small set of commands for debugging. It is for troubleshooting, not application control.

## What the Gateway Is Not

Equally important to what the gateway does is what it must not become.

**Not an application logic host.** Application logic lives on peripheral nodes or on the Pi. The gateway has neither sensors nor actuators (other than a status LED) and does not make decisions about robot behavior.

**Not a permanent message store.** The gateway may buffer messages briefly during transient outages but does not persist messages to flash or maintain queues across reboots. The Pi's broker handles persistence.

**Not a place for miscellaneous features that do not fit elsewhere.** This is the temptation to resist hardest. Over time, every system accumulates features that lack an obvious home, and the gateway will look like a convenient catch-all. The gateway's reliability comes from being focused. Adding unrelated features introduces failure modes that affect the whole system.

**Not a fallback decision-maker.** If the Pi is down, the gateway does not attempt to keep the robot running by making its own decisions. It keeps the network up, accepts connections, buffers briefly if needed, and waits for the Pi to return. A gateway that takes over when the Pi is down would need to know what the Pi was trying to do, which requires application logic — exactly the line being drawn here.

## The USB Connection

The gateway is connected to the Pi via USB. After power itself, this is the most critical physical connection in the system.

### Why USB Instead of WiFi

The Pi-to-gateway link is the foundation everything else rests on, so it should be the most reliable channel in the system. USB is unaffected by RF interference, does not depend on the AP being up, and does not share airtime with peripheral nodes.

USB also keeps the Pi off the wireless network entirely, which simplifies its security posture and removes a class of failure modes where the Pi cannot reach the rest of the system due to its own WiFi issues.

### Native USB vs USB-Serial

ESP32-S3 and ESP32-S2 have native USB peripherals. They can present themselves as composite USB devices: a CDC-ACM serial port for diagnostics, plus a CDC-ECM or RNDIS network interface for MQTT traffic. This is the recommended setup.

With native USB networking, the gateway appears to the Pi as a network interface (such as `usb0`) with an IP address. MQTT traffic between the Pi's broker and the gateway is just TCP over this interface — no protocol translation, no special handling. The gateway is an ordinary MQTT client from the Pi's perspective, reached over USB rather than over a wireless link.

The original ESP32 (without S/S2/S3 suffix) does not have native USB and must use a USB-to-serial chip (CP2102, CH340, or similar). In that case, the connection is a serial port from the Pi's perspective, and MQTT traffic must be tunneled over the serial connection using a custom protocol or something like SLIP/PPP. This works but is more complex. ESP32-S3 is strongly preferred for the gateway role for this reason.

### Powering the Gateway

The Pi's USB port supplies 5V at up to 500 mA (USB 2.0) or 900 mA (USB 3.0). An ESP32-S3 running an AP draws roughly 200-300 mA peak, so this is comfortable.

The gateway being powered by the Pi means the gateway is available whenever the Pi is on and unavailable whenever the Pi is off. This is the desired behavior — the gateway should not outlive the Pi or vice versa. They boot together and shut down together.

If the Pi is powered separately from the rest of the robot (its own battery, a separate regulator), the gateway shares that fate. The power architecture should account for this.

### Cable Considerations

The robot environment subjects the USB cable to vibration and motion. Practical recommendations:

- Use a short cable (under 12 inches if possible) to minimize flex and signal degradation.
- Use USB-C connectors on both ends if the hardware supports it. USB-C is mechanically more robust than micro-USB.
- Mechanically secure both ends. A connector held in by friction alone is unreliable in a moving system.
- Mount the gateway directly adjacent to the Pi so the cable is essentially internal to the brain assembly.

A failed USB cable looks like the gateway disappearing from the Pi's `/dev`, which is easier to diagnose than mysterious WiFi issues.

### USB Power Cycling for Recovery

The Pi can power-cycle its USB ports through software (`uhubctl` on Linux, or GPIO-controlled USB power on some Pi models). This means the Pi can hard-reset the gateway if the gateway hangs in a way that its own watchdog cannot catch.

This adds another recovery layer — the Pi acts as a watchdog of the gateway, just as the gateway's watchdog watches its application firmware. Layers all the way down.

## Gateway Recovery

The gateway uses the same A/B/factory partition scheme as peripheral nodes (see [02-node-firmware-design.md](02-node-firmware-design.md) and [03-recovery-and-resilience.md](03-recovery-and-resilience.md)). Its application firmware can be updated via OTA. The same watchdog, validation gate, and boot counter logic applies.

One important difference: the gateway's recovery target is the Pi over USB, not WiFi to itself. When the gateway is in factory mode, its factory firmware needs to:

1. Establish USB connectivity to the Pi.
2. Wait for the Pi to push new application firmware.

There is no AP to connect to (the AP is exactly what is missing in factory mode), so the recovery channel must be USB. This is one of the reasons USB connectivity matters — it is not just for normal operation; it is also the gateway's recovery path.

The Pi can also flash the gateway via USB at the lowest level. If gateway factory firmware itself fails (the one truly unrecoverable case for peripheral nodes), the Pi can run `esptool.py` over the USB connection and reflash the gateway from scratch — bootloader, partition table, factory partition, everything. This eliminates the "factory firmware bug is permanent" concern that applies to peripheral nodes. The gateway has a deeper recovery channel that peripheral nodes lack.

This means the gateway's factory firmware can be slightly less paranoid than peripheral factory firmware. A bug in gateway factory firmware is recoverable without physical access because the Pi is right there with USB. This is a small relaxation but worth knowing about.

## The Gateway's Own Identity

Even though the gateway is infrastructure, it runs the same universal template as any other node (see [02-node-firmware-design.md](02-node-firmware-design.md)). It publishes heartbeats to `nodes/gateway/status`. It accepts OTA updates. Its `peripheral_health_check()` returns true when the AP is up, MQTT bridging is working, and USB is connected.

The application code for the gateway implements "be a gateway" — `setup_peripheral()` starts the AP, `loop_peripheral()` handles bridging, and so on. The gateway is conceptually a node with a specific role, just one whose role is system infrastructure rather than sensing or actuation.

Everything that is true for peripheral nodes (recovery, OTA, heartbeats, validation, local logging) is also true for the gateway. There is no special-case code for the gateway in the template. It is a node like any other; its difference is in what it does, not in how it is structured.

## What If the Gateway Goes Down

The gateway is a single point of failure for the swarm. If it is down, peripheral nodes cannot reach each other or the Pi.

**Brief gateway reboot.** Peripheral nodes' MQTT clients lose connection, attempt to reconnect, and retry until the gateway is back. Most MQTT clients handle this gracefully — they queue locally for a short window and republish when reconnected. A gateway reboot of under a minute should cause minimal disruption.

**Extended gateway outage.** Peripheral nodes eventually time out and may enter their own connection-failure recovery (which does not help if the gateway is the problem). They continue retrying. When the gateway returns, they reconnect.

**Gateway hardware failure.** This is the only failure that requires intervention. Replace the gateway, USB-flash it with the factory firmware, reconnect to the Pi, and the system resumes. The peripheral nodes do not need to be touched — they reconnect to the gateway's AP automatically once the AP is back.

The gateway being a single point of failure is acceptable because it is a small, well-understood, infrequent failure mode at this scale. Adding redundancy (two gateways with failover) introduces significant coordination complexity for marginal benefit. Redundancy is not appropriate unless the system grows substantially beyond Parley's target scale.

## Hardware Selection

The gateway warrants more careful hardware selection than peripheral nodes. Peripheral nodes can use inexpensive ESP32 boards because they are isolated and substitutable. The gateway is critical infrastructure and benefits from better hardware:

- **ESP32-S3 with 16 MB flash and 8 MB PSRAM.** The PSRAM helps with concurrent connections and message buffering. Native USB is essential.
- **USB-C, not micro-USB.** Mechanical robustness matters in this role.
- **A board with a stable, well-supported breakout.** Espressif's official DevKit boards or well-known commercial boards are good choices. Obscure clones should be avoided for the gateway.
- **Adequate antenna.** If the AP needs to reach peripheral nodes spread around the robot, antenna performance matters. Some boards have ceramic chip antennas (compact, lower range); some have PCB trace antennas (better range); some have U.FL connectors for external antennas (best range, more cost). The choice depends on the robot's physical layout and the placement of peripheral nodes.

Spending an extra few dollars on the gateway versus the cheapest possible board is worthwhile. It is the most important node in the system, and reliability there pays back across every interaction with every peripheral node.

## Status LED Convention

A visible status LED on the gateway provides glanceable system state. A standard convention:

- **Solid off** — Power off or extremely early boot
- **Fast blink (5 Hz)** — Booting, AP not yet up
- **Slow blink (1 Hz)** — AP up, USB to Pi not yet connected
- **Solid on** — Fully operational: AP up, Pi connected, MQTT bridging
- **Double-blink pattern** — Factory mode (recovery)
- **Rapid flash** — Error state, see logs

This allows a human to assess the gateway's state without opening a console or checking the Pi. For a robot being held or examined in hand, glanceable status is genuinely useful.

## Summary

The gateway is the system's spinal cord. It is:

- A WiFi access point for the swarm
- An MQTT bridge between WiFi and USB transports
- The provisioning entry point for new nodes
- The Pi's diagnostic console for the swarm
- USB-connected to the Pi for reliability and recovery

It is not:

- An application logic host
- A persistent message store
- A repository for miscellaneous features
- A fallback decision-maker

The gateway's critical importance demands discipline. It works because it is focused. Adding scope to the gateway is the most dangerous architectural mistake possible in this system.

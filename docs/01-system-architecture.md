# System Architecture

This document describes the structure of a Parley robot system. It is the orienting technical reference — the first document to read after the README.

## Core Properties

Parley is built around three properties that depend on each other:

**Distributed firmware.** Each function of the robot runs on its own ESP32 (or, where appropriate, a small group of related functions on the same ESP32). There is no monolithic firmware. The system grows by adding nodes, not by extending a central codebase.

**Autonomous recovery.** Individual nodes can recover from software failures without human intervention. A layered cascade of watchdog, A/B firmware rollback, and factory-mode fallback ensures that failed updates revert to known-good firmware automatically.

**Conversational integration.** New hardware joins the system through dialogue with an AI agent. The agent researches components, generates firmware, deploys it over the air, reads logs, and identifies issues. Humans direct the work and verify the agent's reasoning.

These properties are interdependent. Distributed firmware keeps node code small enough for the AI to reason about. Autonomous recovery makes wireless iteration safe enough for conversational pace. Conversational integration is what makes the distribution scalable for humans — without it, managing many nodes would be more burden than benefit.

## The Three Compute Layers

A Parley robot has three layers of compute, each with a defined role.

### The Raspberry Pi

The Pi is the operational hub of the running robot. Its responsibilities:

- Run the MQTT broker (Mosquitto) that coordinates all node communication
- Compile firmware from source files in the project repository
- Distribute compiled firmware binaries to the appropriate ESP32 targets over the air
- Aggregate and store logs and telemetry from across the swarm
- Host any robot-level coordination logic that ties multiple nodes together
- Provide diagnostic access via SSH for humans connecting from their devices

The Pi is the only place on the robot that runs a full operating system, and the only place with significant compute, memory, and storage.

The Pi does not need to be on the main WiFi network. It communicates with the rest of the system through the USB-connected gateway node. This keeps the Pi off the wireless attack surface, removes its dependence on external network infrastructure, and gives it a guaranteed-working communication channel to the gateway.

**The AI agent does not run on the Pi.** The agent runs on its provider's servers (Anthropic's, for the v1 implementation using Claude) and is accessed by humans from their own devices — laptops, phones — connecting to the Pi over a network when working on the robot.

This separation has practical consequences:

- The dev environment is independent of the running robot. The Pi can be unavailable without losing access to the AI agent; the dev environment can be unavailable without affecting the robot.
- Compilation can happen on the Pi or on a separate build host. The Pi can be a smaller model (a Pi Zero 2 W has adequate capacity for broker, coordination logic, and gateway interface) or a larger one (a Pi 4 or 5 provides headroom for compilation, vision processing, and future capabilities).
- The Pi's workload is modest. Mosquitto plus coordination logic is light. Watchdog-supervised systemd services handle restarts on the rare crash.

### The Gateway Node

A dedicated ESP32-S3 (with native USB and at least 16MB of flash) is physically connected to the Pi via USB. The gateway has four responsibilities:

- **WiFi access point.** Broadcasts an SSID that all peripheral nodes connect to. The swarm has its own network, independent of any external WiFi infrastructure.
- **MQTT bridge.** Relays MQTT traffic between peripheral nodes over WiFi and the Pi's broker over USB. The broker itself runs on the Pi; the gateway is a transport bridge.
- **Provisioning anchor.** New nodes connect to the gateway's AP first, where they are assigned a node ID and configuration before joining the main system.
- **Recovery anchor.** Nodes that fall back to factory firmware can always reach the gateway because the SSID and credentials are baked in to factory firmware.

The gateway is deliberately not a place for application logic. It is infrastructure. Anything beyond AP, bridge, provisioning, and diagnostics belongs on the Pi or on a peripheral node. Restraint here matters — adding features to the gateway erodes the reliability that justifies treating it as a fixed foundation.

The gateway is intentionally a single point of failure. Adding redundancy at this layer introduces coordination complexity that outweighs its benefit at the scale Parley targets. The Pi-plus-gateway pair should be treated as the indivisible core of the system.

### Peripheral Nodes

Every other ESP32 in the system is a peripheral node, each handling one function — a sensor, an actuator, a coordinator — or a small group of closely-related functions where consolidation is justified.

The default is one function per node. This default exists because it simplifies reasoning, isolates failures, and keeps each node's firmware small enough for an AI agent to work with effectively. The default has real costs: more boards, more power wiring, higher BOM. These costs are worth the benefits at the scale Parley targets.

Consolidation onto a single node is appropriate when functions are inherently coupled. A motor and its dedicated encoder belong on the same node — the encoder feeds the motor's local control loop, and they are already tied by physics. A display and its dedicated input buttons might reasonably share a node. The rule of thumb: consolidate when functions are coupled, separate when they are independent.

What to avoid: putting unrelated functions on the same node to save hardware cost. The complexity tax is real, and it grows nonlinearly as more functions interact.

All peripheral nodes run the same universal template (see [02-node-firmware-design.md](02-node-firmware-design.md)). The only thing that differs between nodes is the application logic for their specific peripheral.

## Communication

The system communicates over MQTT. Every node is an MQTT client. The Pi runs the broker. The gateway bridges between USB and WiFi transports, but the protocol is the same on both sides.

MQTT was chosen for this role because:

- It is mature, lightweight, and well-supported on ESP32.
- The publish/subscribe pattern fits the system naturally — sensors publish, actuators subscribe, the Pi orchestrates.
- It decouples nodes from each other. A node does not need to know who is listening to its data.
- It supports last-will-and-testament messages, useful for detecting node failures.
- It is widely understood by AI agents, which matters for the conversational extension model.

Topic structure follows a convention:

- `nodes/<node_id>/status` — heartbeat and health
- `nodes/<node_id>/data/<channel>` — published sensor data
- `nodes/<node_id>/cmd/<channel>` — commands to actuators
- `nodes/<node_id>/ota` — firmware update channel
- `system/discovery` — node announcements and registration
- `system/anomalies` — surfaced issues for human and AI review

Specific topic schemas are defined per-capability and live in the part library (see [05-registration-workflow.md](05-registration-workflow.md)).

### Optional CAN Bus for Motion Coordination

For specific cases where multiple actuators need millisecond-level synchronization with each other — typically the drive wheels of a differential drive robot, or coordinated joints of an arm — MQTT's variable latency is inadequate. These cases get a small targeted addition: a CAN bus wired directly between the nodes that need to coordinate.

CAN does not replace MQTT for these nodes. They remain full MQTT clients, accessible to the AI agent for registration, OTA, configuration, and telemetry. CAN is an additional peer-to-peer channel used only for the runtime coordination data that needs to flow faster than MQTT can deliver.

Most nodes have no CAN at all. The nodes that need it get a CAN transceiver (such as the SN65HVD230) wired to their standard ESP32, with a shared two-wire bus between them. See [09-can-bus-additions.md](09-can-bus-additions.md) for the full design.

The architecture remains MQTT-first. CAN is a refinement applied to specific node pairs, not a parallel infrastructure tier.

## Network Topology

```
[ Human's device (AI agent) ] <--network--> [ Pi ] <--USB--> [ Gateway ESP32 ] <--WiFi AP--> [ Peripheral ESP32s ]
                                                                                                     |
                                                                                  [ optional CAN bus between motion peers ]
```

The Pi has no wireless connection in the default deployment. All wireless traffic flows through the gateway. The gateway acts as both an access point (for peripheral nodes) and as a USB device (for the Pi) — typically a CDC-ECM or RNDIS network interface on the Pi side, allowing standard TCP/IP traffic.

Humans working on the robot connect from their own devices over whatever network reaches the Pi: ethernet, home WiFi, or a dedicated wireless link. The human's device is not on the robot's internal WiFi network (the gateway's AP) by default — that network is reserved for ESP32 peripherals.

Peripheral nodes are stations on the gateway's AP. They have no awareness that the Pi exists as a separate entity — from their perspective, the broker is just an IP address on their local network, which happens to be the gateway forwarding traffic.

This topology has several useful properties:

- **No external network dependency.** The robot operates without a router, hotspot, or internet connection.
- **Bounded radio environment.** The gateway controls who is on the network. No interference from unrelated devices.
- **Single trust boundary.** The gateway's AP is the system's network. Anything on it is part of the system.
- **Pi reboot survivability.** If the Pi reboots, the gateway stays up, the AP stays up, peripheral nodes stay connected. They queue messages until the Pi returns.

## Power Architecture

Power and communication are deliberately decoupled. After initial USB-flashing, peripheral nodes have only power wiring — no data cables. This is a design goal because data cables on a robot are a maintenance burden and a source of intermittent failures.

The gateway is powered by the Pi's USB port. The Pi is powered from the robot's main supply. Peripheral nodes are powered from the robot's main supply with appropriate regulation per board.

Boot ordering matters. The gateway must come up before peripheral nodes try to connect. Since the gateway is powered by the Pi and the Pi takes time to boot Linux, this happens naturally — by the time peripheral nodes are looking for the AP, the gateway is up and broadcasting.

For motion nodes with CAN transceivers, an additional two-wire CAN bus is added to the wiring. This is the one exception to the power-only-wiring goal, and it applies only to the specific nodes that need millisecond synchronization.

## Persistent State

Persistent state lives in several places, in increasing order of permanence:

1. **NVS on each ESP32.** Local config: WiFi credentials, node ID, MQTT broker address, calibration values. Survives reboots and firmware updates. Wiped only on factory reset.
2. **Files on the Pi.** Logs, recent telemetry, active task state, deployed firmware binaries. Survives Pi reboots.
3. **The Git repository (on humans' development devices and a remote backup).** The project itself: firmware source, the universal template, registered node configurations, the part library, task summaries, design documents. This is the system's long-term memory and the artifact that survives across hardware changes.
4. **A synced copy of the repository on the Pi (optional).** For robots that need to rebuild firmware autonomously without the dev environment present, a copy of the repo can be kept on the Pi.

The repository is the most important of these. If everything else is lost — every ESP32 wiped, the Pi's filesystem corrupted — the repository allows the system to be rebuilt. Standard discipline applies: regular commits, regular pushes to a backup remote, commit messages that capture decisions.

## Why These Choices

Several decisions in the architecture are non-obvious and have specific rationales.

**Why not run the MQTT broker on the gateway?** The gateway is resource-constrained (an ESP32 has under a megabyte of usable RAM) and lacks persistent storage for logging. The Pi has both. MQTT brokers benefit from being on capable hardware where they can log history and serve many clients efficiently. Bridging is the right division of labor.

**Why USB between Pi and gateway instead of WiFi?** Reliability and independence. USB does not fail because of RF interference, does not depend on the AP being up, does not share airtime with peripheral nodes. The Pi-gateway link is the most reliable connection in the system, because it is the foundation everything else rests on.

**Why distribute firmware across many microcontrollers?** Three reasons. First, failure isolation — a bug in one node's code does not affect other nodes. Second, AI tractability — each node's firmware is small enough to fit in an AI agent's working context, which makes conversational development practical. Third, easier substitution — swapping hardware means swapping one board, not editing a monolithic codebase.

**Why MQTT instead of a custom protocol or simpler scheme?** MQTT is good enough, well-understood, and widely supported. AI agents recognize it deeply, which matters for the conversational extension model. The cost of going custom is high; the benefit is small.

**Why ESP32-S3 specifically?** Built-in WiFi, native OTA support with A/B partitioning, mature tooling (PlatformIO and ESP-IDF), broad community, low cost, and enough headroom for both the universal template and application logic. Native USB on the S3 variant is essential for the gateway role. Alternative microcontrollers (STM32, Raspberry Pi Pico W) are viable for individual nodes but lack one or more of the properties that make the ESP32-S3 a clean fit across all roles.

## Scope and Limits

Parley is designed for a specific category of robot: custom-built systems with a meaningful amount of integration work, where hardware and behavior evolve through development. It is appropriate when significant time would otherwise be spent on the engineering tasks that conversational AI assistance can compress.

The architecture has known limits worth being explicit about:

- **Cross-node coordination is not generally real-time.** MQTT over WiFi has variable latency. Tight control loops (motor PID at 1 kHz) must run within a single node, not across nodes. The CAN bus addition supports millisecond coordination between specific peers, but only between those peers and only over that wired channel.
- **The MQTT broker is a single point of failure for swarm-wide coordination.** If the broker fails, nodes continue local operation but cannot coordinate. Recovery requires the broker process to restart.
- **WiFi performance can degrade in motor-noisy environments.** Brushed motors, switching converters, and high-current actuators generate broadband RF noise that can interfere with 2.4 GHz WiFi. Mitigations exist (antenna placement, shielding, CAN for the most affected nodes) but cannot eliminate the problem entirely.
- **The architecture is not a robotics framework in the sense ROS is.** It does not provide navigation, perception, simulation, or message-type infrastructure. It addresses the firmware-and-integration layer. Higher-level capabilities are added separately, on the Pi, possibly using ROS itself.

These limits describe the boundary of what Parley addresses. They are not flaws but the natural scope of a focused architecture.

## Document Set

After this document, the architecture is described in detail across:

- [02-node-firmware-design.md](02-node-firmware-design.md) — the universal template and partition layout
- [03-recovery-and-resilience.md](03-recovery-and-resilience.md) — the autonomous recovery cascade
- [04-gateway-node.md](04-gateway-node.md) — the gateway ESP32 in detail
- [05-registration-workflow.md](05-registration-workflow.md) — the conversational integration process
- [06-role-discovery.md](06-role-discovery.md) — emergent role assignment through observation
- [07-spatial-and-physical-model.md](07-spatial-and-physical-model.md) — physical layout representation
- [08-collaboration-workflow.md](08-collaboration-workflow.md) — the dashboard interface that unifies system visibility, AI conversation, and direct command execution
- [09-can-bus-additions.md](09-can-bus-additions.md) — targeted CAN coordination for motion-critical peers
- [10-local-logging.md](10-local-logging.md) — per-node logging that survives firmware rollback for post-failure diagnostics

For new readers: read this document, then [02-node-firmware-design.md](02-node-firmware-design.md) for the per-node design, then [03-recovery-and-resilience.md](03-recovery-and-resilience.md) for the recovery model. The remaining documents can be read in any order based on interest.

# CAN Bus as Targeted Addition

This document describes how CAN bus is used within a Parley system. CAN is not a fundamental architectural element. It is a targeted addition for specific synchronization problems that MQTT cannot solve.

Read [01-system-architecture.md](01-system-architecture.md) first for the broader system context. The architecture remains primarily MQTT-based; this document describes when and how to layer in CAN where MQTT timing is inadequate.

## The Mental Model

**MQTT is the system's nervous system.** Every node speaks MQTT. Sensors publish, actuators receive commands, telemetry flows to the Pi, configuration flows down, the AI agent reaches every node for registration and OTA. This is the system's primary communication layer and it covers most needs.

**CAN is direct nerve-to-nerve connections between specific muscles that must twitch together.** A small subset of nodes — typically a couple of motors that need millisecond-level coordination — get a CAN bus between them as an additional, peer-to-peer channel. This is in addition to their MQTT connection, not instead of it.

The key consequence: a node with CAN is still a first-class citizen of the conversational system. The AI agent can talk to it, configure it, push firmware, run discovery. CAN is purely a fast peer-to-peer channel for runtime coordination between specific peers. It does not replace MQTT for any of MQTT's normal jobs.

## When CAN Is Actually Needed

The diagnostic question: **does this robot have two or more actuators that need to move in coordinated time with each other?**

Cases where CAN genuinely matters:

- **Differential drive that needs to track straight.** Two wheels, two motors. To produce straight-line motion they must compensate for each other in near-real-time. Over MQTT, position updates arrive 20-100 ms late with jitter, and the wheels fight each other or oscillate. Over CAN, sub-millisecond updates let them stay tightly synchronized.
- **Multi-axis coordinated motion.** A pan-tilt mechanism, robot arm, gimbal — anything where multiple actuators must reach their setpoints simultaneously to trace a planned trajectory. CAN provides simultaneous arrival of synchronized setpoints.
- **Fast emergency stop across coordinated actuators.** When every motor in a coordinated group must stop within a few milliseconds, CAN's hardware-level priority arbitration provides a guaranteed-fast path.

Cases where CAN is not needed:

- **Single-motor systems.** The motor's control loop runs locally on its own ESP32. High-level commands tolerate WiFi latency fine.
- **Independent actuators.** Motor A drives a winch, Motor B drives a camera tilt, they never coordinate. Each is fine on MQTT alone.
- **Slow coordination.** "Motor A runs for 10 seconds, then Motor B starts" is slow enough that MQTT timing is irrelevant.
- **Any sensor.** Sensors rarely need to be synchronized to each other at millisecond precision.
- **Initial development.** Even for cases that might eventually need CAN, the MQTT-only version should be tried first.

## The Reality for Most Robots

For a typical small mobile robot, exactly one place requires CAN: keeping the drive wheels in sync. Everything else — GPS, IMU, cameras, environmental sensors, any non-coordinated actuators — works fine on MQTT alone.

That means CAN transceivers on exactly two nodes (the wheel motors). Two transceivers. A few feet of CAN wire between them.

The mental shift: CAN is not a separate tier of the architecture. It is a small, targeted addition between specific node pairs that have a measured synchronization problem. The architecture remains MQTT-first throughout.

## Hardware: One Board for Everything

All nodes in the system use the same ESP32 board model: **ESP32-S3 with 16 MB flash and 8 MB PSRAM (N16R8)**. This is the same board specified throughout the rest of the architecture documents. There is no separate "motion-capable" board model.

When a node needs CAN capability, an **SN65HVD230 CAN transceiver breakout board** is added (Waveshare makes a reliable one, sold in 2-packs) and wired to the ESP32:

- 3.3V from ESP32 to transceiver VCC
- GND to GND
- ESP32 GPIO5 to transceiver CTX (transmit data from ESP32 to transceiver)
- ESP32 GPIO4 to transceiver CRX (receive data from transceiver to ESP32)

These GPIO assignments match ESP-IDF defaults for the TWAI peripheral. They can be remapped if those pins are needed for other purposes on a specific node.

The transceiver's CANH and CANL lines connect to the shared CAN bus. The two nodes at the physical ends of the bus need 120-ohm termination resistors (the Waveshare board has a jumper for this); intermediate nodes should have termination disabled.

## Why This Approach Beats Dedicated CAN Boards

Some ESP32 boards on the market integrate a CAN transceiver onto the same PCB. They are more expensive (typically $20-30 versus $5-15 for standard boards), often ship with only 8 MB flash (not the 16 MB the partition layout assumes), and add a second board SKU to manage.

The approach of standard ESP32 plus add-on transceiver has real advantages:

- **Single SKU for all nodes.** No decision-tree at provisioning about which board to grab. The same ESP32-S3 N16R8 boards are bought in bulk.
- **16 MB flash everywhere.** No compromise on the A/B/factory/filesystem partition layout.
- **Lower per-unit cost.** The $2-5 transceiver is added only to nodes that need it.
- **Late commitment.** The decision to add CAN to a node does not have to be made at provisioning time. The transceiver can be added later when motion coordination requirements become clear.
- **Mounting flexibility.** The transceiver can be physically positioned near the CAN bus connector with short signal traces, while the ESP32 mounts wherever is convenient.

The honest tradeoff: four signals must be wired correctly per CAN-equipped node, and off-the-shelf transceiver breakouts typically do not include optocoupler isolation between the bus and the ESP32. In motor-noisy environments, lack of isolation can cause intermittent CAN errors. If this becomes a measured problem, isolated transceiver boards exist; starting without isolation and adding it only if symptoms appear is the recommended progression.

## Bus Topology and Physical Setup

CAN is a bus, not a star. All CAN-equipped nodes connect to the same shared pair of wires (CANH and CANL). The bus is daisy-chained — each node taps into the bus rather than running back to a central hub.

For robot-scale systems, the bus is short (a few meters at most) and runs at 1 Mbit/s comfortably. Specific requirements:

- **Twisted pair wire** for CANH and CANL. Reduces noise pickup. Twisted pair from general-purpose data cable is sufficient; CAN-specific cable is overkill at this scale.
- **120-ohm termination at both physical ends** of the bus. Intermediate nodes do not terminate. This is non-negotiable — improper termination causes reflection and bit errors.
- **Common ground reference.** All nodes on the CAN bus need a shared ground. Usually this comes naturally from the robot's shared power system; if nodes are powered from different supplies, a ground wire between them is essential.
- **Short stub length.** The wire from the main bus to each node's transceiver should be as short as practical (under 30 cm). Long stubs cause reflections.

For a two-motor differential drive robot, the bus might be just two transceivers and a foot or two of wire between them. Trivial to wire.

## Firmware: TWAI Capability in the Universal Template

The universal template (see [02-node-firmware-design.md](02-node-firmware-design.md)) gains optional TWAI/CAN support. Nodes detect at boot whether CAN is configured — either by probing for the transceiver, or by reading a configuration flag in NVS — and initialize the TWAI peripheral accordingly.

A node with CAN advertises this capability during registration. The capability ontology (see [06-role-discovery.md](06-role-discovery.md)) gains a new entry: `can_bus_member` with parameters indicating which bus and at what bit rate.

Application code on CAN-equipped nodes gets two additional hooks beyond the standard plugin interface:

```cpp
// Called when a CAN frame arrives on subscribed IDs.
void peripheral_handle_can_frame(uint32_t can_id, const uint8_t* data, size_t len);

// Utility to transmit a CAN frame.
void node_send_can_frame(uint32_t can_id, const uint8_t* data, size_t len);
```

The template handles TWAI initialization, error detection, bus-off recovery, and statistics. The application just sends and receives frames at defined CAN IDs.

A node may advertise its CAN frame IDs over MQTT during registration so the system has a record of who is publishing what on the bus. This is documentation, not runtime coordination — the actual coordination happens directly between the nodes over CAN.

## What Goes Over CAN vs MQTT

For a CAN-equipped node, the dividing line is sharp.

**Over CAN (only between coordinated peers):**

- Synchronized position/velocity setpoints during coordinated motion
- Peer position/velocity data needed in another node's control loop
- Emergency stop signals when fast cross-node propagation matters
- Heartbeats between coordinated peers (so each knows the other is alive)

**Over MQTT (same as any other node):**

- Firmware updates (OTA)
- Configuration (PID gains, profile parameters, what to coordinate with)
- Telemetry to the Pi (encoder positions for logging, temperatures, fault history)
- Registration conversations with the AI agent
- High-level commands ("move to position X")
- Status broadcasts ("healthy")

The principle: **MQTT handles command and report. CAN handles coordinate.** Anything that is the Pi telling a node what to do, or a node telling the Pi what it is doing, uses MQTT. Anything that is two nodes needing to act in lockstep with each other uses CAN.

## Registration With CAN

During registration (see [05-registration-workflow.md](05-registration-workflow.md)), the conversation includes a question about CAN:

> AI agent: Does this node have a CAN transceiver? If yes, what bus is it on and what bit rate?

If yes, the registration captures:

- Which CAN bus this node is on (in case the robot has multiple buses)
- The bit rate (typically 1 Mbit/s for robot applications)
- Which CAN IDs this node uses for transmit and receive
- Which other nodes it expects to coordinate with

The firmware generated for the node includes TWAI initialization with the captured parameters, and the application code includes the CAN frame handling for whatever coordination the node participates in.

If no transceiver is present, none of this happens — the node is registered exactly like any other MQTT-only node.

## When to Add CAN: A Decision Process

The recommended sequence for any new motion-coordination need:

**Step 1: Build the MQTT-only version first.** Get the actuators working independently. Get each one accepting commands and reporting state over MQTT. Verify the basic control works.

**Step 2: Try to coordinate over MQTT.** Send synchronized commands from the Pi. Measure the actual behavior. Does it work well enough for the application?

**Step 3: If MQTT coordination is inadequate, identify the specific problem.** Is it latency? Jitter? Drop-outs? Quantify the issue. "The wheels cannot track a straight line within 5 degrees over 10 meters" is a useful problem statement.

**Step 4: Add CAN to the specific nodes that need it.** Solder transceivers, wire the bus, update firmware, re-register. Verify the specific problem is resolved.

This sequence ensures CAN is added only where it earns its place. It also produces a record (in task summaries) of *why* CAN was added to those specific nodes, which is valuable institutional memory.

## What This Architecture Avoids

Treating CAN as a targeted addition rather than a parallel architecture tier avoids several pieces of complexity that other approaches require:

- **No bridge node.** Because every CAN node is also on MQTT, no translation bridge is needed. Each node publishes its own status to MQTT directly. The Pi sees everything through the standard MQTT interface.
- **No separate motion coordinator.** Trajectory generation runs on the Pi (for slow motions where MQTT latency is fine) or on one of the motion nodes as a designated leader (for fast trajectories). No dedicated hardware component is needed for this role.
- **No second node SKU.** Same hardware everywhere; CAN is an add-on, not a different product.
- **No second registration workflow.** One workflow with one optional capability question, not two parallel paths.

The result is a simpler structure that handles the cases where CAN matters without introducing CAN-specific architecture where it does not.

## Multiple CAN Buses

For a robot complex enough to have distinct motion subsystems, multiple CAN buses can coexist. Examples:

- One bus for the drive wheels (left and right motor coordination)
- A second bus for an articulated arm (multiple joint coordination)
- A third bus for a manipulator (gripper and wrist coordination)

Each bus is independent. Nodes on different buses do not directly coordinate (they communicate through MQTT and the Pi if needed). This is standard CAN practice — keep coordination domains separate so timing concerns do not cross-contaminate.

Each ESP32 has one TWAI peripheral, so each node can be on at most one CAN bus. A node that needs to participate in two coordination domains would have to talk to one bus over CAN and the other over MQTT. For practical robots this case rarely arises.

## Honest Limits

CAN adds real capability but also has real limits:

- **CAN does not solve every timing problem.** The time from "Pi sends command" to "motors start moving in sync" is still bounded by the slower of the two transports. CAN ensures the motors stay in sync with each other once they are moving. A requirement for fast response from the Pi to the motors is a different problem; CAN alone does not fix it.
- **CAN errors can cascade.** A bus problem (wiring fault, termination issue, electrical noise) affects every node on that bus. MQTT failures are typically per-node. Bus-level fault handling matters; the universal template's recovery logic should treat sustained CAN errors as a reason to fall back to MQTT-only coordination if possible, or to trigger fault states if not.
- **Isolation may eventually be needed.** Standard SN65HVD230 breakouts do not isolate the bus from the ESP32. Robots with brushed motors or harsh electrical environments may need isolated transceivers. This addition is best made when an actual problem is observed, not preemptively.
- **Adding CAN is more work than not adding CAN.** Wiring, termination, debugging, firmware additions — all real effort. The work should be undertaken only when there is a real need.

## Summary

CAN is a small, targeted addition to the MQTT-based architecture for the specific case of multiple actuators needing tight synchronization. It is added at the node level (specific nodes get a transceiver wired in), not at the architecture level (no separate motion tier, no dedicated bridge, no parallel infrastructure).

The standard hardware everywhere is ESP32-S3 N16R8 boards. The CAN add-on is a Waveshare SN65HVD230 breakout, wired with four signals to the ESP32 and connected to a shared two-wire CAN bus with termination resistors at the physical ends.

The honest expectation: most robots in this architecture need CAN on exactly two nodes (the drive wheels) and do not need it elsewhere. Start MQTT-only, prove there is a measured sync problem, and add CAN only to the specific nodes involved in that problem.

The architecture remains MQTT-first. CAN is a refinement, not a parallel structure.

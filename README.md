# Parley

Most robots wire their sensors and motors directly to a single processor. Parley is different, each sensor and motor its own microcontroller (node) and custom API. A Raspberry Pi sits at the center with the ability to rewrite the firmware on any of the nodes over WiFi at any time. The human physically attaches the nodes and programs the robot purely through a conversation with an AI agent. The AI agent researches the hardware, writes the firmware, deploys it to the right microcontroller, reads the logs, and iterates until it works. The human directs what should be built and verifies that the results match reality, without writing any code.

Author: Steve Nelson
License: Open Source
Status: Research architecture, not yet validated in production

## What Working With Parley Looks Like

A human is integrating a new GPS module into a robot. The conversation with the AI agent goes roughly like this:

> Human: I have a u-blox NEO-M9N wired to a new ESP32. TX on GPIO17, RX on GPIO16, mounted on top of the chassis.
>
> AI: Looking up the M9N... it speaks UBX over UART at 38400 baud by default. I'll use the SparkFun u-blox library, configure it for 1Hz position updates, and publish to nodes/gps-01/data/position. Sound good?
>
> Human: Yes, go ahead.
>
> [The AI generates firmware. The Pi compiles it. The binary is pushed to the new ESP32 over WiFi. The node reboots into the new firmware.]
>
> AI: Firmware is running. I'm watching the logs — the node connected to WiFi and MQTT, but I'm not seeing any UBX data on UART2. The driver is reporting "no response from receiver." This usually means TX and RX are swapped, or the baud rate is wrong.
>
> Human: Let me check the wiring... yeah, I had TX and RX backwards.
>
> [Human swaps the wires.]
>
> AI: Position fixes coming through now. Accuracy 2.3 meters, location matches roughly where you said you are. The node is registered as the primary GPS.

That exchange takes about five minutes. The new GPS node is now a full participant in the system: heartbeat publishing, OTA-updatable, recoverable from failures, integrated into the project's documentation.

This is the central thing Parley enables. New hardware integrates through conversation. The AI does the technical work of researching parts, choosing libraries, generating firmware, reading logs, and identifying issues. The human contributes the things only they can: knowledge of the physical robot, decisions about what to build, and verification that the AI's interpretations match reality.

Debugging works the same way. When something misbehaves, the AI reads logs and telemetry, forms hypotheses, proposes diagnostic firmware or configuration changes, and iterates with the human until the issue is resolved. The human is not approving generated code blindly — they are evaluating the AI's reasoning, catching errors, and redirecting when the AI heads in the wrong direction.

## What Makes This Possible

A conversational workflow requires three things from the underlying architecture, or it collapses under its own ambition.

**The AI must be able to deploy code safely.** If every firmware push risks bricking a node and requiring physical recovery, the iteration loop is too expensive to use conversationally. Parley addresses this with a layered recovery cascade: hardware watchdog, A/B firmware partitions with automatic rollback, and a factory recovery partition that can always be reached over WiFi. Failed firmware reverts automatically; the worst case is the node returning to a known-good state without human intervention.

**The AI must be able to reason about each node in isolation.** A monolithic firmware is too large for an AI to hold in working memory while reasoning about a change. Parley distributes firmware across many small microcontrollers, each dedicated to one function (or a few closely-related functions). Each node runs a shared universal template that handles infrastructure, so per-node code stays focused and small enough for the AI to work with effectively.

**The AI must have access to what the system is actually doing.** Generated firmware that the AI cannot observe is generated firmware the AI cannot debug. Parley routes node logs, status, and telemetry through MQTT to a central broker on the Raspberry Pi, where the AI can subscribe and read in real time. The AI watches deployments as they happen, identifies problems immediately, and proposes fixes based on what the hardware actually reports.

## The Compute Layout

A Parley robot has three layers of compute, each with a defined role:

**The Raspberry Pi** runs the MQTT broker (coordinating all node communication), compiles firmware from source files in the project repository, distributes compiled binaries to the appropriate ESP32 targets, and hosts robot-level coordination logic. It is the operational hub of the running robot.

**A gateway ESP32** is USB-connected to the Pi and hosts the WiFi network that all peripheral nodes join. It bridges MQTT traffic between the Pi and the wireless swarm, and acts as the recovery anchor that nodes in factory mode can always reach.

**Peripheral ESP32 nodes** each handle a specific function — a sensor, an actuator, a coordinator. They join the gateway's WiFi network, participate in MQTT, and run application code specific to their peripheral on top of the shared universal template.

The AI agent itself does not run on the robot. It runs on Anthropic's servers (for the v1 implementation using Claude) and is accessed by humans from their own devices — laptops, phones — connecting to the Pi over a network when working on the robot. This separation means the dev environment is independent of the running robot; either can be unavailable without affecting the other.

For nodes that need millisecond-level synchronization (typically motors that must coordinate motion), Parley supports adding CAN bus connections between specific peers using inexpensive transceiver modules. This is treated as a targeted refinement rather than a parallel infrastructure tier — most nodes need only WiFi.

## What's Distinctive

Two things, taken together:

The **conversational integration model** treats the AI agent as a knowledgeable engineering collaborator. The AI researches components, generates code, deploys firmware, reads logs, and diagnoses issues — but it does this in dialogue with humans who direct the work, verify the reasoning, and catch mistakes. The repository accumulates institutional memory across conversations so the system grows richer over time.

The **autonomous recovery architecture** makes wireless firmware iteration practical. Without robust recovery, every experimental push is a gamble; with it, failed firmware reverts automatically and the development loop stays fast and safe. This is what makes the conversational model work in practice instead of just in theory.

These two properties are interdependent. The recovery system is what justifies fast iteration; fast iteration is what makes the conversational model worth the investment in tooling. Either alone is interesting; together they enable a workflow that conventional embedded development cannot match.

## Relation to Existing Systems

Parley is not a replacement for ROS or other established robotics frameworks. ROS solves the problem of building robot applications from established components, with sophisticated tooling for navigation, perception, simulation, and message-passing across processes. Parley solves a different problem: how do humans and AI agents collaborate efficiently on the firmware and integration layer of a custom robot?

The two can coexist. A robot using ROS for high-level navigation logic could use Parley for the underlying microcontroller swarm. Parley's MQTT topics could be bridged to ROS topics where useful.

Engineers evaluating Parley against alternatives should consider it primarily for projects where significant custom hardware integration is required and where the team values fast iteration through AI-assisted development. Projects that primarily compose existing ROS packages, or that need ROS's ecosystem of pre-built components, are better served by ROS directly.

## Status and Honest Limits

Parley is a research architecture. Some pieces are well-grounded engineering: the partition layout, the MQTT topology, the recovery cascade, the basic node template. Others are more speculative: role discovery from observation, AI-driven firmware generation at production scale, the conversational integration model's behavior under unusual or adversarial conditions.

Specific limits to be aware of:

- The conversational integration model has been worked out in detail but has not been validated across many real peripheral integrations.
- The recovery cascade is designed but not stress-tested against the full space of possible failure modes.
- WiFi reliability in motor-noisy environments is a real risk that the architecture mitigates but does not eliminate.
- The MQTT broker is a single point of failure for swarm-wide coordination, though individual nodes continue local operation when the broker is unreachable.
- Some functions may benefit from consolidation onto a single node (a motor and its dedicated encoder, for instance); the recommended one-function-per-node default is a guideline for managing complexity, not a hard rule.
- Generated firmware can have subtle bugs that compile and run but misbehave in ways the AI does not catch from logs alone; human verification of consequential changes is essential.

These limits describe the boundary of what is currently understood. They are not flaws in the approach so much as the natural state of a research architecture awaiting more practical validation.

## Document Set

This document is the orientation. The architecture is described across ten further documents:

- [01-system-architecture.md](docs/01-system-architecture.md) — overall structure and components
- [02-node-firmware-design.md](docs/02-node-firmware-design.md) — the universal template and partition layout
- [03-recovery-and-resilience.md](docs/03-recovery-and-resilience.md) — the autonomous recovery cascade
- [04-gateway-node.md](docs/04-gateway-node.md) — the gateway ESP32 connecting Pi to swarm
- [05-registration-workflow.md](docs/05-registration-workflow.md) — the conversational integration process
- [06-role-discovery.md](docs/06-role-discovery.md) — emergent role assignment through observation
- [07-spatial-and-physical-model.md](docs/07-spatial-and-physical-model.md) — physical layout representation
- [08-collaboration-workflow.md](docs/08-collaboration-workflow.md) — the dashboard interface that unifies system visibility, AI conversation, and direct command execution
- [09-can-bus-additions.md](docs/09-can-bus-additions.md) — targeted CAN coordination for motion-critical peers
- [10-local-logging.md](docs/10-local-logging.md) — per-node logging that survives firmware rollback for post-failure diagnostics

Engineers evaluating Parley should read this document first, then [01-system-architecture.md](docs/01-system-architecture.md) for the technical overview.

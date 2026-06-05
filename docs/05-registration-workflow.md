# Registration Workflow

This document describes how new peripheral hardware joins the Parley system. Registration is the most concrete example of the conversational integration model — the workflow where the human, the AI agent, and the running system work together to bring a new piece of hardware to life.

Read [01-system-architecture.md](01-system-architecture.md) for context on the overall system, [02-node-firmware-design.md](02-node-firmware-design.md) for how individual nodes are structured, and [08-collaboration-workflow.md](08-collaboration-workflow.md) for the dashboard interface through which registration conversations happen.

## What Registration Is

Registration is the process by which a new physical peripheral — a sensor, an actuator, a display, or any other component — becomes a working part of the robot system. It is fundamentally a conversation between a human and the AI agent, mediated through the Pi, that produces several artifacts:

- A new piece of application firmware running on a specific ESP32
- A configuration entry in the system's persistent state
- An updated entry in the part library (knowledge for future registrations)
- A task summary documenting what was done and why

The physical work — wiring the peripheral to the ESP32, mounting it on the robot — is done by humans before the conversation begins. The intellectual work — figuring out what library to use, how to talk to the chip, how it integrates with the rest of the system — happens during the conversation.

## What Registration Is Not

To set expectations clearly:

- **Not a wizard with checkboxes.** Real peripherals are too varied for a one-size-fits-all flow. The conversation has structure but also flexibility.
- **Not fully automatic.** Critical information about wiring, mounting, and intent comes from humans who can see and touch the robot. The AI agent cannot observe the physical hardware directly.
- **Not infallible.** Generated firmware sometimes does not work on the first try. The workflow handles iteration gracefully.
- **Not a one-shot operation.** Registration produces an initial working integration; subsequent tasks may refine it.

The goal is to make the common case dramatically faster than traditional embedded development, not to eliminate the need for human judgment.

## The Conversation Phases

A typical registration follows five phases. They are not rigid — the AI agent should adapt to what the specific conversation needs — but they provide a useful frame.

### Phase 1: Trigger and Initial Context

Registration begins when a new ESP32 appears on the network in factory mode. This can happen because:

- A human just plugged in a freshly flashed board.
- An existing node was wiped and is being repurposed.
- A previously-registered node has lost its configuration and is being re-registered.

The Pi sees the factory-mode announcement on `system/discovery`. The human initiates the registration conversation, perhaps with a simple prompt like "let's set up that new board."

The AI agent acknowledges and asks for orienting information. Not a rigid form — a focused conversation. Typical questions:

- What kind of peripheral is this? (sensor, actuator, what type)
- What is the part? (manufacturer, model, breakout board if relevant)
- How is it wired to the ESP32? (specific pins for power, ground, data lines, interrupts)
- Where is it mounted on the robot? (high-level location; spatial details come later)
- What is the intent? (what role this will play in the overall system)
- Does this node have a CAN transceiver? (if motion coordination might be needed; see [09-can-bus-additions.md](09-can-bus-additions.md))

Humans answer in natural language. The AI agent parses these into structured information and asks follow-ups for anything unclear.

### Phase 2: Research and Design

The AI agent researches the part. This is where the agent's broad knowledge and ability to use the web matter most. The research typically covers:

- The datasheet (if available)
- Communication protocol details (I2C address, UART baud rate, SPI mode, etc.)
- Available Arduino or PlatformIO libraries
- Library quality and maintenance status
- Common integration gotchas
- Example code where available

From this research, the AI agent proposes a design: what library to use (or whether to write the protocol from scratch), what initialization sequence is appropriate, what data to publish at what rate, what commands to accept, how to validate that the firmware is working.

The design is presented to the human for review. The human can accept, modify, or push back. Common interactions at this phase:

- "Actually, let's publish at 1 Hz, not 5 Hz."
- "Skip the temperature sensor on this chip — we only care about pressure."
- "Use a different MQTT topic that follows this convention."
- "That library is abandoned; use this other one."
- "Don't use a library; write the protocol directly from the datasheet."

The conversation continues until the design is agreed.

### Phase 3: Firmware Generation

The AI agent writes the application firmware. This is the code that fills in the plugin interface defined in [02-node-firmware-design.md](02-node-firmware-design.md):

- `setup_peripheral()` — initialize the hardware and register MQTT topics
- `loop_peripheral()` — read sensors and check for events
- `peripheral_health_check()` — return whether hardware initialized successfully
- `peripheral_handle_command()` — handle any incoming commands

The firmware is built on the Pi (or a separate build host) using PlatformIO, in a new source directory for this node type. The build either succeeds or surfaces compile errors that the AI agent diagnoses and fixes.

When the build succeeds, the AI agent presents the generated code (or at least the key parts) to the human for review. The human can ask questions, request changes, or approve. Quick approval is fine — the validation in Phase 4 will catch issues that look fine in code review.

### Phase 4: Flash and Validation

The new firmware is OTA-pushed to the node. Because the node is in factory mode, factory firmware accepts the update and reboots into the new application slot.

The AI agent watches for the node to come back online and announce itself with the new firmware version. The validation gate (see [03-recovery-and-resilience.md](03-recovery-and-resilience.md)) determines whether the firmware is healthy enough to keep:

- Did it connect to WiFi?
- Did it connect to MQTT?
- Did `peripheral_health_check()` return true?

If validation passes, the agent moves to functional verification: does the peripheral actually do what it is supposed to do? This is peripheral-specific:

- For a sensor: are readings showing up on the expected topic? Are the values physically reasonable?
- For an actuator: does sending a test command produce the expected response?
- For a display: does writing a test message show on the screen?

Humans are essential to functional verification, because they can observe the physical robot. "I just sent a command to spin the motor — did it spin?" "Is the GPS reporting your actual location?"

If verification fails or values look wrong, the workflow iterates. The AI agent diagnoses based on logs and observed behavior, proposes a fix, regenerates firmware, pushes again. The local logging subsystem (see [10-local-logging.md](10-local-logging.md)) makes this loop fast even when initial firmware fails to validate — the failed firmware's log is automatically retrieved after rollback.

If verification passes, registration moves to the final phase.

### Phase 5: Integration and Documentation

The node is now working in isolation. Final steps integrate it into the system as a whole:

**Assign a stable node ID.** During factory mode, the node was identified by MAC address. Now it receives a human-readable ID (`imu-01`, `gps`, `motor-left`, etc.) that is stored in NVS and used in topics going forward.

**Update the part library.** The integration approach for this part is recorded — library used, key configuration, validation criteria, gotchas encountered. Future registrations of the same part can reuse this.

**Update the spatial layout model.** The peripheral's mounting location and orientation are recorded (see [07-spatial-and-physical-model.md](07-spatial-and-physical-model.md) for the layout file format).

**Capture the capability registration.** What this node can do — what it publishes, what it accepts as commands — is recorded so other parts of the system can find it.

**Write the task summary.** The conversation is summarized: goal, decisions made, firmware version, validation results, anything notable. This becomes part of the project's history.

**Commit to the repository.** Generated firmware source, configuration, layout entry, library updates, task summary — all committed to git. Future AI sessions will read this.

The node is now a fully integrated, documented, recoverable, observable part of the system.

## What Humans Provide

Some information is impossible to derive without human involvement. The registration conversation is structured around obtaining this information clearly.

**Part identity.** What it is, manufacturer, model number, breakout board if relevant. The AI agent can sometimes deduce identity from descriptions or photos, but explicit identification is faster and more reliable.

**Wiring map.** Which pins on the ESP32 connect to which pins on the peripheral. This includes power and ground, data lines (SDA/SCL, TX/RX, etc.), and any optional pins (interrupts, enables, resets).

**Physical context.** Where on the robot the peripheral is mounted and how it is oriented. This affects how readings are interpreted and how actions translate to robot motion.

**Intent.** What this peripheral is supposed to do in the larger system. A motor could be a wheel, a winch, an arm joint, or something else entirely — the same hardware can play very different roles.

**Constraints.** Limits the AI agent should respect. "Don't run this motor above 50% — the gearbox can't handle it." "This sensor is finicky; use the slow init sequence even though it's not strictly required."

Humans are the source of physical-world truth. The AI agent is the source of engineering knowledge. The conversation combines them.

## What the AI Agent Provides

**Technical research.** Datasheets, libraries, protocol details, common pitfalls.

**Design proposals.** A coherent integration approach the human can accept or modify.

**Code generation.** Application firmware that fits the project's conventions and uses the universal template correctly.

**Diagnosis.** When things do not work, the AI agent reads logs (real-time MQTT logs and local logs retrieved from nodes), asks questions, forms hypotheses, and suggests next steps.

**Documentation.** The artifacts that capture what was done and why.

Humans can do all of these things too. The agent does them faster and more consistently for common cases. Human time is best spent on the things only humans can do.

## The Part Library

A key artifact that grows over time: a library of "this kind of part has been integrated before, here's how." Each entry contains:

- Part identification (manufacturer, model, common variants)
- Communication protocol and default settings
- Recommended library
- Standard wiring conventions (typical pinout if it is a breakout board)
- Initialization sequence
- Standard data format and topic structure
- Validation criteria
- Known gotchas and workarounds
- Example application code
- Links to relevant task summaries from past integrations

When registering a part that is already in the library, the conversation is dramatically shorter. The AI agent can propose: "This project has integrated a NEO-M9N before. The previous integration used the SparkFun u-blox library at 1 Hz publishing to `nodes/<id>/data/position`. Same approach?" The human confirms or modifies, and most of the design phase is skipped.

The part library lives in the project repository, in a structured format (one directory per part family, with markdown documentation and example code). The AI agent reads it at the start of any registration session.

This is also where the system's institutional knowledge accumulates. Workarounds, tweaks, lessons learned — all captured in a place that future sessions will see. The library prevents the same problems from being solved repeatedly.

## When Things Go Wrong

Several common failure modes occur, with established diagnostic paths.

### Generated Firmware Does Not Compile

Compile errors are the easiest case. The AI agent reads the error, identifies the issue (missing include, wrong API call, library version mismatch), and fixes the code. Usually one or two iterations resolve it.

If errors persist, the likely culprits are library version mismatches, an outdated ESP32 board package, or incorrect PlatformIO configuration.

### Generated Firmware Compiles But Does Not Validate

The node boots, but `peripheral_health_check()` returns false, or the validation gate times out. This usually means hardware initialization failed.

Common causes:

- I2C device not at expected address (bus scan needed)
- UART baud rate mismatch
- SPI mode mismatch
- Wiring error (TX/RX swapped, SDA/SCL swapped)
- Missing pull-up resistors on I2C
- Power not connected or insufficient

Diagnosis involves reading the boot logs from the node (real-time MQTT logs during validation; local logs retrieved from LittleFS after rollback), looking at error messages, and asking the human to verify wiring. Often the human notices the issue immediately ("oh, I put it on GPIO16 not 17"). Sometimes it is a library configuration issue the AI agent investigates further.

A useful diagnostic technique: generate a minimal "scanner" firmware that probes the bus and reports what it finds. This often reveals the actual hardware state.

### Firmware Validates But Readings Are Wrong

The firmware is running, the bus is working, but the values do not match physical reality. This usually means a configuration issue: wrong scale factor, wrong byte order, wrong register, missing calibration step.

Diagnosis is mostly checking values against expectations. "The IMU should read 1G of acceleration along the Z axis when stationary; it is reading 0.1G — probably a scale factor issue or wrong register."

Human physical access matters here. The human can rotate the IMU, watch the readings change, and confirm whether the response matches expected behavior. This kind of testing is not something the AI agent can perform alone.

### The Library Is Buggy or Inadequate

The chosen library may have bugs, may not expose needed functionality, or may otherwise be unfit. Options:

- Fork the library and patch it locally.
- Switch to a different library if one exists.
- Write the protocol from scratch from the datasheet.

Writing from scratch is more work but often produces cleaner integration. The AI agent can do this with human guidance — the human points at the relevant datasheet sections, the agent writes the implementation, the validation loop catches mistakes.

### The Peripheral Is Just Hard

Some integrations are genuinely difficult: niche parts with poor documentation, proprietary protocols, parts that need precise timing the framework does not support well. These take longer and require more human involvement.

This is acceptable. The workflow handles fast cases very fast and slow cases at a normal pace. The expectation is not that every integration takes five minutes — it is that the easy ones take five minutes and the hard ones take what they need.

## Iteration After Registration

Registration produces an initial working integration. It is not the end. Subsequent tasks may:

- **Tune parameters.** PID gains, sensor filtering, publish rates.
- **Add features.** Additional commands, additional published data, new operating modes.
- **Refactor.** Restructure the application code as understanding deepens.
- **Calibrate.** Sensor calibration, motor encoder ratios, mounting offsets.

Each of these is a separate task with its own conversation, its own focus, its own summary. Registration is the first task for a given peripheral, not the only task.

This discipline keeps tasks bite-sized. Registration produces working firmware. Improvements are separate work. Registration should not sprawl into "make this peripheral perfect" — it should produce "this peripheral is integrated and working in its basic form."

## Re-registration

Sometimes a node needs to be re-registered:

- The hardware was moved to a different physical location.
- The wiring changed.
- The peripheral was replaced with a different model.
- The role of the peripheral changed.

Re-registration is much like initial registration but with prior context. The AI agent reads existing task summaries and configuration, asks the human what changed, and updates accordingly. Often this is faster than initial registration because most of the design work is reusable.

## How Maturity Changes the Workflow

As the part library grows and the project matures, several patterns become visible:

**Common parts get fast.** A second BNO055 IMU integration takes a fraction of the time the first one took. The library entry has the design baked in.

**Project conventions solidify.** Topic structures, message formats, naming patterns become consistent because the AI agent follows established precedent rather than inventing new approaches each time.

**The system becomes self-documenting.** Each registration adds documentation that future registrations build on. The repository becomes a teaching artifact for understanding the system.

**Human involvement shifts toward design.** Early on, humans are involved in many low-level details. As patterns solidify, human input concentrates on high-level intent and constraint, with less attention needed on implementation specifics. The collaboration matures.

This trajectory is part of what makes the workflow scale. Early peripherals are slow; later ones are fast. By the time the system is complex, adding a new peripheral is routine.

## The Underlying Principle

Registration encapsulates one of Parley's deepest principles: **knowledge is built collaboratively and captured persistently.** Nothing important stays in any one person's head or in any one AI session's context. Everything ends up in the repository, where it informs future work.

The team's collective memory — what has been tried, what works, what to avoid — is a real artifact, not a hope. The registration workflow is the mechanism by which that memory grows.

When someone returns to a project after months away, the part library and task summaries are how they re-orient. The system describes itself. Re-discovery of forgotten context is not required.

## Honest Limits

The registration workflow as described has been worked out in detail but has not been validated across many real peripheral integrations. Several limits worth being aware of:

- **AI agent capability varies by part familiarity.** Common parts with good documentation integrate smoothly. Obscure parts, parts with proprietary protocols, or parts requiring tight timing analysis may stress the conversational model. The fallback is more human involvement, which is fine but is the slow path.
- **Generated code can have subtle bugs.** Code that compiles, validates, and produces reasonable-looking readings may still have edge-case errors the AI agent does not notice. Human review of consequential firmware remains essential.
- **The part library only helps if it is maintained.** Skipping Phase 5 (documentation) on a registration breaks the library's value for future work. The discipline of completing every registration through documentation is real ongoing work.
- **Re-registration assumes the prior task summaries are accurate.** If a previous registration was poorly documented, re-registration starts closer to initial registration in effort.
- **The model assumes a knowledgeable AI agent.** A less capable agent (older model, smaller context window, lacking web access) would produce a different and likely worse experience. The workflow described here assumes capabilities at the level of current Claude models.

These limits are real but not disqualifying. The workflow is designed to handle them — by giving humans visibility into what the AI is doing, by capturing knowledge in persistent artifacts, and by iterating freely on safely-recoverable nodes.

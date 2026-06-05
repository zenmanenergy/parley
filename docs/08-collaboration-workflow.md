# The Parley Dashboard

This document specifies the Parley dashboard: a unified web interface that combines system visibility, AI-mediated conversations, and direct command execution into a single working environment.

Read [01-system-architecture.md](01-system-architecture.md) for the underlying system. The dashboard is the human-facing layer on top of that system.

## What the Dashboard Is

The Parley dashboard is the primary interface through which humans work on a Parley robot. It is not a status display layered onto an existing workflow — it is the workflow. Conversations with the AI agent, system monitoring, command execution, and project documentation all happen through the dashboard.

Architecturally, the dashboard is a web application served from the Pi. Humans access it from their laptops (the primary surface) or phones (for quick check-ins and on-the-robot debugging). Multiple clients can be connected at once and see the same shared system state.

The dashboard is not yet implemented. This document specifies its design.

## Architecture

The dashboard is a three-tier web application:

**Frontend.** A responsive web application that runs in the browser. Three-panel layout on laptop-class screens; stacked panels with swipe navigation on phones.

**Backend.** A small Python service running on the Pi alongside Mosquitto. It serves the frontend static assets, manages WebSocket connections to browser clients, bridges between WebSocket and MQTT for real-time data, makes calls to the AI agent's API, and persists conversation and task state to local files.

**WebSocket transport.** All real-time communication between frontend and backend is over WebSocket. Browsers receive a live stream of MQTT activity (heartbeats, telemetry, anomalies, logs). They send conversation messages, command intents, and UI actions. The bidirectional flow over a persistent connection is what makes the dashboard feel live rather than polled.

```
[ Browser (Laptop or Phone) ]
            ↕ WebSocket
[ Dashboard Backend (on Pi) ]
            ↕ MQTT
[ Mosquitto Broker (on Pi) ]
            ↕ USB
[ Gateway ESP32 ]
            ↕ WiFi
[ Peripheral Nodes ]
```

The browser never connects directly to ESP32 nodes. Everything flows through the Pi. This keeps the security model simple — the Pi is the single trust boundary for the swarm — and means the gateway and nodes do not need any awareness of the dashboard.

Authentication for v1 is local-only: the dashboard binds to localhost (or local network only) and does not handle multi-user accounts. Networked deployments with authentication can be added later if needed.

## The Three Main Panels

On a laptop-class screen, the dashboard presents three panels side by side.

### Left Panel: System View

The left panel shows the state of the robot at a glance. Its contents:

- **Node list.** Every registered node, with status indicator (healthy, degraded, in factory mode, offline). Clicking a node opens its detail view.
- **Spatial map (optional).** A schematic view of the robot showing where nodes are physically mounted. Clicking a node on the map opens its detail view. The map is useful when the robot has many nodes or when physical adjacency matters for debugging; it is optional for simpler robots and can be hidden.
- **Anomaly feed.** Recent items from `system/anomalies` — nodes that fell back to factory mode, prediction errors that climbed, firmware versions that are stale. Each anomaly is clickable; clicking it opens a conversation focused on that issue.
- **System health summary.** Top-of-panel indicators: broker connected, all nodes responsive, no active alerts. Color-coded for glanceable status.

The system view updates in real time as MQTT traffic flows in. A node that drops offline shows red within seconds. A new anomaly appears in the feed immediately. The human can always glance at the left panel to see the system state without asking.

### Center Panel: Active Conversation

The center panel is where work actually happens. It is a chat interface between the human and the AI agent, with several augmentations.

The conversation has:

- **Message history.** Standard chat layout, human and AI turns interleaved.
- **Command execution inline.** When the AI agent issues commands to nodes, those commands appear in the conversation flow with their results. See the Commands section below for details.
- **Telemetry references.** When the conversation mentions specific sensor readings or system state, those references can link to live data — clicking shows a chart or current value without leaving the conversation.
- **Reasoning chain visibility.** The AI agent's hypotheses include the data they are based on. "I think the gear ratio is wrong because: commanded 180, encoder reported 82, layout says ratio is 2:1, observed ratio is 2.2:1." Humans can see what evidence the conclusion rests on.

Multiple conversations can be open at once, accessed by tabs across the top of the panel. Each conversation has its own context, its own AI session, and its own task list. Switching tabs switches conversations without losing state in either.

### Right Panel: Context and Tasks

The right panel shows what is loaded for the current conversation and what tasks are emerging from it.

- **Loaded context.** Documents, files, and prior task summaries currently in the AI agent's working memory. Humans can add or remove context items as the conversation evolves.
- **Surfaced tasks.** As the conversation progresses, the AI agent identifies discrete pieces of work — calibration tasks, bug fixes, documentation updates, follow-ups. These appear in a task list as they are surfaced.
- **Related artifacts.** Files being modified, the part library entry being updated, the layout file change about to be committed. The human can see what changes the conversation is producing before they are committed.

The task panel is the workflow's task-management system. There is no separate task tracker — tasks emerge from conversations, live in the right panel, and persist to the repository as they are accepted.

## Conversations as the Primary Work Surface

The conversation is where the work happens. Several design decisions shape how conversations work.

### Starting a Conversation

A new conversation begins with a brief context-setting:

- **Title.** Free text. Becomes the conversation name shown in the tabs and the eventual task summary.
- **Related nodes** (optional). Pick from the registered node list. If specified, the AI agent loads firmware, recent logs, and layout entries for those nodes.
- **Conversation kind** (optional). Categories like Register, Debug, Tune, Refactor, Investigate, Other. Hints at what context to pre-load. The AI agent adapts regardless of the category.

None of these are required. A conversation can start with just a title, and the AI agent figures out what context is relevant as the discussion develops. The categorization is a convenience, not a constraint.

### Multiple Parallel Conversations

Humans can have several conversations open simultaneously. Each is isolated — context loaded into one conversation does not bleed into others. The AI agent treats each conversation as independent.

This matters because real work is often interleaved. A debugging session for the GPS might surface a tangential calibration issue. Rather than mixing them in one conversation (where the AI agent's context degrades), the calibration becomes its own conversation. Both stay focused.

Conversations can reference each other when relevant. A link from one conversation to another preserves the connection in the project history without coupling the conversations' contexts.

### Tasks Emerging From Conversations

This is one of the dashboard's central ideas. The AI agent does not require humans to define tasks up front. Instead, as a conversation develops, the AI agent identifies discrete pieces of work and surfaces them in the task panel.

For example, a debugging session might surface:

- "Calibrate the GPS antenna position offset" (proposed)
- "Update the prediction error threshold for GPS" (proposed)
- "Document the multipath issue observed during this session" (proposed)

These appear in the right panel as the conversation produces them. Humans can:

- **Accept** a task, making it active and ready to be worked on
- **Reject** a task, removing it from the list
- **Defer** a task, marking it for later without acting on it now
- **Focus** the conversation on a specific task, scoping the remaining conversation to that task

The human is never blocked by the task list. They can ignore it, accept everything, or curate carefully. The point is to capture work that would otherwise be forgotten, not to gate the conversation behind acceptance decisions.

When a conversation closes, accepted-but-incomplete tasks remain as work for later. Rejected tasks are gone. Deferred tasks land in a scheduled-for-later state. The conversation itself becomes a summary attached to whichever task it primarily addressed.

### Closing a Conversation

Closing a conversation triggers automatic cleanup:

- The accepted tasks that were completed during the conversation are marked done.
- A summary is generated (by the AI agent, reviewable by the human) capturing the goal, decisions, artifacts produced, and outcome.
- The summary is committed to the repository in the `tasks/` directory with a filename that includes the date and title.
- Any artifacts modified during the conversation (firmware, configs, layout, part library) are committed alongside.

The human can edit the summary before it commits. This is the only point where summary-writing is a deliberate step — and even here, the AI agent does the first draft.

## Direct Command Execution

The AI agent can issue commands to nodes from within a conversation. This is what turns the dashboard from a status display into a working environment.

### How It Works

When the AI agent decides to issue a command, the command appears in the conversation as a structured action:

```
[AI sent: nodes/arm/cmd/rotate {angle: 180}]
[Acknowledged by arm node, position updating...]
[Encoder reports: rotation complete, final angle 82 degrees]
```

The command is fully visible. Humans see what was sent, when it was sent, and what the result was. There is no hidden tool use — the AI agent's actions are part of the conversation flow.

After the command, the AI agent interprets the result:

> The encoder reports 82 degrees of actual rotation for a 180-degree command. That's a ratio of about 2.2 to 1, but the layout file says 2 to 1. The calibration appears to be off.

### Authorization Levels

Not all commands are equally safe. The dashboard supports three authorization levels:

- **Free.** The AI agent can issue these without per-command confirmation. Examples: reading sensor values, requesting log dumps, sending small actuator probes at low intensity, querying node status.
- **Confirmed.** The AI agent proposes the command; the human clicks "approve" before it sends. Examples: spinning motors at higher power, engaging actuators near limits, modifying NVS values, pushing new firmware.
- **Locked.** The AI agent cannot issue these from a normal conversation. Some genuinely dangerous commands (full-speed motor, high-current actuator, anything that could damage the robot or surroundings) require a typed confirmation phrase or operating in a separate "permitted" mode.

The authorization level for each command type is configured per-node during registration. A motor that drives a heavy arm might require confirmation for any rotation. A motor that drives a lightweight steering linkage might allow free probing.

### Always-Available Emergency Stop

The dashboard has a prominent emergency stop control, separate from any conversation, always available regardless of conversation state. Clicking it publishes an immediate stop command to a system topic that every actuator node subscribes to with high priority. Motors stop, actuators hold position, the robot enters a safe state.

The emergency stop does not depend on any conversation working correctly. If the AI agent has gone off the rails or a command is producing unsafe behavior, the human has an unconditional path to safety.

## Code as a First-Class Artifact

The AI agent's work produces code, and that code is not hidden behind the conversation — it is directly visible and editable in the dashboard. This is what bridges "the AI is doing engineering work" with "humans can inspect and modify that work at any level of detail."

### Accessing Code From the Node View

When humans drill into a node from the system view, the node detail panel includes a code tab. The current firmware source for that node is visible — the application code that implements the node's specific function. Humans can read it, search it, and edit it directly without going through a conversation.

The shared universal template that runs on every node is also accessible from a project-level view, since changes there affect all nodes.

### How the AI Agent Presents Edits

When the AI agent proposes code changes during a conversation, the changes appear as a diff in the right panel under a Code tab. The presentation follows established patterns:

- Removed lines are shown crossed out in red
- Added lines are shown highlighted in green
- Unchanged surrounding lines are visible as context
- Each change is annotated with the AI agent's reasoning ("Changed baud rate from 9600 to 38400 to match the M9N default")

When the AI is proposing edits, the Code tab gets attention indicators (a badge on the tab, optional automatic focus). Humans can switch between the task panel and the code review without losing either.

### Accept, Reject, Modify

For each proposed change, humans can:

- **Accept all.** All changes go to the working source. The next build picks them up.
- **Reject all.** Changes are discarded. The AI agent learns the reason if the human provides one and adjusts its hypothesis.
- **Accept partial.** For multi-hunk changes, accept some sections and reject others. This is more advanced UI but useful for substantive edits where most changes are correct but a few are wrong.
- **Modify and accept.** Edit the proposed change before accepting. The AI agent sees the modified version and incorporates it into subsequent reasoning.

Acceptance updates the source code in the working repository. It does not automatically trigger compilation or deployment — those remain separate explicit steps.

### Deployment as a Separate Step

By default, accepting code changes updates the source but does not push new firmware to the node. Deployment is a separate action (typically a single click): compile, validate the build, push to the node's inactive partition, trigger the rollback-protected switch.

This separation matters because multiple changes often want to be deployed together. Accepting three small edits and then deploying once is cleaner than three separate deploy cycles.

For tight iteration loops during active debugging, an optional "deploy on accept" mode collapses the steps. The human toggles this per-conversation when they want rapid iteration: each accepted change triggers an immediate compile and push. This is appropriate when actively isolating a bug and unhelpful when reviewing routine changes.

### Direct Editing Without AI

The code view supports direct human editing without conversation. Humans can:

- Open a file, change a value, save it, deploy
- Edit firmware while a conversation is paused or closed
- Make quick adjustments that do not warrant an AI dialogue

When humans edit directly, any active conversation can see what changed. The AI agent picks up the edit on its next message: "I see you changed the watchdog threshold from 30 to 60 seconds. That should reduce false-positive resets on the slow boot path."

### Trust Modes

The dashboard supports two ways of working that humans can fluidly switch between.

**High-trust mode.** The AI proposes changes, the human glances at them or skips review, deploys. Useful for routine work — adding a sensor type that has been integrated many times before, applying a standard fix to a common pattern, making small adjustments the AI has high confidence in. The human verifies the result through behavior rather than through code review.

**Co-coding mode.** The human reviews every change carefully, asks questions, suggests modifications, sometimes writes their own code that the AI refines. Useful for novel work, safety-critical code, anything where the human's standards exceed the AI's confidence.

Most conversations mix both modes — high trust on small changes, careful review on substantive ones. The dashboard makes either as easy as the other. The same accept/reject/modify flow works for both, with the difference being how much time the human spends reading the diff before deciding.

### AI Agent Confidence

When proposing code changes, the AI agent indicates its confidence level. Examples:

> Confidence: high — this is the standard fix for this type of issue
>
> Confidence: medium — based on indirect evidence; behavior should improve but review recommended
>
> Confidence: low — this is a hypothesis from limited data; careful review and testing needed

The human's review effort can match the stated confidence. High-confidence changes get glanced at; low-confidence changes get scrutinized. This is a heuristic, not a guarantee — the AI's self-assessed confidence can itself be wrong — but it helps allocate human attention.

### Risks Worth Naming

Several risks are inherent to code-review-mediated AI work:

- **Subtle bugs in generated code.** Code that compiles, validates, and produces reasonable-looking behavior can have edge-case errors a quick review will miss. The local logging subsystem (see [10-local-logging.md](10-local-logging.md)) helps catch problems at runtime, but caution is warranted for any change to safety-relevant code.
- **Over-trust drift.** When high-trust mode works well for many small changes, humans may extend trust to changes that warrant more scrutiny. The AI's confidence indication helps but is itself uncertain. Discipline about reviewing substantive changes is needed regardless of how routine the workflow feels.
- **Code editing UI quality matters.** If humans rely on the dashboard for code editing, the editor needs to be genuinely capable — syntax highlighting, find-and-replace, multi-cursor editing, the things developers expect. A poor editor pushes humans back to their own tools, fragmenting the workflow.

These risks do not invalidate the approach. They describe where care and attention pay off.

## Handling Uncertainty in Human Reports

A specific failure mode worth designing for: the human provides verification information that is imprecise or wrong, and the AI agent forms a hypothesis based on it.

Example: AI commands the arm to rotate 180 degrees. The actual rotation is 82 degrees. The human glances at the arm and reports "looked like about 90." The AI agent forms a hypothesis ("ratio is 2 to 1") that is wrong because the human's estimate was off by 8 degrees.

The dashboard supports several patterns to handle this.

### Use Telemetry Over Reports When Telemetry Exists

If the actuator has an encoder, the AI agent should use the encoder reading rather than the human's eyeball estimate. The dashboard makes telemetry visible alongside human reports in the conversation:

> Commanded rotation: 180 degrees
> Encoder reading: 82 degrees
> Human estimate: 90 degrees
> Calibration suggestion: ratio appears to be ~2.2 : 1, not the configured 2 : 1

The encoder is the truth. The human's report is a sanity check that the encoder is reading something physically plausible.

### When the Human Is the Only Sensor, Run Multiple Experiments

If no sensor measures what matters and the human is the only judge, the AI agent should not trust a single report. It runs multiple experiments at varying scales and looks for the pattern across them.

Example calibration of an LED brightness vs commanded value:

- Command brightness 25%. Human reports: "dim"
- Command brightness 50%. Human reports: "medium"
- Command brightness 75%. Human reports: "bright"
- Command brightness 100%. Human reports: "very bright"

A single human report is noisy. The pattern across four reports is much more reliable. The AI agent extracts the trend rather than fixating on individual values.

For numeric estimates, the AI agent should vary the commanded values and average the slope of the human's responses, which is much more reliable than the absolute numbers. The AI should also vary the direction of tests when possible, to cancel systematic perceptual biases (humans often underestimate angles, for example).

The AI agent should explain to the human why multiple measurements are needed: "I'm going to rotate four different amounts. Eyeball estimates have noise, but the pattern across multiple rotations will be reliable."

### Ask for the Most Reliable Verification Form Available

The AI agent should match the request to what's possible:

- "How many degrees did it rotate?" invites an unreliable eyeball estimate.
- "Did the rotation match what you expected, or was it noticeably off?" invites a categorical judgment, which is more reliable than precise numbers.
- "Use the protractor on the arm to read the actual angle" invites an instrumented measurement, which is most reliable.

The AI should ask for the form of verification that the human can actually provide given their tools. If they have a protractor, ask them to use it. If they don't, ask for a category. If even categorical answers are uncertain, fall back to multiple-experiment averaging.

### Show the AI's Reasoning Chain

When the AI agent commits to a hypothesis, the conversation should show the evidence chain. Not just "AI thinks the gear ratio is wrong" but "AI thinks the gear ratio is wrong because: commanded 180, encoder reported 82, layout says 2:1, observed 2.2:1."

This lets humans spot bad inferences. If the AI cites a piece of evidence that was misreported, the human can correct it before the hypothesis hardens. The dashboard renders this reasoning chain inline with the AI's conclusions, not hidden in a separate view.

### Don't Demand More Precision Than the Next Step Needs

The AI agent should not press humans for precision that isn't actionable. "Did it rotate at all?" is fine for a yes/no debug. "Roughly how far?" is fine for hypothesis-forming. "Measure precisely with a protractor" only when the precision matters for the next step.

When the AI asks for precision, the conversation should make clear why that precision is needed. Then humans understand whether to estimate or instrument.

## The System View in Detail

The left panel's system view has more depth than just a list.

### Node Detail

Clicking a node opens its detail panel: current firmware version, uptime, boot counter, recent reset reason, last heartbeat time, WiFi RSSI, free memory, recent log entries, and the node's MQTT topic activity.

From the node detail, humans can:

- Open a conversation focused on this node (loads firmware, recent logs, layout entry as context)
- Issue diagnostic commands (request a log dump, change log level, request a heartbeat)
- View live telemetry charts for this node's published data
- Initiate node-specific actions (force reboot, trigger factory mode, push new firmware)

### Anomaly Feed

System-level anomalies appear in a feed at the bottom of the left panel:

- "Node motor-left fell back to factory mode (3 times this week)"
- "GPS prediction error has been climbing — was 1.2m last week, now 3.8m"
- "Firmware version on imu-01 is two months old; recent updates not applied"

Each anomaly is clickable. Clicking opens a new conversation pre-loaded with the relevant context. The anomaly itself becomes the conversation's starting context.

Anomalies that are addressed (or explicitly dismissed) disappear from the feed. Anomalies that are ignored repeatedly should fade rather than escalate — the human's judgment is final, and a system that nags becomes a system that gets disabled.

## The Spatial Map

The optional spatial map view shows the robot schematically with nodes positioned where they are physically mounted. This is useful when:

- The robot has many similar nodes (an articulated arm with multiple joints) and physical position helps disambiguate.
- Mounting adjacency matters for debugging (two nodes close together both showing issues may share a cause — loose connector, damaged cable, EMI source).
- Conversations need to specify physical context: clicking a node on the map starts a conversation focused on that physical location.

The map is built from the layout file (see [07-spatial-and-physical-model.md](07-spatial-and-physical-model.md)). It updates when the layout is updated. It is a visualization of the spatial model, not a separate authoritative source.

The map is optional. For simple robots with few nodes, the list view is sufficient and the map adds unneeded complexity. The dashboard should support disabling the map for projects that don't need it.

## Telemetry Visualization

Sensor data over time is genuinely useful for debugging — calibration drift, prediction error trends, motor temperature behavior, intermittent sensor failures. The dashboard supports live charts of node telemetry.

Charts can be:

- Embedded in conversations (the AI agent references a value and the chart appears inline)
- Opened from node detail (live view of a specific node's data)
- Pinned to the dashboard (a small persistent chart for something being actively monitored)

The visualization is straightforward — line charts with time on the X axis, value on the Y axis, recent history visible at a glance, with the ability to zoom out for historical context. Multiple metrics can overlay on the same chart for comparison.

The data backing the charts is whatever has flowed through MQTT recently. The Pi maintains a rolling history (typically a few hours to a few days) in local storage; queries beyond that retrieve from the repository's archived logs.

## Mobile Access

Phones get a vertically-stacked layout: the conversation takes the full screen, with the system view and task panel accessible via swipe or tabs. The same backend, the same conversation state, the same commands — just laid out for a smaller screen.

Phone access is useful for:

- Quick check-ins on system state without opening a laptop
- Running registration conversations while physically near the robot
- Emergency stop access from anywhere on the local network
- Brief diagnostic conversations during testing sessions

The phone interface is not the primary working surface — substantial conversations and code review happen better on a laptop. But the phone interface should be complete enough that minor tasks and emergency response don't require getting a laptop out.

## What Gets Persisted

The dashboard's local state includes:

- All conversations (history, surfaced tasks, AI context loaded at the time)
- Task summaries written at conversation close
- Node metadata (current state, recent telemetry windows, anomaly history)
- Recent log retrievals from nodes
- Dashboard configuration (which views are active, telemetry charts pinned, etc.)

Conversations and task summaries are committed to the project repository on close. Other state (recent telemetry, current node status) lives in local files on the Pi and is regenerated from MQTT activity when needed.

The principle: anything that should survive across Pi reboots and across project sessions lives in the repository. Anything that is operational state of a running session lives in local files.

## Honest Status

The dashboard is currently a design specification, not an implemented system. Engineers evaluating Parley should treat this document as describing the destination, not the starting point.

Until the dashboard is built, Parley can be used through more primitive interfaces:

- Direct MQTT clients (mosquitto_sub, mosquitto_pub) for system observation
- SSH to the Pi for command-line work
- The AI agent accessed through whatever interface the v1 implementation provides (the Claude web UI or API directly)
- Manual git workflow for committing task summaries and artifacts

These interfaces are functional but require humans to manually do what the dashboard would do automatically: write task summaries, correlate node status with conversation context, coordinate between conversation and action, and so on. The dashboard's value is in unifying these into a single surface; without it, the manual coordination overhead is real but tolerable for small projects.

## Implementation Effort

Building the dashboard is a substantial software project. A first version covering the core (three-panel layout, single-conversation chat, basic command execution, simple node list) is probably weeks of focused work. The fully-featured version described here — multiple conversations, spatial map, telemetry charts, surfaced tasks, mobile interface — is more.

Incremental implementation makes sense. The minimum useful dashboard is probably:

1. Backend that bridges WebSocket and MQTT
2. Node list view with status
3. A single conversation panel with the AI agent
4. Inline command execution
5. Code view for nodes with basic accept/reject of AI-proposed changes
6. Basic emergency stop

Everything else (spatial map, multiple conversations, surfaced tasks, telemetry charts, mobile layout, partial accept, deploy-on-accept mode, direct in-dashboard editing) builds on this foundation. Each addition produces value proportionally to the work required.

## Honest Limits

The dashboard is the most ambitious component of Parley:

- **Substantial implementation effort.** Real software project, not a quick utility.
- **Depends on AI agent capability.** Surfaced tasks, command execution from chat, reasoning chain visibility — these all rest on the AI agent doing real engineering work, not just responding to messages.
- **WebSocket connections need careful handling.** Reconnects, queued messages during disconnection, multiple clients seeing consistent state — these are real engineering problems that need attention.
- **The authorization model needs care.** Free commands that turn out to be unsafe in some context, locked commands that turn out to be needed urgently — the levels are guidelines and the right choices depend on the specific robot.
- **Emergency stop is genuinely critical.** It must work even when the dashboard is misbehaving. Testing this thoroughly is non-trivial but essential.

These limits are real but not disqualifying. The dashboard is the largest single piece of work Parley implies, but it is also where most of the workflow's value is realized. An incomplete dashboard with the core working is far better than no dashboard at all.

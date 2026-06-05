# Role Discovery

This document describes how Parley can determine the *roles* of its peripherals through observation rather than declaration. It is the most exploratory document in the set, describing a capability that is well-grounded in principle, valuable when it works, and intended to be implemented incrementally as the system matures.

Read `05-registration-workflow.md` first. Role discovery extends the registration workflow with an additional idea: not all of a peripheral's identity needs to be told to the system; some of it can be derived from what the peripheral does in the world.

## Status

Role discovery as described in this document is a research direction within Parley, not a validated implementation. The earlier phases (capability advertisement, manually-declared structural models, observation-based validation) are well-understood engineering. The later phases (assisted discovery, autonomous discovery) are speculative and have not been built at scale.

Engineers evaluating Parley should treat this document as describing where the architecture *could* go, with the understanding that incremental implementation is both possible and recommended.

## What "Role" Means Here

A peripheral has both a **capability** (what its hardware can do) and a **role** (what it means in the larger system).

Capability is intrinsic and easy to declare. A motor with an encoder can spin forward and backward and report rotation. A GPS module can produce position fixes. An IMU can measure orientation and acceleration. These are properties of the hardware that do not depend on context.

Role is contextual and emerges from how the peripheral is used. Two motors with encoders can be the left and right wheels of a differential drive robot, the joints of an arm, the wheels of a four-wheel-drive vehicle, or many other things. The same hardware, very different roles.

Traditional systems require humans to declare both: "this is a motor, and it is the left wheel." Role discovery allows humans to declare only capability and lets the system derive role from observation.

## Why This Is Worth Pursuing

Three reasons, in rough order of practical value:

**Robustness to physical change.** If hardware is rearranged — left and right wheels swapped, a sensor remounted in a new orientation, a motor replaced with a different model — the system can re-derive its self-model rather than requiring manual configuration updates. This is the most directly useful consequence.

**Catches human errors.** During registration a human might declare "this is the left wheel" and be wrong. Role discovery cross-checks declarations against observed behavior. The system can flag inconsistencies: "Commanding this motor turns the chassis right, which is the opposite of what a left wheel would do. Verify the wiring."

**Better engineering pattern.** Configuration is brittle; discovery is robust. A system that builds and maintains its own self-model through observation is closer to how biological systems work and produces a more honest engineering artifact than one that relies on hand-maintained configuration files. Discovered properties stay current as the robot changes; declared properties drift out of sync.

## The Capability Ontology

For role discovery to work, the system needs a defined vocabulary of capabilities. Not an open-ended list, but a structured ontology that specifies what each capability means and what data it produces.

A starting set:

| Capability | Description | Data |
|------------|-------------|------|
| `rotational_actuator` | Can apply torque to a rotating shaft | Receives speed/torque commands; reports state |
| `rotational_sensor` | Measures rotation of a shaft | Reports angle, velocity |
| `linear_actuator` | Can apply force along an axis | Receives position/force commands |
| `linear_sensor` | Measures position along an axis | Reports position |
| `position_2d` | Reports 2D position in some frame | Reports x, y |
| `position_3d` | Reports 3D position in some frame | Reports x, y, z |
| `orientation_3d` | Reports 3D orientation | Reports quaternion or Euler angles |
| `angular_velocity_3d` | Reports rotation rate around 3 axes | Reports rate vector |
| `linear_acceleration_3d` | Reports linear acceleration along 3 axes | Reports acceleration vector |
| `magnetic_field_3d` | Reports magnetic field vector | Reports B vector |
| `pressure` | Reports atmospheric pressure | Reports pressure |
| `temperature` | Reports temperature | Reports temperature |
| `range_1d` | Reports distance to nearest obstacle in one direction | Reports distance |
| `image_2d` | Reports 2D pixel data | Reports image frames |

Each capability has a defined message format on its standard MQTT channel. A node that advertises `position_3d` publishes data conforming to a known schema, regardless of what specific hardware is producing it.

This ontology is the shared vocabulary that makes discovery possible. Without it, every node speaks its own language and there is no common ground for reasoning about relationships.

The ontology is extensible. New capabilities are added when new hardware types arrive. Extending the ontology is a project-level decision, not a per-node decision — additions should be reviewed and committed to the repository, not invented during individual registration sessions.

## The Discovery Process

Discovery is essentially system identification — running controlled experiments to learn the input-output relationships of a system. Applied to a robot whose configuration is unknown, this becomes "command actuators in defined patterns, observe how all sensors respond, and deduce structure from the correlations."

A typical discovery session follows these steps.

### Step 1: Establish a Baseline

Hold the robot still. Record what every sensor reports under static conditions. This becomes the noise floor — readings during this baseline that are not constant reveal sensor noise characteristics, the gravity vector orientation for IMUs, the starting GPS position, and so on.

The baseline is a snapshot of what the world looks like when nothing is happening.

### Step 2: Probe Each Actuator Individually

For each actuator capability advertised by the system, command it in a defined way and watch all sensors. For a rotational actuator:

- Command 20% forward for 1 second
- Wait for motion to settle
- Record what every sensor saw during the command and the period after
- Command 20% reverse for 1 second
- Record again
- Return to neutral

The recording captures correlations. If commanding motor A causes the GPS to translate 0.3 m to the north, motor A is somehow connected to forward motion of the robot. If it also causes the IMU to register a yaw rotation of 12 degrees clockwise, motor A produces both translation and rotation — characteristic of a wheel on one side of a differential drive.

### Step 3: Probe Combinations

After individual probes, run combinations. Spin motor A and motor B simultaneously. Record what happens. The combined response provides cross-coupling information that individual probes cannot reveal.

For two wheels, the combined response should be nearly pure forward motion (the rotational components cancel). If it is not, either the wheels are not symmetric, or one is not a wheel, or there is something else going on.

### Step 4: Solve for Structure

Given the correlation data from probing, solve for the structural model.

For a differential drive robot:

- Each wheel produces translation (proportional to its rotation) and rotation about the center (proportional to its rotation, with sign depending on which side).
- The wheelbase (distance between wheels) determines the ratio of translation to rotation per wheel rotation.
- The wheel radius determines the distance traveled per encoder tick.

From the observed data:

- GPS displacement per motor command yields wheel radius times encoder gearing.
- IMU yaw rate per motor command yields wheel position offset from center.
- The ratio of these two yields the wheelbase.

The math is overdetermined when multiple sensors provide redundant information, which means parameters can be solved for with reasonable accuracy and the solution can be validated against expected physical relationships.

### Step 5: Assign Roles

Once the structural model is solved, role labels can be applied. "Motor A produces +V translation when commanded forward and +ω rotation when commanded forward, with consistent ratios. This matches the role 'left wheel of a differential drive.' Motor B has the opposite rotation sign — that is the right wheel."

The labels are outputs of the discovery process, not inputs.

## What Sensors Need to Be Available

Discovery only works if there is enough sensor information to observe the effects of actions. Common cases:

**Best case: GPS plus IMU.** Translation and rotation can both be measured directly. Discovery is straightforward and works in most outdoor conditions.

**Indoor case: IMU only.** Rotation can be measured (gyroscope and magnetometer). Linear motion is much harder — accelerometer integration drifts badly, and translations are typically not visible in a useful way. Which actuators cause rotation can still be discovered, but distinguishing "moves forward" from "moves backward" becomes ambiguous without an external reference.

**Workbench case: motors elevated, wheels spinning freely.** No chassis motion at all. The system can still discover that motor A and motor B are independent rotational actuators with their own encoders, but it cannot discover what they do in the world because they are not doing anything.

**Camera-augmented case: visual odometry or AprilTags.** A camera looking at a fixed reference (a known floor pattern, or AprilTags on the walls) provides translation information indoors. This unlocks more powerful discovery in non-GPS environments.

The honest summary: discovery works best for differential drive robots outdoors. It works partially indoors with IMU only. It needs additional sensors for richer scenarios.

## The Indoor Problem

The indoor problem deserves explicit attention because much robot development happens indoors.

Practical workarounds:

**Calibrate outdoors, deploy indoors.** Run discovery once outside where GPS works, save the resulting structural model, and use that model for subsequent indoor operation. The model is a property of the robot's mechanical configuration, not its environment, so it remains valid as long as the hardware does not change.

**Manual ground truth.** During discovery, a human pushes the robot around or measures motion with a tape measure. The human reports observed motion to the AI agent, which incorporates it into the model. This is slower but works without external sensors.

**Visual reference.** Apply a few AprilTags to the floor and put a camera on the robot. The camera's motion relative to the tags provides translation data. This adds hardware but is useful for indoor work.

**Wheel-only model.** If the actuators have encoders, the encoders themselves provide some information about motion in the wheel frame. A kinematic model that says "the robot moves d meters per encoder tick" can be derived without ever directly observing world-frame motion. The model is correct as long as the wheels do not slip — a strong assumption, but often a useful one.

## Cross-Sensor Consistency

Multiple sensors cross-check each other, and this is one of the most valuable properties of discovery.

When motor A is commanded forward briefly, three things should be consistent:

1. Motor A's encoder reports rotation of N ticks.
2. The IMU reports an angular velocity around the chassis Z axis with some integral θ.
3. The GPS reports a translation vector of magnitude d in some direction.

If these three are consistent (the encoder count predicts the IMU rotation through the wheelbase model, and predicts the GPS translation through the wheel radius model), the model is correct. If they are inconsistent, either the model is wrong or one of the sensors is misbehaving.

This consistency check is also how the system continues to monitor itself during normal operation. If during routine motion the encoder predicts more translation than the GPS observed, something is happening — wheels slipping on a wet surface, an encoder misreading, GPS multipath. The discrepancy itself is information.

A peripheral whose readings stop matching the model should be flagged for human attention. The system can detect when its self-model has gone stale.

## Mounting Offset Discovery

A subtler form of discovery: figuring out where a sensor is mounted, not just what it does.

**IMU mounted off-center.** When the robot rotates, an off-center IMU measures both the rotation (correctly) and a centripetal acceleration (because it is at a radius from the rotation axis). The accelerometer reading during pure rotation reveals the radius — the IMU's offset from the rotation center.

**IMU mounted at non-zero orientation.** If the IMU is bolted in at 45 degrees from the chassis frame, every reading is in its own frame, not the robot's. Discovery can determine the rotation by comparing IMU-frame motion to actuator-frame predictions during defined motions.

These offsets are properties of how the hardware was physically installed. They cannot be determined without observation. Discovery handles them naturally — they become parameters in the structural model that get solved alongside everything else.

Humans can also provide initial estimates: "the IMU is mounted with X forward, but the rotation axis is uncertain." Discovery refines the estimate, potentially catches errors, and produces a calibrated model.

## When Discovery Fails or Is Ambiguous

Discovery has limits. Scenarios where it fails:

**Symmetry without a tiebreaker.** Two motors that are perfectly symmetric (same response to commands, same effect on sensors) are distinguishable only by which one was commanded. Without external reference, the system cannot determine which is "left" — only that they are a mirrored pair. Naming requires either human input ("the one I just commanded is the left one") or asymmetric reference (a wheel encoder with a slightly different ratio, an asymmetric robot frame).

**Coupled mechanisms.** Two motors that are mechanically coupled (driving the same axle, for example) move together when either is commanded. The model sees "motor A and motor B always move together" and might or might not correctly identify this as coupling rather than coincidence.

**Non-rigid systems.** Discovery assumes the robot is approximately rigid. Significant flexure, suspension travel, or articulated parts produce data that does not fit a simple kinematic model.

**Insufficient sensor coverage.** A robot with actuators but limited sensors (just encoders, no IMU, no GPS) lacks enough information to derive the structural model. Discovery requires sensor diversity to work.

**Hardware faults.** A flaky sensor produces noise that confuses the discovery process. Validation criteria during discovery should include sanity checks — readings within physical bounds, low noise during baseline, and so on.

In ambiguous cases, the system should be honest: "These two motors are independent actuators, but it is not clear which is left and which is right. Please indicate." Falling back to human input is correct. Pretending to know things that cannot be derived is not.

## The Conversational Layer

Discovery does not have to happen automatically. It works well as a conversational tool that humans invoke when relevant.

A typical interaction:

> *Human:* I just added a new motor node, can you figure out what it does?
>
> *AI agent:* I will run a discovery routine. Make sure the robot has space to move — we will do small motions but the robot will move around. Ready?
>
> *Human:* Go ahead.
>
> *AI agent:* [runs probe sequence] Here is what I observed: commanding the new motor at 30% forward caused the robot to rotate counter-clockwise and translate slightly backward. Combined with the existing motors, the new one acts as a third wheel that rotates the robot when activated alone. This looks like a steering or castering wheel rather than a drive wheel. Is that consistent with how you wired it?
>
> *Human:* Yes, it is a steering wheel on a tricycle configuration.
>
> *AI agent:* Got it. I will register it as a steering actuator and update the kinematic model. The model now treats this as a tricycle with two rear drive wheels and one front steering wheel.

This pattern is collaborative — the AI agent proposes interpretations, the human confirms or corrects, and the model gets built. It is also self-documenting: the conversation becomes part of the task summary, recording why the model is what it is.

## Persistence and Re-Discovery

Once discovery has run successfully, the resulting structural model is stored in the repository. Subsequent operations use the stored model rather than re-deriving it.

The model can become stale. Reasons it might need to be re-derived:

- Hardware was modified (wheel changed, sensor moved, motor replaced).
- Sensor recalibration changed reported values.
- The robot was moved between environments (indoor vs outdoor) where different sensors are reliable.
- The ongoing consistency check flagged discrepancies.

Re-discovery is a deliberate operation, not automatic. Humans trigger it when they know something has changed, or when the system flags persistent inconsistency. Running discovery routinely without reason wastes time and risks confusing the model with transient effects.

The stored model has a timestamp and confidence value. When checking the model's validity during normal operation, both inform decisions. "The current model was last verified two months ago. It has been showing 5% prediction error since the wheels were replaced. Re-discovery is recommended."

## Discovery Probe Safety

Running discovery means commanding actuators in defined ways. This has safety implications.

Probes should be:

**Conservative in magnitude.** Low torque, low speed, short duration. Discovery does not need to push the system hard; it just needs to produce observable correlations.

**Bounded in space.** The robot should stay within its work area. If discovery is going to move the robot, the human should clear space first.

**Stoppable.** A clear emergency stop mechanism, separate from the discovery process. If something looks wrong, the human can halt immediately.

**Monitored.** During discovery, the system watches for anomalous responses. A motor that draws unusual current, a sensor that produces wild values, a structural noise that suggests mechanical contact — any of these should pause discovery for human review.

For a robot that has the potential to damage itself or its environment (heavy, fast, or operating near valuable things), discovery should be even more conservative. Always prefer slower, smaller probes that take longer over faster, larger probes that finish quickly.

## Implementation Strategy

This is the most ambitious capability in Parley and should be implemented incrementally, not as a single project.

**Phase 1: Capability advertisement only.** Every node declares its capabilities at registration. The system builds a capability inventory but does not try to infer roles. This is achievable with the work described in `05-registration-workflow.md` and provides most of the immediate practical benefit.

**Phase 2: Manual structural model.** Humans, with AI agent assistance, write the structural model explicitly. "Wheels A and B are differential drive, wheelbase 0.4 m, wheel radius 0.05 m." Stored in the repository. Used for control. No automatic discovery yet.

**Phase 3: Validation against observation.** During normal operation, the system checks the manually-written model against observed sensor data. Discrepancies are flagged. This catches mistakes in the manual model and builds confidence the model is correct.

**Phase 4: Assisted discovery.** The AI agent runs probe sequences and proposes model parameters to humans. Humans retain final authority. The agent does the math; the human approves the result.

**Phase 5: Autonomous discovery.** The system runs discovery on demand and updates the model with minimal human involvement. Validation criteria ensure the new model is at least as consistent as the old.

Each phase builds on the previous and provides value on its own. Skipping directly to Phase 5 is not advisable — the earlier phases produce the data, conventions, and confidence that make later phases possible.

Most projects probably stop at Phase 2 or 3 and that is fine. The architecture supports Phase 5 but does not require it.

## Honest Limits

Role discovery is not magic. It works well in clean, well-instrumented scenarios and degrades in messy, under-instrumented ones. It cannot compensate for missing sensors. It cannot infer things that are not observable.

Specific limits engineers should be aware of:

- **Phases 4 and 5 are speculative.** The math is correct; the conversational interface has been worked out; the practical implementation across many robots has not been done. Treat the later phases as a research direction within Parley, not a proven capability.
- **Discovery requires sufficient sensor diversity.** A robot with only motor encoders cannot derive its own structural model. Adding sensors specifically to enable discovery is an architecture decision with its own costs.
- **Indoor operation is harder.** Without GPS or visual odometry, only rotation is reliably observable. Translation discovery requires creative workarounds.
- **Symmetry breaks the model.** Perfectly symmetric configurations require external information to label.
- **The system can confidently get the wrong answer.** Bad sensor data, mechanical coupling, or non-rigid behavior can produce a model that fits the observed data but does not match the physical robot. Cross-sensor consistency catches many such cases, but not all.

Treat role discovery as a powerful tool that earns its complexity in the right contexts, not as a feature that should be applied everywhere. Many projects work fine with traditional configuration through Phase 2 manual declaration. Discovery is worth pursuing further when the system is complex enough that configuration becomes a maintenance burden, when hardware is being rearranged frequently, or when the engineering goal of "the system maintains its own self-model" is itself a project priority.

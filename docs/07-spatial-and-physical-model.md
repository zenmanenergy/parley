# Spatial and Physical Model

This document describes how Parley represents the physical layout of a robot — where peripherals are mounted, how they are oriented, and why this matters for interpreting sensor data and controlling actions.

Read `01-system-architecture.md` for system context, `05-registration-workflow.md` for how layout information is captured during registration, and `06-role-discovery.md` for how some of this information can be derived from observation rather than declaration.

## Why Spatial Information Matters

A sensor's reading is not a property of the sensor alone — it is a property of the sensor plus its mounting. The same IMU produces different orientation data depending on whether it is bolted flat to the chassis or rotated 90 degrees. The same camera sees different things depending on whether it is pointing forward or down. The same GPS antenna receives different satellite coverage depending on whether it is under metal or in the open.

Without a model of how peripherals are physically arranged on the robot, the system can use raw sensor data only naively. Combining data across sensors, predicting how actions will affect the world, or interpreting readings in a robot-relative frame all require knowing the spatial relationships.

The cost of getting this wrong is subtle misbehavior. The robot turns the wrong way, navigates by the wrong reference, applies controls in the wrong frame. These bugs are insidious because the firmware looks fine in isolation — the issue is in how readings are interpreted at the system level.

## Coordinate Frames

The system uses a small set of named coordinate frames. Each frame is a 3D coordinate system with a defined origin and orientation.

### The Robot Frame

The primary frame. Origin at the robot's geometric center (or some other defined reference point — the center of the wheelbase is common). Orientation:

- **+X forward**, in the direction the robot considers "ahead"
- **+Y left**, perpendicular to forward
- **+Z up**, perpendicular to the ground when the robot is upright

This is a right-handed coordinate system. The convention matches ROS and most robotics literature.

The robot frame is the default frame for commands and high-level reasoning. "Move forward 1 meter" means motion along +X in the robot frame. "Turn left" means rotation about +Z.

### Per-Peripheral Frames

Each peripheral has its own frame, with origin at the peripheral's mounting point and orientation determined by how it is physically installed. These frames are child frames of the robot frame.

Examples:

- The IMU's frame might have origin 5 cm above the chassis center, with axes rotated relative to the robot frame depending on how the IMU was bolted in.
- A wheel's frame has origin at the wheel hub and axes aligned with the wheel's rotation.
- A camera's frame has origin at the camera's optical center and axes aligned with image conventions (Z forward into the scene, X right, Y down — opposite from the robot frame).

The transformation between the robot frame and each peripheral frame is captured as a pose: a 3D position offset and a rotation. Once these poses are known, sensor data can be transformed into the robot frame for unified reasoning.

### The World Frame

For navigation and outdoor operation, a world frame is needed. The conventional choice for outdoor robots is ENU: X east, Y north, Z up. GPS positions are reported in this frame (or in closely related coordinates that can be projected to ENU locally).

Indoor systems sometimes use a fixed local frame defined by external infrastructure (AprilTags, fiducials, a starting pose). The choice depends on the application.

The robot's pose in the world frame is what navigation algorithms use to plan and track motion. It is derived from sensor data — typically GPS for position, IMU integrated with magnetic heading for orientation — through sensor fusion.

## How Layout Information Is Captured

During registration of a peripheral (see `05-registration-workflow.md`), the human provides spatial information as part of the conversation. The level of detail depends on what the peripheral needs.

### Position-Only Peripherals

For peripherals where mounting position matters but orientation does not (a temperature sensor, a pressure sensor, a battery monitor), the human describes the location:

> "The temperature sensor is mounted near the center of the chassis, just inside the front panel."

This is captured as approximate coordinates in the robot frame: `(0.15, 0, 0.05)` for "15 cm forward, 0 lateral, 5 cm above the floor." Precision does not matter much for these — the value of the location is mostly for human reference, not for computation.

### Orientation-Sensitive Peripherals

For peripherals where mounting orientation matters (IMU, camera, GPS antenna), the human describes both position and orientation:

> "The IMU is mounted on top of the chassis, near the center, with the chip's X arrow pointing forward and the Z axis pointing up."

This is captured as both position and a rotation. If the IMU is mounted with its axes aligned with the robot frame, the rotation is identity. If it is rotated, the rotation captures that.

The conversation can include any orientation hints the human has:

> "I had to mount it sideways because of the case design — its X axis points to the right of the robot, and its Y axis points forward."

This describes a specific 90-degree rotation that the system applies when interpreting IMU readings.

### Kinematic Peripherals

For wheels, joints, and other peripherals that participate in the robot's motion, more detail is needed:

- Position of the rotation axis
- Direction of the rotation axis
- Wheel radius (for wheels)
- Gear ratio between motor and output (for geared systems)
- Travel limits (for joints with hard stops)

This information is what makes the kinematic model possible — turning encoder counts into chassis motion, or chassis commands into motor commands.

## Stored Format

Layout information lives in the project repository as structured data, typically YAML or JSON. Example for a small differential drive robot:

```yaml
robot:
  frame: chassis_center

peripherals:
  imu_node:
    type: orientation_3d
    frame:
      parent: robot
      position: [0.0, 0.0, 0.05]
      rotation: [0, 0, 0]    # roll, pitch, yaw — no rotation needed

  gps_node:
    type: position_2d
    frame:
      parent: robot
      position: [0.0, 0.0, 0.30]    # antenna mounted high
      rotation: identity

  left_wheel:
    type: rotational_actuator
    frame:
      parent: robot
      position: [0.0, 0.20, 0.0]    # 20 cm to the left
      rotation_axis: [0, 1, 0]      # Y axis (lateral)
    wheel_radius: 0.05
    encoder_ticks_per_revolution: 1024
    gear_ratio: 20

  right_wheel:
    type: rotational_actuator
    frame:
      parent: robot
      position: [0.0, -0.20, 0.0]   # 20 cm to the right
      rotation_axis: [0, 1, 0]
    wheel_radius: 0.05
    encoder_ticks_per_revolution: 1024
    gear_ratio: 20
```

This is the system's spatial self-knowledge. Code that needs to reason about the robot's body reads this file and works in the robot frame using the transformations specified.

## Using the Model

A few common operations rely on the spatial model.

### Transforming Sensor Data

When the IMU reports angular velocity, that data is in the IMU's frame. To use it for robot-level reasoning, it is rotated into the robot frame using the IMU's rotation in the layout:

```
omega_robot = R_imu_to_robot * omega_imu
```

If the IMU is mounted with rotation identity (aligned with the robot frame), this is a no-op. If it is rotated 90 degrees, the components swap accordingly.

This is the kind of operation that produces subtle bugs when wrong. "The robot turns the wrong way when commanded" often traces back to a missed or incorrect frame transformation.

### Computing Forward Kinematics

For a wheeled robot, forward kinematics turns wheel rotations into chassis motion. The math depends on the wheel positions and orientations from the layout. For a differential drive:

```
v_robot_x = (v_left_wheel + v_right_wheel) / 2
omega_robot_z = (v_left_wheel - v_right_wheel) / wheelbase
```

Where `v_left_wheel` and `v_right_wheel` are linear velocities at the wheel contact points (encoder ticks × 2π × wheel_radius / ticks_per_revolution), and `wheelbase` is the distance between wheels (computed from their layout positions).

The layout file provides the wheelbase and wheel radius. The kinematics code reads them rather than hard-coding them.

### Computing Inverse Kinematics

The reverse operation: turning desired chassis motion into wheel commands. For the same differential drive:

```
v_left_wheel = v_robot_x - omega_robot_z * wheelbase / 2
v_right_wheel = v_robot_x + omega_robot_z * wheelbase / 2
```

This is what runs when a command says "drive forward at 0.5 m/s while turning at 30 degrees per second" — the desired chassis motion is decomposed into wheel commands.

### Centripetal Compensation

If an IMU is mounted off-center (not at the chassis center), it experiences centripetal acceleration during turns even when the chassis is not accelerating linearly. The layout's IMU position relative to the rotation axis allows this to be compensated:

```
a_centripetal = omega^2 * r
a_robot_imu_corrected = a_imu_measured - a_centripetal
```

Without this correction, the IMU "feels" acceleration during pure rotation, which confuses sensor fusion algorithms.

### GPS-IMU Fusion

Combining GPS position with IMU motion requires knowing the offset between the GPS antenna and the chassis reference point. The GPS reports the antenna's position; obtaining the chassis position requires transforming by the antenna offset rotated by the current robot orientation.

## Precision Targets

Spatial models can be specified to extreme precision, but the precision actually needed depends on the application.

**Coarse precision (~10 cm) is fine for:**

- Logging and human reference
- Approximate sensor placement for environmental sensors
- High-level navigation in large environments

**Medium precision (~1 cm) is needed for:**

- Forward and inverse kinematics
- Sensor fusion at typical robot scales
- Useful odometry estimates

**Fine precision (~1 mm) is needed for:**

- Manipulation tasks
- High-accuracy positioning
- Calibration of optical systems

Most robot projects work with medium precision throughout. Fine precision is rarely needed unless the application specifically demands it. Time spent measuring to a tenth of a millimeter when a centimeter is sufficient is better spent elsewhere.

The layout file should also indicate confidence. A position measured carefully with calipers is more trustworthy than one estimated by eye. Marking the confidence helps later debugging: "the IMU was recorded as 5 cm above the chassis, but that was an eyeball estimate" tells future investigation to measure more carefully if predictions diverge from observations.

## Updating the Model

The layout changes when:

- A peripheral is moved.
- A peripheral is replaced with a different model that has different mounting.
- The robot itself is modified (a wheel changed, a sensor relocated).
- Discovery (see `06-role-discovery.md`) refines parameters.

Each change is a deliberate update to the layout file. The change is committed to git with a clear message ("moved IMU 3 cm forward to clear new servo mount"). The change is also reflected in any task summaries that document the modification.

This is the kind of documentation that robot projects need and rarely have. Months later, when something seems off, the git history of the layout file shows what physical changes happened and when. Without this, debugging requires recreating history from memory, which is unreliable.

## Cross-Checking Layout Against Reality

The layout file describes the physical robot, but the description can be wrong. Several ways to check:

**During registration.** When a new peripheral is added, the validation phase often surfaces layout errors. "The IMU is declared as mounted with X forward, but readings during a forward push show motion in the IMU's Y axis. The mounting may actually be rotated 90 degrees." Catching this immediately is much better than discovering it later.

**During discovery.** Role discovery (see `06-role-discovery.md`) compares predicted to observed motion. Mismatches often trace back to layout errors — a wheel offset that does not match the actual mounting, an axis orientation that is wrong, a wheelbase measurement that is off.

**During normal operation.** The system can continuously check layout-derived predictions against observed sensor data. Persistent prediction errors suggest the layout is stale — something has physically changed without the layout being updated.

**Manually.** Periodic physical verification of the layout is worth doing once at the start and after significant modifications. This is tedious but produces ground truth that automated checks cannot.

The layout should not be treated as "set once and forget." It is a living description of the robot, kept in sync with the actual hardware.

## Mounting Conventions

Standard mounting conventions reduce confusion:

**For IMUs, mount with axes aligned to the robot frame when possible.** This is not always possible (case constraints, board orientation, cable routing), but when it is, the layout rotation is identity and there is no math to get wrong.

**For cameras, document the optical axis explicitly.** "Pointing forward" is ambiguous — does the optical axis point along +X (into the scene) or along +Z (up out of the camera)? Image-frame conventions differ from world-frame conventions; explicit documentation prevents mistakes.

**For wheels, document the rotation axis direction.** Which direction is positive rotation? Right-hand rule along which axis? This determines what "forward command" means for the motor controller, and getting it wrong causes the wheel to spin backward.

**For GPS antennas, document the patch orientation.** GPS patch antennas have a directional gain pattern — the "up" face should face the sky. Mounting matters more than is often appreciated for GPS performance.

These conventions are captured in the part library (see `05-registration-workflow.md`) so future registrations of the same hardware type follow the same patterns.

## Articulated Robots

Everything above assumes a rigid robot — a single rigid body whose parts do not move relative to each other (except for wheels rotating, which is rotation only, not translation in the chassis frame).

Articulated robots — robots with joints, arms, articulated bases — are more complex. Each segment has its own frame, frames are connected by joints whose state changes during operation, and the layout becomes a kinematic tree rather than a flat list.

The system can be extended to handle articulation: each joint becomes a peripheral with capabilities `rotational_actuator` and `rotational_sensor`, and the layout tracks the kinematic chain. The math is more involved, and the design decisions about how to represent and update the chain are more complex.

For initial scope, rigid robots are simpler. Articulation should be added when a specific project needs it, with the awareness that the layout subsystem becomes meaningfully more complex.

## Soft and Compliant Systems

Real robots are not perfectly rigid. Suspensions flex, frames bend slightly under load, mountings have small amounts of play. For most purposes this is negligible — the rigid model works fine. For high-precision applications (manipulation, optical alignment), compliance matters and the simple rigid layout becomes inadequate.

A robot with significant suspension travel or doing precision work should treat the layout as a nominal description rather than an exact one, and use sensors to measure actual configuration during operation.

For most general-purpose robots in Parley's target scale, the rigid model is sufficient.

## Honest Limits

The spatial model as described is well-grounded in standard robotics practice, but several aspects deserve explicit attention:

- **Manual measurement is error-prone.** Positions and orientations measured by hand are typically accurate to a few millimeters and a few degrees. This is fine for most purposes but inadequate for precision tasks.
- **Coordinate convention errors are common.** Mixing up left vs right, swapping axes, getting the sign of rotation wrong — these are easy mistakes that produce confusing bugs. Conventions in this document are deliberate; deviating from them invites errors.
- **The rigid-body assumption holds only at moderate accuracy.** Sub-millimeter precision typically requires modeling compliance, which this architecture does not address directly.
- **Articulated robots are a substantial extension.** The architecture supports them in principle (kinematic trees instead of flat layouts) but the implementation effort is significantly higher than rigid robots.
- **The layout file requires discipline to maintain.** When hardware changes are not reflected in the layout, the system silently uses stale information. Catching this requires either deliberate cross-checking or noticing the resulting misbehavior.

These limits are realistic engineering constraints, not flaws unique to this architecture. The same issues apply to any spatial modeling approach for robots.

## Summary

The spatial and physical model is the bridge between the digital system (firmware, configurations, MQTT messages) and the physical robot (mounted hardware, mechanical structure, motion in the world). It captures:

- Where each peripheral is mounted
- How each peripheral is oriented
- The kinematic structure of the robot's drive system
- Mounting conventions and assumptions

It enables:

- Correct interpretation of sensor data
- Forward and inverse kinematics
- Sensor fusion
- Predictive checks against observed reality

It lives in the repository as structured configuration, gets updated deliberately as the robot is modified, and is cross-checked against reality through registration validation, discovery, and ongoing operation.

It is a small but essential piece of infrastructure. Without it, sensor and actuator interpretation requires hard-coded assumptions everywhere. With it, the system maintains an honest record of its physical state and can adapt when that state changes.

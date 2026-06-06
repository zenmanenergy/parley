# Parley Capability Ontology

The Parley capability ontology defines a standardized vocabulary of what peripheral hardware can do. It enables the system to reason about compatibility, similar integrations, and feature matching without requiring deep domain knowledge of every part type.

## Design Principles

1. **Finite and well-defined:** The ontology is a closed vocabulary, not open-ended. New capabilities are added deliberately, reviewed, and committed to the repository.

2. **Composable:** A peripheral can advertise multiple capabilities. A 9-DOF IMU advertises three separate capabilities: `orientation_3d`, `angular_velocity_3d`, and `linear_acceleration_3d`.

3. **Data-focused:** Each capability is defined by the data it produces or consumes, not by the hardware. This enables two different sensors that produce equivalent data to be treated as interchangeable.

4. **Implementation-agnostic:** The ontology describes what data is produced; it does not specify how. An MPU-6050 and a BNO055 both produce `angular_velocity_3d` but via different protocols.

## Core Capabilities

### Actuators

#### rotational_actuator
Can apply torque to a rotating shaft.
- **Receives:** angle_deg (float), speed_rpm (float), torque_nm (float)
- **Reports:** angle_deg (float), speed_rpm (float), position_encoder_ticks (int)
- **Examples:** DC motors, servo motors, stepper motors
- **Common:** Differential drive wheels, arm joints, pan-tilt actuators

#### linear_actuator
Can apply force along an axis.
- **Receives:** position_m (float), force_n (float)
- **Reports:** position_m (float)
- **Examples:** Linear solenoids, hydraulic cylinders, lead-screw actuators
- **Common:** Gripper jaws, slide mechanisms

### Sensors: Position and Motion

#### position_2d
Reports 2D position in some reference frame (usually world frame).
- **Reports:** x_m (float), y_m (float), confidence_m (float)
- **Examples:** GPS (outdoor), visual odometry, dead reckoning
- **Common:** Outdoor navigation, localization

#### position_3d
Reports 3D position in some reference frame.
- **Reports:** x_m (float), y_m (float), z_m (float), confidence_m (float)
- **Examples:** Optical motion capture, lidar, multi-sensor fusion
- **Common:** Aerial robots, indoor localization with markers

#### orientation_3d
Reports 3D orientation as a quaternion or Euler angles.
- **Reports:** qx (float), qy (float), qz (float), qw (float) [quaternion format]
- **Alternatives:** roll (float), pitch (float), yaw (float) [Euler format]
- **Examples:** IMU, compass, AHRS module
- **Common:** Maintaining robot heading, articulated platform orientation

#### rotational_sensor
Measures rotation of a shaft (encoder).
- **Reports:** angle_deg (float), velocity_dps (float)
- **Examples:** Rotary encoder, magnetic encoder, optical encoder
- **Common:** Wheel encoders, joint angle measurement

#### linear_sensor
Measures position along an axis.
- **Reports:** position_m (float)
- **Examples:** Linear potentiometer, magnetic linear encoder, ultrasonic distance
- **Common:** Slide position measurement, work envelope sensing

### Sensors: Inertial and Environmental

#### angular_velocity_3d
Reports rotation rate around three axes (rad/s or dps).
- **Reports:** gx (float), gy (float), gz (float) [rad/s]
- **Examples:** Gyroscope in IMU, MEMS gyro chip
- **Common:** IMU-based heading estimation, rotation rate detection

#### linear_acceleration_3d
Reports linear acceleration along three axes (m/s²).
- **Reports:** ax (float), ay (float), az (float) [m/s²]
- **Examples:** Accelerometer in IMU, MEMS accel chip
- **Common:** Impact detection, orientation in gravity field, vibration sensing

#### magnetic_field_3d
Reports magnetic field vector (µT or mG).
- **Reports:** bx (float), by (float), bz (float) [µT]
- **Examples:** Magnetometer in IMU, standalone compass IC
- **Common:** Compass heading, local magnetic anomaly detection

#### temperature
Reports temperature (°C or °F).
- **Reports:** temp_c (float)
- **Examples:** Thermistor, DS18B20, temperature IC
- **Common:** Thermal monitoring, environmental sensing

#### pressure
Reports atmospheric or water pressure (Pa or hPa).
- **Reports:** pressure_pa (float)
- **Examples:** Barometric sensor, depth sensor
- **Common:** Altitude estimation, water depth measurement

### Sensors: Ranging and Vision

#### range_1d
Reports distance to nearest obstacle in one direction (m).
- **Reports:** distance_m (float)
- **Examples:** Ultrasonic sensor, infrared rangefinder, time-of-flight sensor
- **Common:** Obstacle avoidance, proximity detection, wall following

#### image_2d
Reports 2D pixel data (image frame).
- **Reports:** image_base64 (string), width (int), height (int), format (string)
- **Examples:** USB camera, CSI camera, smartphone camera
- **Common:** Visual navigation, object detection, visual odometry

## Special Capabilities

### can_bus_member
Indicates a node participates in direct CAN bus communication (see [09-can-bus-additions.md](../docs/09-can-bus-additions.md)).
- **Parameters:** bus_id (int), bit_rate (int, typically 1000000)
- **Data:** CAN frame exchange with specific frame IDs
- **Common:** Motor synchronization, real-time coordination

## Using the Ontology

### Registration Phase

During peripheral registration (see [05-registration-workflow.md](../docs/05-registration-workflow.md)), the human declares what capabilities the peripheral has:

> **AI:** What capabilities does this IMU have?
>
> **Human:** It's a 9-DOF IMU, so orientation, angular velocity, and acceleration. Also has a built-in thermometer.
>
> **AI:** So it advertises orientation_3d, angular_velocity_3d, linear_acceleration_3d, and temperature?
>
> **Human:** Yes, and it publishes magnetometer data too.
>
> **AI:** Adding magnetic_field_3d as well.

The declared capabilities are stored in the registration configuration and used for validation. If the peripheral fails to publish on the expected topics or the published data format does not match the ontology, the validation gate catches it.

### Part Library

The part library includes capability declarations for each part (see `part_library.json`). This enables Claude to propose compatible integrations based on prior experience:

> I see you're registering a BNO055. The last three integrations of this part published data at 5 Hz with these exact field names. Should I use the same approach?

### Role Discovery (Future)

When role discovery is implemented (Phase 2+, see [06-role-discovery.md](../docs/06-role-discovery.md)), the system will verify that observed behavior matches declared capabilities:

> Declared: `rotational_actuator` with left-wheel role
>
> Observed: Motor A produces +V translation when commanded forward, -V rotation when commanded forward
>
> Match: ✗ Inconsistent — a left wheel should produce +rotation (not -rotation) when commanded forward

This cross-check catches wiring errors or misunderstandings about what role a motor plays.

## Adding New Capabilities

New capabilities are added when:

1. A new hardware class arrives that doesn't fit existing capabilities.
2. The new capability is distinct enough to warrant its own entry (not just a combination of existing ones).
3. A project needs it and there's reason to expect other projects will too.

**Process:**

1. Engineer proposes the capability with:
   - Clear name and description
   - Data format (fields, types, units)
   - Representative examples
   - Expected use cases

2. Community reviews for:
   - Composability (does it play well with others?)
   - Distinctness (is this really a separate capability?)
   - Future-proofing (will this need to split or merge later?)

3. If approved, add to `part_library.json` under `capability_ontology`.

4. Commit to git with a clear message explaining the rationale.

Example new capability proposal:

```
capability_name: audio_input
description: Captures audio from a microphone or audio interface
data_reported:
  audio_samples_pcm: array of int16
  sample_rate_hz: int
  duration_ms: int
examples: [USB microphone, line-in interface, MEMS microphone]
use_cases: [sound level sensing, acoustic event detection, voice command input]
```

## Validation and Cross-Checking

### At Registration Time

When firmware is deployed to a new node, the dashboard validates that the node publishes on topics matching its declared capabilities:

```
node_id: "imu-01"
capabilities: ["orientation_3d", "angular_velocity_3d", "linear_acceleration_3d"]
expected_topics: [
  "nodes/imu-01/data/imu",    // or topic structure depends on firmware
]
```

If the firmware publishes `nodes/imu-01/data/imu` with fields `{qx, qy, qz, qw, gx, gy, gz, ax, ay, az}`, validation passes. If fields are missing or named differently, validation fails with a clear error.

### During Ongoing Operation

The system maintains a "capabilities catalog" for all active nodes. Conversations with Claude can reference this:

> I notice the motor on the left wheel reports encoder position but the right wheel doesn't. Are both motors the same hardware, or is the right motor not publishing its encoder data?

This enables humans to notice when declared capabilities diverge from actual behavior.

## Honest Limits

1. **Capabilities describe data, not semantics.** The ontology says what data is available; it does not interpret meaning. A `rotational_sensor` could be a wheel encoder or a potentiometer — same data, different physical meaning. Context (what the human says the sensor's role is) provides the semantic interpretation.

2. **New capabilities are not added lightly.** The ontology is intentionally closed. Adding a new capability affects every future registration conversation where Claude might reference it. Proliferating capabilities degrades the clarity and utility of the system.

3. **The ontology is not an API standard.** It defines **what** data is produced, not **how** it's transmitted. One IMU might publish compact binary format over MQTT; another might publish JSON. Both satisfy the `orientation_3d` capability; they just differ in wire format.

4. **Composition limits.** Some hardware produces data that almost fits a capability but doesn't quite. For example, a GPS that reports 2D position but in a non-standard latitude/longitude format. The pragmatic solution is to create an adapter layer in firmware that converts to standard format, rather than creating a new capability.

## Summary

The capability ontology provides a shared vocabulary for discussing what hardware can do, independent of specific implementation details. It enables the AI agent to learn from prior integrations, enables validation to catch misconfigurations, and provides a starting point for future automated reasoning about system composition and role discovery.


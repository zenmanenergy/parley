# Registration Workflow Implementation Guide

This guide documents the enhanced registration workflow for Parley, including automated context loading, part library integration, and spatial layout capture.

## Overview

Registration is the process by which a new peripheral joins the Parley system. The updated workflow includes:

1. **Automatic context suggestion** from the part library
2. **Spatial layout integration** to document mounting and orientation
3. **Capability validation** against the ontology
4. **Multi-node registration** support (planned)

## Prerequisites

The following files must exist in the project root:

- `part_library.json` - Database of previously-integrated hardware
- `layout.yaml` - Current robot spatial configuration
- `CAPABILITY_ONTOLOGY.md` - Documentation of capability definitions

The backend (`pi/dashboard/server.py`) automatically loads these files and injects relevant context into Claude conversations.

## Phase 1: Trigger and Initial Context

When a new ESP32 appears in factory mode, initiate a registration conversation:

```
Human: "Let's set up that new motor board."

AI: "Sure! I'll help register the new motor node. To get started:
    1. What type of peripheral is this? (sensor, actuator, etc.)
    2. What's the specific part? (manufacturer, model, breakout board if relevant)
    3. How is it wired to the ESP32?
    
    If this is a part we've integrated before, I can suggest the previous approach."
```

The AI automatically:
- Loads `part_library.json` and searches for relevant entries based on your description
- Loads `layout.yaml` to suggest where on the robot this might go
- Shows recent integrations of similar parts if they exist

## Phase 2: Research and Design

The conversation continues with technical decisions:

```
Human: "It's an MG996R servo."

AI: [Loads part_library, finds MG996R entry]
    "Great! We've integrated this servo 5 times before (last on 2026-05-20).
    Key reminders:
    - Requires separate 6V power supply
    - GPIO12 is the recommended PWM pin
    - Center position is 90 degrees
    
    Previous integrations used ESP32Servo library at 50Hz. Same approach?"
```

During design phase, layout information is incorporated:

```
Human: "Yes, same approach. It's a steering servo on the front."

AI: [Loads layout.yaml]
    "I see the robot has left/right wheels and is a differential drive.
    The steering servo will be a new addition.
    
    Questions for the layout:
    1. Where on the chassis is it mounted? (X/Y/Z relative to center)
    2. How is it oriented? (which axis does it rotate around?)
    3. Should I add it to the layout.yaml file?"
```

The AI proposes a layout entry:

```
AI: "Based on typical steering servo mounting, I'll suggest:
    
    steering_servo:
      type: rotational_actuator
      frame:
        parent: robot
        position: [0.15, 0.0, 0.08]    # 15 cm forward, 8 cm high
        rotation_axis: [0, 0, 1]       # Rotates around Z axis (yaw)
    
    Confirm or modify?"
```

## Phase 3: Firmware Generation

The AI writes firmware that includes capability declarations:

```cpp
// firmware/nodes/src/steering/main.cpp
#include "node_template.h"

void setup_peripheral() {
    servo.setPeriodHertz(50);
    servo.attach(12);  // GPIO12 for PWM
    servo.write(90);   // Center
    
    // Declare capabilities to the system
    node_publish_json("nodes/<id>/capabilities", {
        "capabilities": ["rotational_actuator"],
        "servo_range": [0, 180],
        "default_center": 90,
        "mounted_as": "steering"
    });
    
    node_subscribe("nodes/<id>/cmd/steering");
}

void loop_peripheral() {
    // Read and publish current angle
    int angle = servo.read();
    node_publish_json("nodes/<id>/data/servo", {
        "angle_deg": angle,
        "target_deg": target_angle
    });
}

bool peripheral_health_check() {
    // Verify servo is responding
    return servo.attached();
}

void peripheral_handle_command(const char* channel, JsonDocument& payload) {
    if (String(channel) == "steering") {
        int angle = payload["angle_deg"] | 90;
        angle = constrain(angle, 0, 180);
        servo.write(angle);
        target_angle = angle;
    }
}
```

## Phase 4: Flash and Validation

After successful compilation, firmware is pushed to the node:

```
AI: "Firmware compiled successfully. Pushing to the node...
    Validation gate checks:
    1. WiFi connection - ✓
    2. MQTT broker connection - ✓
    3. peripheral_health_check() - ✓ (servo attached)
    4. Capability publication - ✓ (capabilities topic updated)
    5. Functional verification - ? (please test)"
```

Functional verification is human-driven:

```
Human: "I commanded it to 45 degrees and the servo moved correctly."

AI: "Excellent! The servo is responding as expected. Moving to integration phase."
```

## Phase 5: Integration and Documentation

Final steps integrate the node:

### 1. Assign Node ID
```
AI: "Assigning node ID 'steering' to MAC address XX:XX:XX:XX:XX:XX"
Node will be known as: nodes/steering/...
```

### 2. Update Part Library
```json
{
  "id": "mg996r_servo",
  "last_integrated": "2026-06-06",
  "integration_count": 6,
  "task_refs": ["task-2026-06-06-steering-servo-registration"]
}
```

### 3. Update Layout
```yaml
peripherals:
  steering_servo:
    type: rotational_actuator
    # ... position and orientation from earlier ...
```

### 4. Generate Task Summary
```
REGISTRATION SUMMARY: MG996R Steering Servo

Objective: Integrate MG996R servo as steering actuator on differential drive robot

Hardware:
- Part: MG996R High-Torque Servo Motor
- Pin: GPIO12 (PWM)
- Power: 6V (separate supply)
- Role: Front steering on differential drive

Design Decisions:
- Used SparkFun ESP32Servo library (proven in 5 prior integrations)
- 50Hz PWM frequency (standard for servo)
- 0-180 degree range with 90-degree center
- Mounted 15cm forward, center height

Validation:
- WiFi: ✓
- MQTT: ✓
- Health check: ✓
- Functional: ✓ (tested 45° command)

Next: Calibrate steering response if needed; integrate path planning

Status: COMPLETE
Date: 2026-06-06
```

## Automatic Context Injection

The backend automatically provides context during conversations:

### Part Library Context
When the user mentions a part name or model, Claude receives:
```
## Relevant Parts from Library

### MG996R High-Torque Servo Motor
- Model: MG996R
- Communication: PWM
- Recommended library: ESP32Servo
- Key gotchas: Requires separate 6V supply, GND must be common with ESP32
- Last used: 2026-05-20 (integration count: 5)
```

### Layout Context
Claude automatically sees the current robot structure:
```
## Current Robot Layout
Robot: example_differential_drive
Registered peripherals: left_wheel, right_wheel, imu_01, gps_01
```

### Capability Ontology
When capabilities are mentioned, Claude has full ontology definitions available:
```
## Relevant Capabilities

rotational_actuator: Can apply torque to a rotating shaft
- Data received: angle_deg, speed_rpm, torque_nm
- Data published: angle_deg, speed_rpm, position_encoder_ticks
```

## Multi-Node Registration (Planned)

Future enhancement: register multiple similar nodes in a batch:

```
Human: "Register the left and right drive wheels (both MG6530 with encoders)"

AI: "I'll register both wheels together. For efficiency:
    1. Generate one set of firmware with node_id parameter
    2. Compile once, push to both nodes with different IDs
    3. Update layout with both wheels positioned symmetrically
    
    This should take 1/3 the time of registering them separately."
```

Implementation would add a `multi_node_registration` message type that:
- Generates firmware parameterized by node role/ID
- Compiles once with env variables
- Publishes to multiple OTA topics
- Updates layout entries in batch

## Best Practices

### 1. Declare Capabilities Clearly
Always include your hardware's capabilities during the conversation:
```
Human: "It's a BME680 sensor - temperature, humidity, pressure, and air quality."
```

### 2. Measure and Document Mounting
Provide physical measurements when possible:
```
Human: "The sensor is mounted 10 cm forward of center, 5 cm high, with no rotation."
```

### 3. Review Generated Code
Check the proposed firmware for correctness:
```
Human: "The WiFi initialization looks good, but change the publish interval to 2 Hz instead of 5 Hz."
```

### 4. Document Gotchas
Add any hardware-specific issues to your notes:
```
Human: "Add a note: this I2C device is sensitive to line noise - use short twisted-pair cables"
```

### 5. Keep Part Library Updated
After successful integration, let the AI update the library entry:
```
AI: "Should I update part_library.json to bump the integration_count for MG996R to 6?"
Human: "Yes, and add 'steering' to the usage notes."
```

## Troubleshooting Registration

### Issue: "Part not in library"

If a new part isn't in the library, the AI will research it:

```
AI: "This BMM150 magnetometer isn't in our library yet.
    I'll research the datasheet and integration approach.
    Should I add it to the library after this works?"
```

Add it by committing an update to `part_library.json` with full details for future reference.

### Issue: "Wiring mismatch detected"

If firmware publishes different data than expected:

```
AI: "The firmware publishes {x,y,z} but the part library entry expects {qx,qy,qz,qw}.
    Either:
    1. Correct the firmware to match the capability
    2. Redefine the capability if the firmware is correct
    
    Which should we do?"
```

### Issue: "Layout position uncertain"

If spatial mounting is ambiguous:

```
Human: "I'm not sure exactly where the IMU is mounted."

AI: "That's okay. Start with an approximate position (estimated, eye measurement).
    We can refine it later using role discovery calibration.
    For now, should I estimate based on typical IMU mounting?"
```

## Integration with Dashboard

The dashboard provides UI support for registration:

1. **Part Library Browser** - Search and filter parts by type, name, or capability
2. **Layout Visualization** - See mounting positions of all peripherals
3. **Capability Validator** - Verify that published data matches declared capabilities
4. **Registration Conversation** - AI-assisted conversation with part context injected
5. **Code Review** - Examine and approve generated firmware before compilation

## Summary

The enhanced registration workflow:
- Automatically suggests relevant prior integrations
- Captures spatial layout of the robot for kinematic reasoning
- Validates capabilities against the ontology
- Builds institutional knowledge in the part library over time
- Reduces the time to integrate similar hardware in the future

The process balances automation (research, design proposal, code generation) with human judgment (wiring verification, physical measurement, functional testing).

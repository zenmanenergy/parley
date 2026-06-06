# CAN Bus Configuration and Usage Guide

This guide explains how to configure and use CAN bus support in Parley nodes. Read [docs/09-can-bus-additions.md](docs/09-can-bus-additions.md) first for the architectural overview.

## Quick Start: Adding CAN to a Node

### 1. Hardware Setup

For a node that needs CAN, you will need:
- ESP32-S3 N16R8 (standard Parley board)
- Waveshare SN65HVD230 CAN transceiver breakout
- Twisted-pair wire for CANH/CANL bus lines
- 120-ohm termination resistors at bus ends

**Wiring to ESP32:**
```
Transceiver VCC  -> ESP32 3.3V
Transceiver GND  -> ESP32 GND
Transceiver CTX  -> ESP32 GPIO5 (CAN_TX_PIN)
Transceiver CRX  -> ESP32 GPIO4 (CAN_RX_PIN)
```

The transceiver's CANH and CANL pins connect to the shared CAN bus. On Waveshare breakouts, there's a jumper to enable/disable 120-ohm termination — enable it only on the two nodes at the physical ends of the bus.

### 2. Enable CAN in Firmware Configuration

During the registration conversation with Claude, when asked about CAN:

> **AI:** Does this node have a CAN transceiver connected?
>
> **You:** Yes, it's on the differential drive motor bus at 1 Mbps.

Claude will generate firmware that includes CAN support. The generated code will set the NVS flag `can_enabled` to `true`.

### 3. Verify CAN is Running

Once the firmware boots:
- Check the serial log for: `[CAN] TWAI initialized at 1Mbps`
- Check MQTT discovery message: `mosquitto_sub -t "system/discovery"` should show `"can":{"enabled":true,"bitrate":1000000,"bus_off":false}`
- Check diagnostics: `mosquitto_sub -t "nodes/<node_id>/data/can_diags"` should show `{"tx":0,"rx":0,"err":0,"bus_off":false}`

## Using CAN from Application Code

Once CAN is initialized, peripheral code can send and receive frames:

### Sending Frames

```cpp
// In your peripheral code:

void loop_peripheral() {
    // Check if CAN is available
    if (!node_has_can()) return;

    // Build a CAN frame with motor setpoint
    uint8_t frame_data[8];
    frame_data[0] = 0x42;  // Command: set velocity
    frame_data[1] = 0x00;
    // ... populate rest of frame

    // Send it at CAN ID 0x101
    bool sent = node_send_can_frame(0x101, frame_data, 8);
    if (!sent) {
        node_log(LogLevel::WARN, "CAN transmit failed");
    }
}
```

### Receiving Frames

If your peripheral needs to receive frames from peer motors, implement the optional callback:

```cpp
// In your peripheral code (e.g., motor_left.cpp):

// This hook is called whenever a CAN frame arrives
void peripheral_handle_can_frame(uint32_t can_id, const uint8_t* data, size_t len) {
    if (can_id == 0x102) {
        // This is the right motor's velocity feedback
        int16_t right_velocity = (int16_t)((data[0] << 8) | data[1]);
        
        // Use it to adjust our motor control
        adjust_motor_for_sync(right_velocity);
        
        node_log(LogLevel::DEBUG, "Right motor: %d", right_velocity);
    }
}
```

The template automatically:
- Checks for incoming CAN frames once per loop iteration
- Calls your `peripheral_handle_can_frame()` for each frame
- Manages the RX queue (10 frames max before overrun)
- Publishes diagnostics every 30 seconds

## Monitoring and Diagnostics

### Real-Time Diagnostics

Every 30 seconds, the node publishes CAN diagnostics to:
```
nodes/<node_id>/data/can_diags
```

Example output:
```json
{"tx":247,"rx":189,"err":0,"bus_off":false}
```

- `tx`: Total frames transmitted since boot
- `rx`: Total frames received since boot  
- `err`: Total error conditions detected (ALERT_BUS_ERROR, ALERT_ERR_PASS)
- `bus_off`: True if bus went offline; template auto-recovers

### Checking Bus Status

From the dashboard or command line:
```bash
# Watch CAN diagnostics in real-time
mosquitto_sub -t "nodes/motor_left/data/can_diags"
```

### Logs

CAN events are logged to LittleFS:
```
[DEBUG] CAN disabled (no transceiver configured)
[INFO]  CAN TWAI initialized at 1Mbps
[WARN]  CAN Bus error detected (total: 1)
[ERROR] CAN Bus went offline; attempting recovery
```

## Troubleshooting

### "Bus error detected" or "Bus went offline"

**Symptoms:** Frequent error messages, `bus_off` = true in diagnostics

**Check:**
1. Verify wiring: GPIO4/GPIO5 to transceiver, CANH/CANL to bus
2. Verify termination: 120-ohm resistors at both ends of the bus only
3. Check for loose connections or shorted signals
4. Verify transceiver is powered (3.3V)
5. Look for electrical noise: move CAN wires away from power/motor lines

**Recovery:** The template automatically attempts bus restart. If errors persist, the node logs are written to LittleFS and can be retrieved for analysis.

### No frames received despite sending

**Check:**
1. Verify the CAN ID is correct (matches what the receiving node is listening for)
2. Check that `peripheral_handle_can_frame()` is defined in your code
3. Verify both nodes are on the same CAN bus with same bit rate (1 Mbps)
4. Monitor RX diagnostics: if `rx` is 0, the receiving node is not getting frames

### Transceiver not detected at boot

If the firmware boots without CAN even though you wired it:
- The NVS flag `can_enabled` is not set
- Re-register the node, and when asked about CAN, say yes
- The generated firmware will set the flag and initialize on next reboot

## Multiple CAN Buses

If your robot has more than one coordination domain (e.g., separate buses for drive wheels and arm joints), you need to:

1. Add a second transceiver to the nodes involved
2. Each ESP32 has only one TWAI peripheral, so a node can be on only one bus
3. Nodes on different buses coordinate through MQTT if needed
4. Register each node separately, specifying which bus it's on

The template will use a single transceiver; multi-bus scenarios require custom firmware or multiple nodes.

## Example: Differential Drive Synchronization

A two-motor differential drive needs the wheels to stay in sync:

**Left motor node (motor_left.cpp):**
```cpp
void loop_peripheral() {
    // Run left motor controller
    update_left_motor();
    
    // Get right motor's velocity for feedback
    // (comes via CAN frame from right node)
    int16_t right_velocity = get_latest_can_right_velocity();
    
    // Adjust our control to match
    if (left_error > right_velocity + tolerance) {
        reduce_left_speed();
    }
}

void peripheral_handle_can_frame(uint32_t can_id, const uint8_t* data, size_t len) {
    if (can_id == 0x201) {  // Right motor velocity frame
        right_velocity = (int16_t)((data[0] << 8) | data[1]);
    }
}
```

**Right motor node (motor_right.cpp):**
```cpp
void loop_peripheral() {
    update_right_motor();
    
    // Publish our velocity to left motor
    int16_t my_velocity = get_motor_velocity();
    uint8_t frame[8];
    frame[0] = (my_velocity >> 8) & 0xFF;
    frame[1] = my_velocity & 0xFF;
    node_send_can_frame(0x201, frame, 2);
}
```

Both nodes also publish to MQTT for logging and high-level commands, but **only** use CAN for the real-time wheel synchronization loop.

## Advanced: Configuring CAN Parameters

To change the CAN bit rate or GPIO pins, edit `node_template.cpp`:

```cpp
#define CAN_RX_PIN   GPIO_NUM_4   // Change to different GPIO if 4 is needed
#define CAN_TX_PIN   GPIO_NUM_5   // Change to different GPIO if 5 is needed
#define CAN_BITRATE  1000000      // Change to 500000 or other valid rate
#define CAN_RX_QUEUE 10           // Increase if frames are being dropped
```

Recompile and push via OTA.

## References

- [09-can-bus-additions.md](docs/09-can-bus-additions.md) — Architectural overview and design rationale
- [02-node-firmware-design.md](docs/02-node-firmware-design.md) — Firmware template structure
- [05-registration-workflow.md](docs/05-registration-workflow.md) — How to register a node with CAN

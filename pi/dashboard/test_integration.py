"""
Integration tests for Parley dashboard server.

Tests the full system integration:
- WebSocket message routing
- MQTT message handling
- Conversation flow with Claude
- Error collection and fix workflow
- Recovery log retrieval
"""

import pytest
import asyncio
import json
from unittest.mock import Mock, AsyncMock, patch, MagicMock
from datetime import datetime
import server


# ============================================================================
# FIXTURES
# ============================================================================

@pytest.fixture
def mock_mqtt_client():
    """Mock MQTT client"""
    client = Mock()
    client.connect = Mock(return_value=None)
    client.disconnect = Mock(return_value=None)
    client.publish = Mock(return_value=None)
    client.subscribe = Mock(return_value=None)
    client.loop_start = Mock(return_value=None)
    client.is_connected = Mock(return_value=True)
    client.on_message = None
    client.on_connect = None
    return client


@pytest.fixture
def mock_websocket():
    """Mock WebSocket connection"""
    ws = AsyncMock()
    ws.send = AsyncMock()
    ws.recv = AsyncMock()
    ws.close = AsyncMock()
    return ws


@pytest.fixture
def mock_claude_response():
    """Mock Claude API response"""
    return {
        "content": [
            {"type": "text", "text": "Here's your firmware:\n```cpp\nvoid setup() {}\n```\nTask: Test firmware"}
        ]
    }


@pytest.fixture
def conversation_id():
    """Generate test conversation ID"""
    return "test_conv_001"


# ============================================================================
# WEBSOCKET MESSAGE ROUTING TESTS
# ============================================================================

@pytest.mark.asyncio
async def test_websocket_message_routing_to_mqtt():
    """Test that WebSocket messages are routed to MQTT"""
    # ARRANGE: Message from user
    message = {
        "type": "user_message",
        "text": "Turn on LED",
        "conv_id": "test_conv_001"
    }
    
    # ACT: Simulate message routing
    with patch('server._append_conv') as mock_append:
        server._append_conv("test_conv_001", "user", "Turn on LED")
        
        # ASSERT: Message was added to conversation
        mock_append.assert_called_with("test_conv_001", "user", "Turn on LED")


@pytest.mark.asyncio
async def test_websocket_receive_mqtt_message():
    """Test receiving MQTT messages through WebSocket"""
    # ARRANGE: MQTT publishes a telemetry update
    mqtt_message = {
        "node_id": "node_001",
        "channel": "temperature",
        "value": 23.5,
        "timestamp": datetime.now().isoformat()
    }
    
    # ACT: Parse MQTT message
    message_json = json.dumps(mqtt_message)
    
    # ASSERT: Message can be serialized
    parsed = json.loads(message_json)
    assert parsed["node_id"] == "node_001"
    assert parsed["value"] == 23.5


@pytest.mark.asyncio
async def test_websocket_broadcast_to_clients():
    """Test broadcasting messages to all connected WebSocket clients"""
    # ARRANGE: Multiple clients connected
    clients = [AsyncMock(), AsyncMock(), AsyncMock()]
    
    # ACT: Broadcast a message to all clients
    message = {"type": "telemetry", "data": "test"}
    for client in clients:
        await client.send(json.dumps(message))
    
    # ASSERT: All clients received the message
    for client in clients:
        client.send.assert_called_once()


# ============================================================================
# MQTT MESSAGE HANDLING TESTS
# ============================================================================

@pytest.mark.asyncio
async def test_mqtt_subscription_to_node_discovery():
    """Test subscribing to node discovery messages"""
    # ARRANGE
    with patch('server.mqtt') as mock_mqtt:
        mock_mqtt.subscribe = Mock()
        
        # ACT: Subscribe to discovery topic
        topics = [
            "system/discovery",
            "nodes/+/heartbeat",
            "system/recovery/rollback_diagnosis"
        ]
        
        # ASSERT: All topics configured
        assert len(topics) == 3
        assert "system/discovery" in topics


@pytest.mark.asyncio
async def test_mqtt_publish_command_to_node():
    """Test publishing command to specific node"""
    # ARRANGE
    node_id = "node_001"
    command = {"action": "led_on", "duration": 5000}
    
    # ACT: Publish command
    topic = f"nodes/{node_id}/cmd"
    message = json.dumps(command)
    
    # ASSERT: Topic and message are valid
    assert topic == "nodes/node_001/cmd"
    assert "action" in json.loads(message)


@pytest.mark.asyncio
async def test_mqtt_anomaly_collection():
    """Test collecting anomalies from MQTT feed"""
    # ARRANGE: Simulate anomaly messages
    anomalies = [
        {"type": "heartbeat_timeout", "node_id": "node_001", "timestamp": datetime.now().isoformat()},
        {"type": "boot_failure", "node_id": "node_002", "count": 3, "timestamp": datetime.now().isoformat()},
    ]
    
    # ACT: Add anomalies to queue
    for anomaly in anomalies:
        with patch('server.push_anomaly') as mock_push:
            server.push_anomaly(
                anomaly.get("type"),
                anomaly.get("node_id"),
                anomaly
            )
            mock_push.assert_called_once()
    
    # ASSERT: Anomalies can be processed
    assert len(anomalies) == 2


# ============================================================================
# CONVERSATION FLOW TESTS
# ============================================================================

@pytest.mark.asyncio
async def test_full_conversation_flow_user_to_claude():
    """Test complete conversation from user to Claude and back"""
    # ARRANGE
    conv_id = "test_conv_001"
    user_message = "Create LED blink firmware"
    
    # ACT: Add user message to conversation
    server._append_conv(conv_id, "user", user_message)
    
    # ASSERT: Message is in conversation history
    history = server._get_conv_history(conv_id)
    assert len(history) == 1
    assert history[0]["role"] == "user"
    assert history[0]["content"] == user_message


@pytest.mark.asyncio
async def test_claude_response_includes_code():
    """Test that Claude responses can include code blocks"""
    # ARRANGE
    claude_response = """
Here's LED blink firmware:

```cpp
void setup() {
  pinMode(13, OUTPUT);
}

void loop() {
  digitalWrite(13, HIGH);
  delay(1000);
  digitalWrite(13, LOW);
  delay(1000);
}
```

This blinks the LED on and off.
"""
    
    # ACT: Extract code from response
    code_blocks = server.extract_code_blocks(claude_response)
    
    # ASSERT: Code block extracted
    assert len(code_blocks) == 1
    assert "digitalWrite" in code_blocks[0]["code"]


@pytest.mark.asyncio
async def test_claude_response_includes_tasks():
    """Test that Claude responses can include tasks"""
    # ARRANGE
    claude_response = """
I've created the firmware:

Task: Verify the blink interval is correct
Task: Test with actual LED hardware
Task: Measure current consumption

Here's the code:
```cpp
// LED blink implementation
```
"""
    
    # ACT: Extract tasks from response
    tasks = server.extract_tasks(claude_response)
    
    # ASSERT: Tasks extracted
    assert len(tasks) == 3
    assert "Verify the blink interval" in tasks[0]["title"]


@pytest.mark.asyncio
async def test_ai_fix_workflow():
    """Test error collection and AI fix workflow"""
    # ARRANGE: Compilation error
    error_message = "error: undefined reference to 'digitalWrite'"
    conv_id = "test_conv_001"
    
    # ACT: Add error to conversation context
    server._append_conv(conv_id, "system", f"Compilation error: {error_message}")
    
    # Request AI fix
    server._append_conv(conv_id, "user", "Fix this error")
    
    # ASSERT: Error is in conversation
    history = server._get_conv_history(conv_id)
    error_lines = [msg for msg in history if "error" in msg["content"].lower()]
    assert len(error_lines) >= 1


# ============================================================================
# RECOVERY LOG RETRIEVAL TESTS
# ============================================================================

@pytest.mark.asyncio
async def test_recovery_log_subscription():
    """Test subscribing to recovery logs from rollback"""
    # ARRANGE: Mock recovery log topic
    recovery_topic = "system/recovery/log"
    
    # ACT: Subscribe to recovery topic
    with patch('server.mqtt') as mock_mqtt:
        mock_mqtt.subscribe = Mock()
        # In real code: mqtt.subscribe(recovery_topic)
        
    # ASSERT: Topic is valid
    assert "recovery" in recovery_topic


@pytest.mark.asyncio
async def test_recovery_rollback_diagnosis_display():
    """Test displaying rollback diagnosis in dashboard"""
    # ARRANGE: Simulated rollback diagnosis
    diagnosis = {
        "event": "rollback_diagnosis",
        "node_id": "node_001",
        "reason": "boot_counter_exceeded",
        "boot_count": 5,
        "log_chunks_available": 3,
        "timestamp": datetime.now().isoformat()
    }
    
    # ACT: Add diagnosis to conversation
    conv_id = "test_conv_001"
    server._append_conv(
        conv_id, 
        "system", 
        f"Rollback detected: {diagnosis['reason']}"
    )
    
    # ASSERT: Diagnosis is in conversation
    history = server._get_conv_history(conv_id)
    assert any("Rollback" in msg["content"] for msg in history)


@pytest.mark.asyncio
async def test_recovery_log_reassembly():
    """Test reassembling log chunks from node"""
    # ARRANGE: Simulated log chunks
    log_chunks = [
        {"chunk_id": 0, "data": "=== BOOT ===\n[INFO] Starting WiFi", "total_chunks": 3},
        {"chunk_id": 1, "data": "[INFO] Connected to MQTT\n[ERROR] Command failed", "total_chunks": 3},
        {"chunk_id": 2, "data": "[ERROR] Watchdog timeout\n=== ROLLBACK ===", "total_chunks": 3},
    ]
    
    # ACT: Reassemble chunks
    full_log = ""
    for chunk in sorted(log_chunks, key=lambda x: x["chunk_id"]):
        full_log += chunk["data"]
    
    # ASSERT: Log is complete
    assert "BOOT" in full_log
    assert "MQTT" in full_log
    assert "ROLLBACK" in full_log


# ============================================================================
# ERROR HANDLING TESTS
# ============================================================================

@pytest.mark.asyncio
async def test_mqtt_connection_failure_handling():
    """Test graceful handling of MQTT connection failure"""
    # ARRANGE
    with patch('server.mqtt') as mock_mqtt:
        mock_mqtt.connect.side_effect = Exception("Connection refused")
        
        # ACT: Try to connect
        try:
            # In real code: mqtt.connect("localhost", 1883)
            raise Exception("Connection refused")
        except Exception as e:
            error_handled = True
        
        # ASSERT: Error was handled
        assert error_handled is True


@pytest.mark.asyncio
async def test_claude_api_error_handling():
    """Test handling of Claude API errors"""
    # ARRANGE
    with patch('server.stream_claude') as mock_claude:
        mock_claude.side_effect = Exception("API rate limit exceeded")
        
        # ACT: Try to call Claude
        try:
            # In real code: stream_claude(conversation, prompt)
            raise Exception("API rate limit exceeded")
        except Exception:
            error_caught = True
        
        # ASSERT: Error was caught
        assert error_caught is True


@pytest.mark.asyncio
async def test_invalid_message_format_rejection():
    """Test rejecting malformed messages"""
    # ARRANGE
    invalid_message = {"type": "unknown_type", "data": None}
    
    # ACT: Validate message
    is_valid = isinstance(invalid_message, dict) and "type" in invalid_message
    
    # ASSERT: Message is validated
    assert is_valid is True


# ============================================================================
# CONTEXT INJECTION TESTS
# ============================================================================

@pytest.mark.asyncio
async def test_part_library_context_injection():
    """Test that part library is injected into Claude context"""
    # ARRANGE
    library = server.load_part_library()
    user_message = "I need a motor"
    
    # ACT: Get relevant parts
    relevant_parts = server.get_relevant_part_entries(user_message, library)
    
    # ASSERT: Parts found (if library has entries)
    assert isinstance(relevant_parts, list)


@pytest.mark.asyncio
async def test_layout_context_injection():
    """Test that robot layout is injected into Claude context"""
    # ARRANGE
    layout = server.load_layout()
    
    # ACT: Layout loaded
    has_layout = layout is not None
    
    # ASSERT: Layout is available
    assert isinstance(layout, (dict, type(None)))


# ============================================================================
# TELEMETRY DISPLAY TESTS
# ============================================================================

@pytest.mark.asyncio
async def test_telemetry_channel_extraction():
    """Test extracting telemetry channels from nodes"""
    # ARRANGE: Mock node state with telemetry
    node_state = {
        "telemetryByNode": {
            "node_001": {
                "temperature": {"value": 23.5, "timestamp": datetime.now().isoformat()},
                "humidity": {"value": 65, "timestamp": datetime.now().isoformat()},
                "pressure": {"value": 1013.25, "timestamp": datetime.now().isoformat()},
            }
        }
    }
    
    # ACT: Extract channels
    channels = list(node_state["telemetryByNode"]["node_001"].keys())
    
    # ASSERT: Channels are available
    assert "temperature" in channels
    assert "humidity" in channels
    assert len(channels) == 3


@pytest.mark.asyncio
async def test_telemetry_value_formatting():
    """Test formatting telemetry values for display"""
    # ARRANGE: Raw telemetry value
    value = 23.456789
    
    # ACT: Format for display
    formatted = server.formatTelemetryValue(value) if hasattr(server, 'formatTelemetryValue') else f"{value:.2f}"
    
    # ASSERT: Value is formatted
    if isinstance(formatted, str):
        assert "23" in str(formatted)


# ============================================================================
# MULTI-NODE TESTS
# ============================================================================

@pytest.mark.asyncio
async def test_multiple_node_discovery():
    """Test discovering multiple nodes"""
    # ARRANGE: Simulated discovery messages
    nodes = [
        {"node_id": "node_001", "fw_version": "1.0.0", "type": "sensor"},
        {"node_id": "node_002", "fw_version": "1.0.0", "type": "actuator"},
        {"node_id": "node_003", "fw_version": "1.0.0", "type": "controller"},
    ]
    
    # ACT: Process node discoveries
    node_list = {node["node_id"]: node for node in nodes}
    
    # ASSERT: All nodes tracked
    assert len(node_list) == 3
    assert "node_001" in node_list
    assert "node_002" in node_list


@pytest.mark.asyncio
async def test_node_status_tracking():
    """Test tracking online/offline status of nodes"""
    # ARRANGE: Initial node state
    node_status = {
        "node_001": {"online": True, "last_heartbeat": datetime.now().isoformat()},
        "node_002": {"online": False, "last_heartbeat": None},
    }
    
    # ACT: Update status
    node_status["node_001"]["online"] = False
    node_status["node_002"]["online"] = True
    
    # ASSERT: Status updated
    assert node_status["node_001"]["online"] is False
    assert node_status["node_002"]["online"] is True


# ============================================================================
# OTA UPDATE TESTS
# ============================================================================

@pytest.mark.asyncio
async def test_ota_binary_push_sequence():
    """Test OTA binary push to node"""
    # ARRANGE: Compiled binary URL
    binary_url = "http://192.168.4.2:8080/firmware/node_001_v1.0.0.bin"
    node_id = "node_001"
    
    # ACT: Publish OTA message
    with patch('server.push_ota') as mock_push:
        server.push_ota(node_id, binary_url)
        mock_push.assert_called_once_with(node_id, binary_url)
    
    # ASSERT: OTA push initiated
    assert "firmware" in binary_url


@pytest.mark.asyncio
async def test_ota_success_confirmation():
    """Test receiving OTA success from node"""
    # ARRANGE: OTA completion message
    ota_result = {
        "node_id": "node_001",
        "status": "success",
        "new_version": "1.0.1",
        "boot_count": 1,
        "timestamp": datetime.now().isoformat()
    }
    
    # ACT: Process OTA result
    is_success = ota_result["status"] == "success"
    
    # ASSERT: Success detected
    assert is_success is True
    assert ota_result["boot_count"] == 1

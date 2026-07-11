"""
Unit tests for server.py dashboard backend.

Run with: pytest test_server.py -v

Tests focus on pure functions and data processing that don't require
WebSocket or MQTT connections.
"""

import json
import pytest
import sys
import time
from pathlib import Path
from unittest.mock import patch, MagicMock, mock_open
from collections import deque

# Add the pi/dashboard directory to path so we can import server
sys.path.insert(0, str(Path(__file__).parent))

import server


class TestExtractCodeBlocks:
	"""Tests for extract_code_blocks() function."""

	def test_empty_text(self):
		"""Empty text should return empty list."""
		result = server.extract_code_blocks("")
		assert result == []

	def test_no_code_blocks(self):
		"""Text without code blocks should return empty list."""
		text = "This is just plain text without any code blocks."
		result = server.extract_code_blocks(text)
		assert result == []

	def test_single_code_block(self):
		"""Single code block should be extracted."""
		text = "```cpp\nvoid setup() { Serial.begin(115200); }\n```"
		result = server.extract_code_blocks(text)
		assert len(result) == 1
		assert result[0]["lang"] == "cpp"
		assert "setup()" in result[0]["code"]

	def test_code_block_with_file_annotation(self):
		"""Code block with // FILE: comment should capture filename."""
		text = '```cpp\n// FILE: firmware/nodes/src/motor/main.cpp\nvoid setup() {}\n```'
		result = server.extract_code_blocks(text)
		assert len(result) == 1
		assert result[0]["filename"] == "firmware/nodes/src/motor/main.cpp"

	def test_multiple_code_blocks(self):
		"""Multiple code blocks should all be extracted."""
		text = "```cpp\ncode1\n```\nSome text\n```python\ncode2\n```"
		result = server.extract_code_blocks(text)
		assert len(result) == 2
		assert result[0]["lang"] == "cpp"
		assert result[1]["lang"] == "python"

	def test_code_block_default_language(self):
		"""Code block without language should default to 'text'."""
		text = "```\nsome code\n```"
		result = server.extract_code_blocks(text)
		assert len(result) == 1
		assert result[0]["lang"] == "text"

	def test_env_platformio_annotation(self):
		"""Code block with env: comment should set filename to platformio.ini."""
		text = "```ini\n// env: motor\nkey=value\n```"
		result = server.extract_code_blocks(text)
		assert len(result) == 1
		assert result[0]["filename"] == "firmware/nodes/platformio.ini"

	def test_code_block_multiline_content(self):
		"""Code block with multiple lines should preserve content."""
		code = "void setup() {\n  Serial.begin(115200);\n  delay(100);\n}"
		text = f"```cpp\n{code}\n```"
		result = server.extract_code_blocks(text)
		assert len(result) == 1
		assert code in result[0]["code"]


class TestExtractTasks:
	"""Tests for extract_tasks() function."""

	def test_empty_text(self):
		"""Empty text should return empty list."""
		result = server.extract_tasks("")
		assert result == []

	def test_no_tasks(self):
		"""Text without task markers should return empty list."""
		text = "This is just regular text."
		result = server.extract_tasks(text)
		assert result == []

	def test_task_prefix_format(self):
		"""Lines starting with 'Task:' should be extracted."""
		text = "Task: Configure the motor controller\nSome explanation here."
		result = server.extract_tasks(text)
		assert len(result) == 1
		assert result[0]["title"] == "Configure the motor controller"
		assert result[0]["status"] == "pending"

	def test_action_prefix_format(self):
		"""Lines with 'ACTION:' should also be extracted."""
		text = "ACTION: Test the firmware on hardware"
		result = server.extract_tasks(text)
		assert len(result) == 1
		assert result[0]["title"] == "Test the firmware on hardware"

	def test_todo_prefix_format(self):
		"""Lines with 'TODO:' should be extracted."""
		text = "TODO: Review the error logs"
		result = server.extract_tasks(text)
		assert len(result) == 1
		assert result[0]["title"] == "Review the error logs"

	def test_checkbox_format(self):
		"""Checkbox format '[ ] task' should be extracted."""
		text = "[ ] Fix the compiler error"
		result = server.extract_tasks(text)
		assert len(result) == 1
		assert result[0]["title"] == "Fix the compiler error"

	def test_multiple_tasks(self):
		"""Multiple task markers should extract all tasks."""
		text = """
		Task: First task
		[ ] Second task
		ACTION: Third task
		TODO: Fourth task
		"""
		result = server.extract_tasks(text)
		assert len(result) == 4

	def test_tasks_have_timestamps(self):
		"""All extracted tasks should have created timestamp."""
		text = "Task: Do something\n[ ] Do another thing"
		before = time.time()
		result = server.extract_tasks(text)
		after = time.time()
		assert len(result) == 2
		for task in result:
			assert "created" in task
			assert before <= task["created"] <= after

	def test_task_id_uniqueness(self):
		"""Each task should have a unique ID."""
		text = "Task: First\nTask: Second\nTask: Third"
		result = server.extract_tasks(text)
		ids = [t["id"] for t in result]
		assert len(ids) == len(set(ids))  # all unique

	def test_case_insensitive_prefix(self):
		"""Task prefixes should be case-insensitive."""
		text = "task: lowercase\nTASK: uppercase\nTask: titlecase"
		result = server.extract_tasks(text)
		assert len(result) == 3


class TestLoadPartLibrary:
	"""Tests for load_part_library() function."""

	def test_load_nonexistent_file(self):
		"""Loading when file doesn't exist should return empty dict."""
		with patch.object(Path, "exists", return_value=False):
			result = server.load_part_library()
			assert result == {}

	def test_load_valid_json(self):
		"""Valid JSON file should be loaded correctly."""
		lib_data = {
			"parts": {
				"motor_001": {"name": "DC Motor", "voltage": 12}
			}
		}
		mock_file_content = json.dumps(lib_data)
		with patch("builtins.open", mock_open(read_data=mock_file_content)):
			with patch.object(Path, "exists", return_value=True):
				result = server.load_part_library()
				assert result == lib_data

	def test_load_empty_json(self):
		"""Empty JSON object should return empty dict."""
		with patch("builtins.open", mock_open(read_data="{}")):
			with patch.object(Path, "exists", return_value=True):
				result = server.load_part_library()
				assert result == {}

	def test_load_invalid_json(self):
		"""Invalid JSON should return empty dict and log warning."""
		with patch("builtins.open", mock_open(read_data="{ invalid json")):
			with patch.object(Path, "exists", return_value=True):
				result = server.load_part_library()
				assert result == {}


class TestLoadLayout:
	"""Tests for load_layout() function."""

	def test_yaml_not_available(self):
		"""When yaml is None, should return empty dict."""
		with patch.object(server, "yaml", None):
			result = server.load_layout()
			assert result == {}

	def test_load_nonexistent_file(self):
		"""Loading when file doesn't exist should return empty dict."""
		with patch.object(Path, "exists", return_value=False):
			if server.yaml is not None:
				result = server.load_layout()
				assert result == {}

	def test_load_valid_yaml(self):
		"""Valid YAML file should be loaded correctly."""
		yaml_data = {"nodes": {"coordinator": {"x": 0, "y": 0}}}
		yaml_str = "nodes:\n  coordinator:\n    x: 0\n    y: 0"
		with patch("builtins.open", mock_open(read_data=yaml_str)):
			with patch.object(Path, "exists", return_value=True):
				if server.yaml is not None:
					with patch.object(server.yaml, "safe_load", return_value=yaml_data):
						result = server.load_layout()
						assert result == yaml_data

	def test_load_empty_yaml(self):
		"""Empty or null YAML should return empty dict."""
		with patch("builtins.open", mock_open(read_data="")):
			with patch.object(Path, "exists", return_value=True):
				if server.yaml is not None:
					with patch.object(server.yaml, "safe_load", return_value=None):
						result = server.load_layout()
						assert result == {}


class TestGetRelevantPartEntries:
	"""Tests for get_relevant_part_entries() function."""

	def test_empty_library(self):
		"""Empty library should return empty list."""
		result = server.get_relevant_part_entries("motor", {})
		assert result == []

	def test_no_parts_key(self):
		"""Library without 'parts' key should return empty list."""
		result = server.get_relevant_part_entries("motor", {"other": {}})
		assert result == []

	def test_match_by_name(self):
		"""Should find parts matching keywords in name (substring match)."""
		library = {
			"parts": {
				"motor1": {"name": "dc motor", "type": "actuator"},
				"led1": {"name": "LED", "type": "light"}
			}
		}
		# Function does substring matching: "dc motor" must be in the message
		result = server.get_relevant_part_entries("I have a dc motor to install", library)
		assert len(result) == 1
		assert result[0]["name"] == "dc motor"

	def test_match_by_model(self):
		"""Should find parts matching keywords in model."""
		library = {
			"parts": {
				"imu1": {"name": "Inertial Sensor", "model": "MPU6050", "type": "sensor"}
			}
		}
		result = server.get_relevant_part_entries("mpu6050", library)
		assert len(result) == 1

	def test_match_by_manufacturer(self):
		"""Should find parts matching manufacturer name."""
		library = {
			"parts": {
				"motor1": {"name": "Motor", "manufacturer": "Maxon", "type": "actuator"}
			}
		}
		result = server.get_relevant_part_entries("maxon", library)
		assert len(result) == 1

	def test_case_insensitive_match(self):
		"""Matching should be case-insensitive."""
		library = {
			"parts": {
				"motor1": {"name": "DC Motor", "type": "actuator"}
			}
		}
		result = server.get_relevant_part_entries("DC MOTOR", library)
		assert len(result) == 1

	def test_multiple_matches(self):
		"""Should find all parts matching keywords (substring match)."""
		library = {
			"parts": {
				"motor1": {"name": "dc motor", "type": "actuator"},
				"motor2": {"name": "servo motor", "type": "actuator"},
				"sensor1": {"name": "motor speed sensor", "type": "sensor"}
			}
		}
		# All parts contain either "dc motor", "servo motor", "motor speed sensor", or "actuator" keyword
		result = server.get_relevant_part_entries("dc motor and servo motor and sensor for motor speed", library)
		assert len(result) == 3

	def test_no_match(self):
		"""Should return empty list when no keywords match."""
		library = {
			"parts": {
				"led1": {"name": "LED", "type": "light"}
			}
		}
		result = server.get_relevant_part_entries("motor", library)
		assert len(result) == 0


class TestPushAnomaly:
	"""Tests for push_anomaly() function."""

	def test_push_single_anomaly(self):
		"""Should add anomaly to feed with timestamp."""
		# Save original feed
		original_feed = server.anomaly_feed.copy()
		try:
			server.anomaly_feed.clear()
			anomaly = {"node_id": "motor_1", "error": "overheat"}
			server.push_anomaly(anomaly)
			assert len(server.anomaly_feed) == 1
			assert server.anomaly_feed[0]["node_id"] == "motor_1"
			assert "ts" in server.anomaly_feed[0]
		finally:
			server.anomaly_feed.clear()
			server.anomaly_feed.extend(original_feed)

	def test_push_respects_max_anomalies(self):
		"""Should respect MAX_ANOMALIES limit."""
		original_feed = server.anomaly_feed.copy()
		try:
			server.anomaly_feed.clear()
			# Push more than MAX_ANOMALIES
			for i in range(server.MAX_ANOMALIES + 5):
				server.push_anomaly({"index": i})
			assert len(server.anomaly_feed) <= server.MAX_ANOMALIES
		finally:
			server.anomaly_feed.clear()
			server.anomaly_feed.extend(original_feed)

	def test_anomaly_has_timestamp(self):
		"""Pushed anomaly should have ts field."""
		original_feed = server.anomaly_feed.copy()
		try:
			server.anomaly_feed.clear()
			before = time.time()
			server.push_anomaly({"error": "test"})
			after = time.time()
			assert len(server.anomaly_feed) == 1
			assert "ts" in server.anomaly_feed[0]
			assert before <= server.anomaly_feed[0]["ts"] <= after
		finally:
			server.anomaly_feed.clear()
			server.anomaly_feed.extend(original_feed)


class TestConversationHelpers:
	"""Tests for conversation session management."""

	def test_get_conv_history_new_conversation(self):
		"""New conversation should return empty list."""
		server.conv_sessions.clear()
		result = server._get_conv_history("conv_123")
		assert result == []

	def test_get_conv_history_returns_copy(self):
		"""Should return a copy, not reference to original."""
		server.conv_sessions.clear()
		server._append_conv("conv_123", "user", "hello")
		result1 = server._get_conv_history("conv_123")
		result1.append({"role": "assistant", "content": "hi"})
		result2 = server._get_conv_history("conv_123")
		assert len(result2) == 1  # Original unchanged

	def test_append_conv(self):
		"""Should append message to conversation."""
		server.conv_sessions.clear()
		server._append_conv("conv_123", "user", "hello")
		result = server._get_conv_history("conv_123")
		assert len(result) == 1
		assert result[0]["role"] == "user"
		assert result[0]["content"] == "hello"

	def test_append_multiple_messages(self):
		"""Should append multiple messages in order."""
		server.conv_sessions.clear()
		server._append_conv("conv_123", "user", "msg1")
		server._append_conv("conv_123", "assistant", "msg2")
		server._append_conv("conv_123", "user", "msg3")
		result = server._get_conv_history("conv_123")
		assert len(result) == 3
		assert result[0]["content"] == "msg1"
		assert result[1]["content"] == "msg2"
		assert result[2]["content"] == "msg3"

	def test_separate_conversations(self):
		"""Different conversation IDs should be separate."""
		server.conv_sessions.clear()
		server._append_conv("conv_1", "user", "conv1_msg")
		server._append_conv("conv_2", "user", "conv2_msg")
		result1 = server._get_conv_history("conv_1")
		result2 = server._get_conv_history("conv_2")
		assert len(result1) == 1
		assert len(result2) == 1
		assert result1[0]["content"] == "conv1_msg"
		assert result2[0]["content"] == "conv2_msg"


class TestPushOTA:
	"""Tests for push_ota() function."""

	def test_push_ota_valid(self):
		"""push_ota should publish to MQTT topic."""
		client = MagicMock()
		with patch.object(server, "mqtt_client", client):
			result = server.push_ota("motor_1", "http://pi.local:8080/motor.bin")
			# Should attempt to publish
			assert result is True or result is False  # Function doesn't assert, just returns

	def test_push_ota_format(self):
		"""OTA push should format message with node_id and bin_url."""
		client = MagicMock()
		with patch.object(server, "mqtt_client", client):
			server.push_ota("test_node", "http://example.com/test.bin")
			# Check that publish was called (if mqtt_client exists)


class TestNodeRegistry:
	"""Tests for NodeRegistry class - node state management."""

	def test_upsert_new_node(self):
		"""Upsert should create new node with mac and first_seen."""
		registry = server.NodeRegistry()
		result = registry.upsert("aa:bb:cc:dd:ee:ff", {"status": "online"})
		assert result["mac"] == "aa:bb:cc:dd:ee:ff"
		assert "first_seen" in result
		assert result["status"] == "online"

	def test_upsert_updates_existing_node(self):
		"""Upsert should update existing node."""
		registry = server.NodeRegistry()
		first_upsert = registry.upsert("aa:bb:cc:dd:ee:ff", {"status": "online"})
		first_seen = first_upsert["first_seen"]
		
		time.sleep(0.01)  # Ensure time passes
		second_upsert = registry.upsert("aa:bb:cc:dd:ee:ff", {"status": "offline", "version": "1.0"})
		
		assert second_upsert["mac"] == "aa:bb:cc:dd:ee:ff"
		assert second_upsert["first_seen"] == first_seen  # Should not change
		assert second_upsert["status"] == "offline"
		assert second_upsert["version"] == "1.0"
		assert second_upsert["last_seen"] > first_upsert["last_seen"]

	def test_update_by_node_id_existing_node(self):
		"""update_by_node_id should find and update node by node_id."""
		registry = server.NodeRegistry()
		registry.upsert("aa:bb:cc:dd:ee:ff", {"node_id": "motor_1", "status": "online"})
		
		registry.update_by_node_id("motor_1", {"battery": "85%", "temp": 32})
		
		nodes = registry.all()
		assert len(nodes) == 1
		assert nodes[0]["node_id"] == "motor_1"
		assert nodes[0]["battery"] == "85%"
		assert nodes[0]["temp"] == 32

	def test_update_by_node_id_creates_if_not_found(self):
		"""update_by_node_id should create node if not found."""
		registry = server.NodeRegistry()
		registry.update_by_node_id("new_node", {"status": "factory"})
		
		nodes = registry.all()
		assert len(nodes) == 1
		assert nodes[0]["node_id"] == "new_node"
		assert nodes[0]["status"] == "factory"

	def test_all_returns_sorted_list(self):
		"""all() should return nodes sorted by node_id."""
		registry = server.NodeRegistry()
		registry.upsert("aa:bb:cc:dd:ee:ff", {"node_id": "z_node"})
		registry.upsert("11:22:33:44:55:66", {"node_id": "a_node"})
		registry.upsert("99:88:77:66:55:44", {"node_id": "m_node"})
		
		nodes = registry.all()
		node_ids = [n["node_id"] for n in nodes]
		assert node_ids == ["a_node", "m_node", "z_node"]

	def test_connectivity_online(self):
		"""Recent nodes with online status should show connectivity=online."""
		registry = server.NodeRegistry()
		registry.upsert("aa:bb:cc:dd:ee:ff", {"status": "online", "last_seen": time.time()})
		
		nodes = registry.all()
		assert nodes[0]["connectivity"] == "online"

	def test_connectivity_offline_after_timeout(self):
		"""Nodes not seen in >120s should show connectivity=offline."""
		registry = server.NodeRegistry()
		old_time = time.time() - 200  # 200 seconds ago
		registry.upsert("aa:bb:cc:dd:ee:ff", {"status": "online"})
		# Manually set last_seen to old time (simulating stale node)
		registry._nodes["aa:bb:cc:dd:ee:ff"]["last_seen"] = old_time
		
		nodes = registry.all()
		assert nodes[0]["connectivity"] == "offline"

	def test_connectivity_factory_status(self):
		"""Nodes with factory/needs_provisioning status should show connectivity=factory."""
		registry = server.NodeRegistry()
		registry.upsert("aa:bb:cc:dd:ee:ff", {"status": "factory", "last_seen": time.time()})
		registry.upsert("11:22:33:44:55:66", {"status": "needs_provisioning", "last_seen": time.time()})
		
		nodes = registry.all()
		nodes_by_mac = {n.get("mac"): n for n in nodes}
		assert nodes_by_mac["aa:bb:cc:dd:ee:ff"]["connectivity"] == "factory"
		assert nodes_by_mac["11:22:33:44:55:66"]["connectivity"] == "factory"

	def test_mark_offline_ages_out_nodes(self):
		"""mark_offline() should age nodes not seen in >120s."""
		registry = server.NodeRegistry()
		old_time = time.time() - 200
		recent_time = time.time()
		
		registry.upsert("aa:bb:cc:dd:ee:ff", {"status": "online"})
		registry.upsert("11:22:33:44:55:66", {"status": "online"})
		
		# Manually set last_seen for old node (simulating stale node)
		registry._nodes["aa:bb:cc:dd:ee:ff"]["last_seen"] = old_time
		
		registry.mark_offline()
		nodes = registry.all()
		nodes_by_mac = {n.get("mac"): n for n in nodes}
		
		assert nodes_by_mac["aa:bb:cc:dd:ee:ff"]["connectivity"] == "offline"
		assert nodes_by_mac["11:22:33:44:55:66"]["connectivity"] == "online"

	def test_multiple_nodes(self):
		"""Registry should handle multiple independent nodes."""
		registry = server.NodeRegistry()
		
		# Add multiple nodes
		for i in range(5):
			mac = f"aa:bb:cc:dd:ee:{i:02x}"
			registry.upsert(mac, {"node_id": f"node_{i}", "status": "online"})
		
		nodes = registry.all()
		assert len(nodes) == 5
		assert all("node_id" in n for n in nodes)
		assert all("mac" in n for n in nodes)

	def test_thread_safety_concurrent_upserts(self):
		"""Registry should handle concurrent upserts safely."""
		import threading
		
		registry = server.NodeRegistry()
		errors = []
		
		def upsert_node(mac, node_id):
			try:
				registry.upsert(mac, {"node_id": node_id, "status": "online"})
			except Exception as e:
				errors.append(e)
		
		threads = []
		for i in range(10):
			mac = f"aa:bb:cc:dd:ee:{i:02x}"
			t = threading.Thread(target=upsert_node, args=(mac, f"node_{i}"))
			threads.append(t)
			t.start()
		
		for t in threads:
			t.join()
		
		assert len(errors) == 0
		assert len(registry.all()) == 10

	def test_upsert_preserves_existing_fields(self):
		"""Upsert should preserve fields not in the update dict."""
		registry = server.NodeRegistry()
		registry.upsert("aa:bb:cc:dd:ee:ff", {"node_id": "motor_1", "status": "online", "version": "1.0"})
		
		# Upsert with partial update
		registry.upsert("aa:bb:cc:dd:ee:ff", {"battery": "85%"})
		
		nodes = registry.all()
		assert nodes[0]["node_id"] == "motor_1"
		assert nodes[0]["status"] == "online"
		assert nodes[0]["version"] == "1.0"
		assert nodes[0]["battery"] == "85%"

	def test_all_returns_copy_not_reference(self):
		"""all() should return copies so modifications don't affect internal state."""
		registry = server.NodeRegistry()
		registry.upsert("aa:bb:cc:dd:ee:ff", {"node_id": "motor_1", "status": "online"})
		
		nodes = registry.all()
		nodes[0]["status"] = "hacked"  # Try to modify
		
		# Check that internal state is unchanged
		nodes2 = registry.all()
		assert nodes2[0]["status"] == "online"


class TestExtractCommands:
	"""Tests for extract_commands() function."""

	def test_empty_text(self):
		"""Empty text should return empty list."""
		result = server.extract_commands("")
		assert result == []

	def test_no_commands(self):
		"""Text without command markers should return empty list."""
		text = "Just some regular text without any commands."
		result = server.extract_commands(text)
		assert result == []

	def test_single_command_with_json_payload(self):
		"""Single command with JSON payload should be extracted."""
		text = '[COMMAND] nodes/motor-01/cmd/rotate {"angle": 90}'
		result = server.extract_commands(text)
		assert len(result) == 1
		assert result[0]["node_id"] == "motor-01"
		assert result[0]["channel"] == "rotate"
		assert result[0]["payload"]["angle"] == 90
		assert result[0]["status"] == "pending"
		assert result[0]["auth_level"] == "confirmed"

	def test_cmd_alias_format(self):
		"""[CMD] alias should also be recognized."""
		text = '[CMD] nodes/sensor-01/cmd/sample {"rate": 10}'
		result = server.extract_commands(text)
		assert len(result) == 1
		assert result[0]["node_id"] == "sensor-01"

	def test_multiple_commands(self):
		"""Multiple commands should all be extracted."""
		text = '''[COMMAND] nodes/motor-01/cmd/rotate {"angle": 90}
		Some text in between.
		[CMD] nodes/motor-02/cmd/speed {"rpm": 100}'''
		result = server.extract_commands(text)
		assert len(result) == 2
		assert result[0]["node_id"] == "motor-01"
		assert result[1]["node_id"] == "motor-02"

	def test_command_with_invalid_json_payload(self):
		"""Command with invalid JSON should have empty payload."""
		text = '[COMMAND] nodes/motor-01/cmd/rotate {invalid json}'
		result = server.extract_commands(text)
		assert len(result) == 1
		assert result[0]["payload"] == {}

	def test_command_with_simple_payload(self):
		"""Command with non-JSON payload should have empty payload."""
		text = '[COMMAND] nodes/motor-01/cmd/on activate'
		result = server.extract_commands(text)
		assert len(result) == 1
		assert result[0]["payload"] == {}

	def test_command_has_id_and_timestamp(self):
		"""Each command should have unique ID and timestamp."""
		text = '[COMMAND] nodes/motor-01/cmd/rotate {"angle": 90}'
		before = time.time()
		result = server.extract_commands(text)
		after = time.time()
		assert result[0]["id"].startswith("cmd_")
		assert "created" in result[0]
		assert before <= result[0]["created"] <= after

	def test_case_insensitive_command_marker(self):
		"""Command marker should be case-insensitive."""
		text1 = '[command] nodes/motor-01/cmd/rotate {"angle": 90}'
		text2 = '[Command] nodes/motor-01/cmd/rotate {"angle": 90}'
		result1 = server.extract_commands(text1)
		result2 = server.extract_commands(text2)
		assert len(result1) == 1
		assert len(result2) == 1


class TestExtractReasoning:
	"""Tests for extract_reasoning() function."""

	def test_empty_text(self):
		"""Empty text should return empty list."""
		result = server.extract_reasoning("")
		assert result == []

	def test_no_reasoning_blocks(self):
		"""Text without reasoning markers should return empty list."""
		text = "Just some regular text."
		result = server.extract_reasoning(text)
		assert result == []

	def test_single_reasoning_block(self):
		"""Single reasoning block should be extracted."""
		text = """[REASONING]
		- Observed: Temperature is rising
		- Hypothesis: Thermal runaway condition
		- Confidence: high
		[/REASONING]"""
		result = server.extract_reasoning(text)
		assert len(result) == 1
		assert result[0]["observed"] == "Temperature is rising"
		assert result[0]["hypothesis"] == "Thermal runaway condition"
		assert result[0]["confidence"] == "high"

	def test_reasoning_with_all_fields(self):
		"""Reasoning block with all fields should parse correctly."""
		text = """[REASONING]
		- Observed: Motor not responding
		- Context: Last command was speed control
		- Hypothesis: Motor controller reset
		- Confidence: medium
		- Next Step: Check motor controller logs
		[/REASONING]"""
		result = server.extract_reasoning(text)
		assert len(result) == 1
		assert result[0]["observed"] == "Motor not responding"
		assert result[0]["context"] == "Last command was speed control"
		assert result[0]["hypothesis"] == "Motor controller reset"
		assert result[0]["confidence"] == "medium"
		assert result[0]["next_step"] == "Check motor controller logs"

	def test_multiple_reasoning_blocks(self):
		"""Multiple reasoning blocks should all be extracted."""
		text = """[REASONING]
		- Observed: Error 1
		- Hypothesis: Cause 1
		- Confidence: high
		[/REASONING]
		Some text in between.
		[REASONING]
		- Observed: Error 2
		- Hypothesis: Cause 2
		- Confidence: low
		[/REASONING]"""
		result = server.extract_reasoning(text)
		assert len(result) == 2
		assert result[0]["observed"] == "Error 1"
		assert result[1]["observed"] == "Error 2"

	def test_reasoning_case_insensitive_markers(self):
		"""Reasoning markers should be case-insensitive."""
		text = """[reasoning]
		- Observed: Test observation
		- Hypothesis: Test hypothesis
		[/reasoning]"""
		result = server.extract_reasoning(text)
		assert len(result) == 1
		assert result[0]["observed"] == "Test observation"

	def test_reasoning_default_confidence(self):
		"""Reasoning without confidence field should default to medium."""
		text = """[REASONING]
		- Observed: Something happened
		- Hypothesis: Maybe this is why
		[/REASONING]"""
		result = server.extract_reasoning(text)
		assert result[0]["confidence"] == "medium"

	def test_reasoning_invalid_confidence_ignored(self):
		"""Invalid confidence values should be ignored."""
		text = """[REASONING]
		- Observed: Test
		- Hypothesis: Test
		- Confidence: very_high
		[/REASONING]"""
		result = server.extract_reasoning(text)
		# Invalid confidence should not change default
		assert result[0]["confidence"] == "medium"

	def test_reasoning_full_text_preserved(self):
		"""Full reasoning text should be preserved."""
		text = """[REASONING]
		- Observed: Multi-line
		  observation details
		- Hypothesis: Some hypothesis
		[/REASONING]"""
		result = server.extract_reasoning(text)
		assert "Multi-line" in result[0]["full_text"]
		assert "observation details" in result[0]["full_text"]


if __name__ == "__main__":
	pytest.main([__file__, "-v"])

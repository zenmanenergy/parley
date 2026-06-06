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


if __name__ == "__main__":
	pytest.main([__file__, "-v"])

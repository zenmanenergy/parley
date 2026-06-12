#!/usr/bin/env python3
"""
Parley dashboard backend.

Serves the static frontend and maintains a WebSocket connection to every
browser client. Bridges between MQTT (Mosquitto on the Pi) and WebSocket
so the browser sees live node data, anomalies, and logs.

Also provides:
  - Claude AI conversation (streams responses back via WebSocket)
  - PlatformIO firmware compilation (runs pio run in subprocess)
  - OTA push (publishes compiled .bin URL to MQTT for the node to fetch)

Dependencies:
    pip install asyncio websockets paho-mqtt anthropic aiofiles

Run:
    python3 server.py

Configuration via environment variables:
    MQTT_HOST          broker host                     (default: localhost)
    MQTT_PORT          broker port                     (default: 1883)
    WS_PORT            WebSocket port                  (default: 8765)
    HTTP_PORT          Static file HTTP port           (default: 8080)
    ANTHROPIC_API_KEY  Your Anthropic API key          (required for AI)
    FIRMWARE_NODES_DIR Absolute path to firmware/nodes (default: auto-detect)
    PI_IP              Pi IP as seen by nodes          (default: 192.168.4.2)
"""

import asyncio
import json
import logging
import os
import re
import subprocess
import time
import threading
from http.server import HTTPServer, SimpleHTTPRequestHandler
from pathlib import Path
from typing import Dict, Set, Any, List

import paho.mqtt.client as mqtt
try:
	import yaml
except ImportError:
	yaml = None
import websockets
from websockets.asyncio.server import ServerConnection

# Load .env file if present
try:
	from dotenv import load_dotenv
	load_dotenv()
except ImportError:
	pass  # dotenv optional for development

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

MQTT_HOST    = os.environ.get("MQTT_HOST",    "localhost")
MQTT_PORT    = int(os.environ.get("MQTT_PORT", "1883"))
WS_PORT      = int(os.environ.get("WS_PORT",   "8765"))
HTTP_PORT    = int(os.environ.get("HTTP_PORT",  "8080"))
PI_IP        = os.environ.get("PI_IP",          "192.168.4.2")

ANTHROPIC_API_KEY = os.environ.get("ANTHROPIC_API_KEY", "")

STATIC_DIR   = Path(__file__).parent / "static"
FIRMWARE_DIR = Path(__file__).parent / "static" / "firmware"   # served over HTTP

# Default firmware/nodes dir: two levels up from pi/dashboard/ then into firmware/nodes
_default_fw_nodes = Path(__file__).parent.parent.parent / "firmware" / "nodes"
FIRMWARE_NODES_DIR = Path(os.environ.get("FIRMWARE_NODES_DIR", str(_default_fw_nodes)))

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
)
log = logging.getLogger("parley")

# ---------------------------------------------------------------------------
# Parley system prompt for Claude
# This is sent as the system message for every conversation so Claude
# always knows how to write compatible firmware.
# ---------------------------------------------------------------------------

PARLEY_SYSTEM_PROMPT = """
You are an expert embedded-systems AI assistant for the Parley robot platform.

## System Architecture
- Raspberry Pi: coordinator, runs Mosquitto MQTT broker and this dashboard
- Gateway ESP32-S3: WiFi access point (SSID=ParleyNet), connects to Pi via USB CDC-ECM
- Peripheral nodes: ESP32-S3 boards connected to gateway via WiFi
- All communication: MQTT broker at 192.168.4.2:1883
- Build system: PlatformIO + Arduino framework + ESP-IDF 5.x

## Node Plugin Interface (node_template.h)
Every peripheral node implements these 4 REQUIRED functions:

```cpp
#include "node_template.h"

// Called once after WiFi + MQTT are connected
void setup_peripheral();

// Called every loop iteration — main application work here
void loop_peripheral();

// Must return true if healthy, false if degraded. Must be fast (<1ms).
bool peripheral_health_check();

// Handle command from nodes/<node_id>/cmd/<channel>
void peripheral_handle_command(const char* channel, JsonDocument& payload);
```

Optional weak functions (default = no-op, only implement if needed):
```cpp
void peripheral_shutdown();          // before reboot
void peripheral_publish_status();    // add custom fields to heartbeat
void peripheral_on_disconnect();     // MQTT dropped
void peripheral_on_reconnect();      // MQTT restored
```

## Utility Functions (always available)
```cpp
bool node_publish(const char* topic, const char* payload, bool retained = false);
bool node_publish_string(const char* topic, const String& payload);
bool node_publish_json(const char* topic, JsonDocument& doc);
bool node_subscribe(const char* topic);  // register additional subscriptions
void node_log(LogLevel level, const char* msg);
// LogLevel: LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR

// NVS persistent storage (namespace "parley_app")
bool node_nvs_get(const char* key, char* buf, size_t len);
bool node_nvs_set(const char* key, const char* value);
```

## File Locations
- Application code: firmware/nodes/src/<env_name>/main.cpp
- Must include: #include "node_template.h"
- Must NOT define setup() or loop() — the template owns those
- Sensor data topic: nodes/<node_id>/data/<channel>

## PlatformIO Environment (add to firmware/nodes/platformio.ini)
```ini
[env:<env_name>]
build_src_filter = +<src/<env_name>/*>
```
The shared `lib_deps` from the base [env] section are inherited automatically.

## Hardware
- MCU: ESP32-S3 N16R8 (16MB flash, 8MB PSRAM, 240 MHz dual-core)
- Arduino libraries available: Wire, SPI, Servo, analogRead, etc.
- PSRAM accessible via `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`

## Coding Rules
- Tabs for indentation (not spaces)
- setup_peripheral(): sensor init + subscribe to command channels
- loop_peripheral(): non-blocking sensor reads + publish data
- peripheral_health_check(): only check health, no side effects
- Avoid delay() — use millis() timing patterns

## Output Format
When writing firmware, always use this exact format so the dashboard can parse it:

```cpp
// FILE: firmware/nodes/src/<env_name>/main.cpp
<your code here>
```

If a new platformio.ini entry is needed:
```ini
// FILE: firmware/nodes/platformio.ini
[env:<env_name>]
build_src_filter = +<src/<env_name>/*>
```

Think carefully before writing code. Ask clarifying questions if the node's purpose, sensors, or actuators are not clear.
"""



# ---------------------------------------------------------------------------
# Node registry
# In-memory: mac -> node dict. Updated by MQTT messages.
# ---------------------------------------------------------------------------

class NodeRegistry:
    def __init__(self):
        self._nodes: Dict[str, Dict[str, Any]] = {}
        self._lock = threading.Lock()

    def upsert(self, mac: str, update: Dict[str, Any]) -> Dict[str, Any]:
        with self._lock:
            if mac not in self._nodes:
                self._nodes[mac] = {"mac": mac, "first_seen": time.time()}
            self._nodes[mac].update(update)
            self._nodes[mac]["last_seen"] = time.time()
            return dict(self._nodes[mac])

    def update_by_node_id(self, node_id: str, update: Dict[str, Any]):
        with self._lock:
            for mac, node in self._nodes.items():
                if node.get("node_id") == node_id:
                    node.update(update)
                    node["last_seen"] = time.time()
                    return
            # Node not in registry yet (status arrived before discovery)
            self._nodes[node_id] = {"node_id": node_id, "last_seen": time.time(), **update}

    def all(self) -> list:
        with self._lock:
            now = time.time()
            result = []
            for node in self._nodes.values():
                n = dict(node)
                last = n.get("last_seen", 0)
                age = now - last
                if age > 120:
                    n["connectivity"] = "offline"
                elif n.get("status") in ("factory", "needs_provisioning"):
                    n["connectivity"] = "factory"
                else:
                    n["connectivity"] = "online"
                result.append(n)
            return sorted(result, key=lambda n: n.get("node_id", n.get("mac", "")))

    def mark_offline(self):
        """Age out nodes that haven't been seen in >120s."""
        now = time.time()
        with self._lock:
            for node in self._nodes.values():
                if now - node.get("last_seen", 0) > 120:
                    node["connectivity"] = "offline"


registry = NodeRegistry()

# ---------------------------------------------------------------------------
# Anomaly feed (capped ring buffer)
# ---------------------------------------------------------------------------

MAX_ANOMALIES = 50
anomaly_feed: list = []
anomaly_lock = threading.Lock()

def push_anomaly(payload: Dict[str, Any]):
    with anomaly_lock:
        anomaly_feed.append({"ts": time.time(), **payload})
        if len(anomaly_feed) > MAX_ANOMALIES:
            anomaly_feed.pop(0)


# ---------------------------------------------------------------------------
# WebSocket client set + broadcast
# ---------------------------------------------------------------------------

ws_clients: Set[ServerConnection] = set()
ws_lock = asyncio.Lock()

# asyncio event loop reference — set once the ws server starts
_loop: asyncio.AbstractEventLoop | None = None


def _schedule_broadcast(msg: dict):
    """Thread-safe: schedule a broadcast from the MQTT thread."""
    if _loop is None:
        return
    asyncio.run_coroutine_threadsafe(_broadcast(msg), _loop)


async def _broadcast(msg: dict):
    data = json.dumps(msg)
    async with ws_lock:
        dead = set()
        for client in ws_clients:
            try:
                await client.send(data)
            except Exception:
                dead.add(client)
        ws_clients.difference_update(dead)


# Reassembly buffer for chunked recovery logs: node_id -> list of (seq, chunk)
_recovery_log_chunks: dict = {}


# ---------------------------------------------------------------------------
# MQTT bridge
# ---------------------------------------------------------------------------

def on_connect(client, userdata, flags, rc, properties=None):
    if rc == 0:
        log.info("MQTT connected to %s:%d", MQTT_HOST, MQTT_PORT)
        client.subscribe("system/discovery")
        client.subscribe("system/anomalies")
        client.subscribe("system/registered")
        client.subscribe("system/recovery")
        client.subscribe("system/recovery/log")
        client.subscribe("nodes/+/status")
        client.subscribe("nodes/+/log")
        client.subscribe("nodes/+/data/#")
    else:
        log.warning("MQTT connect failed rc=%d", rc)


def on_disconnect(client, userdata, disconnect_flags, rc, properties=None):
    log.warning("MQTT disconnected rc=%d — will reconnect", rc)


def on_message(client, userdata, msg):
    topic: str = msg.topic
    try:
        payload = json.loads(msg.payload)
    except Exception:
        payload = {"raw": msg.payload.decode(errors="replace")}

    log.debug("on_message: topic=%s payload=%s", topic, payload)

    # --- system/discovery ---
    if topic == "system/discovery":
        mac = payload.get("mac")
        log.debug("system/discovery: mac=%s", mac)
        if mac:
            registry.upsert(mac, payload)
            # Get the full node with connectivity calculated
            all_nodes = registry.all()
            node = next((n for n in all_nodes if n.get("mac") == mac), None)
            if node:
                log.info("Node upserted: mac=%s last_seen=%s connectivity=%s", mac, node.get("last_seen"), node.get("connectivity"))
                _schedule_broadcast({"type": "node_update", "node": node})

    # --- system/registered ---
    elif topic == "system/registered":
        mac = payload.get("mac")
        if mac:
            node = registry.upsert(mac, {**payload, "registered": True})
            _schedule_broadcast({"type": "node_update", "node": node})

    # --- system/anomalies ---
    elif topic == "system/anomalies":
        push_anomaly(payload)
        _schedule_broadcast({"type": "anomaly", "anomaly": {"ts": time.time(), **payload}})

    # --- system/recovery (rollback diagnosis summary) ---
    elif topic == "system/recovery":
        node_id = payload.get("node_id", "unknown")
        log.warning("Recovery event from %s: %s", node_id, payload.get("event"))
        push_anomaly(payload)
        _schedule_broadcast({
            "type": "recovery_diagnosis",
            "node_id": node_id,
            "diagnosis": payload,
            "ts": time.time(),
        })

    # --- system/recovery/log (chunked previous.log after rollback) ---
    elif topic == "system/recovery/log":
        node_id = payload.get("node_id", "unknown")
        seq     = payload.get("seq", 0)
        chunk   = payload.get("chunk", "")
        done    = payload.get("done", False)
        if node_id not in _recovery_log_chunks:
            _recovery_log_chunks[node_id] = []
        _recovery_log_chunks[node_id].append((seq, chunk))
        if done:
            ordered = sorted(_recovery_log_chunks.pop(node_id, []), key=lambda x: x[0])
            full_log = "".join(c for _, c in ordered)
            _schedule_broadcast({
                "type": "recovery_log_complete",
                "node_id": node_id,
                "log": full_log,
                "ts": time.time(),
            })

    # --- nodes/<id>/status ---
    elif topic.endswith("/status"):
        parts = topic.split("/")
        if len(parts) == 3:
            node_id = parts[1]
            registry.update_by_node_id(node_id, payload)
            # Find full node record to broadcast
            all_nodes = registry.all()
            node = next((n for n in all_nodes if n.get("node_id") == node_id), None)
            if node:
                _schedule_broadcast({"type": "node_update", "node": node})

    # --- nodes/<id>/log ---
    elif "/log" in topic:
        parts = topic.split("/")
        if len(parts) >= 3:
            _schedule_broadcast({
                "type": "log",
                "node_id": parts[1],
                "entry": payload,
                "ts": time.time(),
            })

    # --- nodes/<id>/data/... ---
    elif "/data/" in topic:
        parts = topic.split("/")
        if len(parts) >= 4:
            _schedule_broadcast({
                "type": "telemetry",
                "node_id": parts[1],
                "channel": "/".join(parts[3:]),
                "data": payload,
                "ts": time.time(),
            })


def start_mqtt():
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect    = on_connect
    client.on_disconnect = on_disconnect
    client.on_message    = on_message
    client.connect_async(MQTT_HOST, MQTT_PORT)
    client.loop_start()
    return client


# ---------------------------------------------------------------------------
# Conversation session store
# conv_id -> list of {"role": "user"|"assistant", "content": "..."}
# ---------------------------------------------------------------------------

CONVERSATIONS_DIR = Path(__file__).parent / "conversations"
CONVERSATIONS_DIR.mkdir(exist_ok=True)

conv_sessions: Dict[str, List[dict]] = {}
conv_lock = threading.Lock()
conv_metadata: Dict[str, dict] = {}  # conv_id -> {title, created, related_nodes, code_blocks, etc}

def _get_conv_history(conv_id: str) -> List[dict]:
    with conv_lock:
        if conv_id not in conv_sessions:
            conv_sessions[conv_id] = []
        return list(conv_sessions[conv_id])

def _append_conv(conv_id: str, role: str, content: str):
    with conv_lock:
        if conv_id not in conv_sessions:
            conv_sessions[conv_id] = []
        conv_sessions[conv_id].append({"role": role, "content": content, "timestamp": time.time()})
    _save_conversation_to_disk(conv_id)

def _set_conv_metadata(conv_id: str, **kwargs):
	"""Update conversation metadata (title, related_nodes, kind, etc)."""
	with conv_lock:
		if conv_id not in conv_metadata:
			conv_metadata[conv_id] = {"created": time.time()}
		conv_metadata[conv_id].update(kwargs)
	_save_conversation_to_disk(conv_id)

def _save_conversation_to_disk(conv_id: str):
	"""Save a conversation and its metadata to a JSON file."""
	try:
		with conv_lock:
			if conv_id not in conv_sessions:
				return
			history = conv_sessions[conv_id]
			meta = conv_metadata.get(conv_id, {})
		
		# Generate filename from metadata or conv_id
		title = meta.get("title", "untitled")
		created = meta.get("created", time.time())
		created_dt = time.strftime("%Y-%m-%d_%H-%M-%S", time.localtime(created))
		# Sanitize title for filename
		title_safe = re.sub(r"[^a-zA-Z0-9_-]", "_", title)[:40]
		filename = f"{created_dt}_{title_safe}.json"
		
		filepath = CONVERSATIONS_DIR / filename
		
		conv_data = {
			"id": conv_id,
			"title": title,
			"kind": meta.get("kind", "other"),
			"created": created,
			"closed": meta.get("closed"),
			"related_nodes": meta.get("related_nodes", []),
			"messages": history,
			"code_blocks": meta.get("code_blocks", []),
			"tasks": meta.get("tasks", []),
			"summary": meta.get("summary"),
		}
		
		filepath.write_text(json.dumps(conv_data, indent=2, default=str), encoding="utf-8")
		log.debug("Saved conversation %s to %s", conv_id, filepath)
	except Exception as e:
		log.error("Failed to save conversation: %s", e)

def _load_conversations_from_disk():
	"""Load all past conversations from disk on startup."""
	try:
		if not CONVERSATIONS_DIR.exists():
			return
		for filepath in sorted(CONVERSATIONS_DIR.glob("*.json"), reverse=True):
			try:
				data = json.loads(filepath.read_text(encoding="utf-8"))
				conv_id = data.get("id")
				if conv_id:
					with conv_lock:
						conv_sessions[conv_id] = data.get("messages", [])
						conv_metadata[conv_id] = {
							"title": data.get("title"),
							"kind": data.get("kind"),
							"created": data.get("created"),
							"closed": data.get("closed"),
							"related_nodes": data.get("related_nodes", []),
							"code_blocks": data.get("code_blocks", []),
							"tasks": data.get("tasks", []),
							"summary": data.get("summary"),
						}
				log.debug("Loaded conversation from %s", filepath.name)
			except Exception as e:
				log.warning("Failed to load conversation from %s: %s", filepath.name, e)
	except Exception as e:
		log.error("Failed to load conversations: %s", e)

def _search_conversations(query: str = "", kind: str = "", node_id: str = "", limit: int = 50) -> List[dict]:
	"""Search conversations by title, kind, node, or full-text search."""
	results = []
	query_lower = query.lower()
	
	with conv_lock:
		for conv_id, meta in sorted(conv_metadata.items(), key=lambda x: x[1].get("created", 0), reverse=True):
			# Filter by kind
			if kind and meta.get("kind") != kind:
				continue
			
			# Filter by node
			if node_id and node_id not in meta.get("related_nodes", []):
				continue
			
			# Search by title or full-text
			if query_lower:
				title = meta.get("title", "").lower()
				if query_lower not in title:
					# Try full-text search in messages
					found = False
					for msg in conv_sessions.get(conv_id, []):
						if query_lower in msg.get("content", "").lower():
							found = True
							break
					if not found:
						continue
			
			# Build result card
			result = {
				"id": conv_id,
				"title": meta.get("title", "Untitled"),
				"created": meta.get("created"),
				"closed": meta.get("closed"),
				"kind": meta.get("kind", "other"),
				"related_nodes": meta.get("related_nodes", []),
				"summary": meta.get("summary", ""),
				"message_count": len(conv_sessions.get(conv_id, [])),
				"code_blocks": len(meta.get("code_blocks", [])),
			}
			results.append(result)
	
	return results[:limit]



# ---------------------------------------------------------------------------
# Context loading: part library, layout, prior work
# ---------------------------------------------------------------------------

def load_part_library() -> dict:
	"""Load the part library from part_library.json in the project root."""
	try:
		root = Path(__file__).parent.parent.parent
		part_lib_file = root / "part_library.json"
		if part_lib_file.exists():
			with open(part_lib_file, "r") as f:
				return json.load(f)
		return {}
	except Exception as e:
		log.warning(f"Failed to load part_library.json: {e}")
		return {}

def load_layout() -> dict:
	"""Load the spatial layout from layout.yaml in the project root."""
	if yaml is None:
		log.warning("yaml module not installed; layout context unavailable. Run: pip install pyyaml")
		return {}
	try:
		root = Path(__file__).parent.parent.parent
		layout_file = root / "layout.yaml"
		if layout_file.exists():
			with open(layout_file, "r") as f:
				return yaml.safe_load(f) or {}
		return {}
	except Exception as e:
		log.warning(f"Failed to load layout.yaml: {e}")
		return {}

def get_relevant_part_entries(user_message: str, library: dict) -> list:
	"""Extract relevant part library entries based on user message keywords."""
	if not library or "parts" not in library:
		return []
	relevant = []
	msg_lower = user_message.lower()
	for part_id, part_data in library.get("parts", {}).items():
		# Match by name, model, or manufacturer
		name = part_data.get("name", "").lower()
		model = part_data.get("model", "").lower()
		mfg = part_data.get("manufacturer", "").lower()
		part_type = part_data.get("type", "").lower()
		if any(keyword in msg_lower for keyword in [name, model, mfg, part_type] if keyword):
			relevant.append(part_data)
	return relevant

# ---------------------------------------------------------------------------
# Claude streaming helper
# ---------------------------------------------------------------------------

async def stream_claude(ws: ServerConnection, conv_id: str, user_message: str, node_context):
	"""Call Claude API, stream chunks to the browser, return full response text."""
	if not ANTHROPIC_API_KEY:
		await ws.send(json.dumps({
			"type": "ai_error",
			"conv_id": conv_id,
			"error": "ANTHROPIC_API_KEY not set on the Pi. Export it and restart the server.",
		}))
		return None

	try:
		import anthropic
	except ImportError:
		await ws.send(json.dumps({
			"type": "ai_error",
			"conv_id": conv_id,
			"error": "anthropic package not installed. Run: pip install anthropic",
		}))
		return None

	# Build system, optionally injecting live node context
	system = PARLEY_SYSTEM_PROMPT
	if node_context:
		system += f"\n\n## Focused node (live data)\n```json\n{json.dumps(node_context, indent=2)}\n```\n"

	nodes = registry.all()
	if nodes:
		brief = [{"node_id": n.get("node_id"), "status": n.get("connectivity"), "fw": n.get("fw")} for n in nodes]
		system += f"\n\n## All registered nodes (live)\n```json\n{json.dumps(brief, indent=2)}\n```\n"

	# Auto-load part library and layout for registration conversations
	part_library = load_part_library()
	relevant_parts = get_relevant_part_entries(user_message, part_library)
	if relevant_parts:
		system += f"\n\n## Relevant Parts from Library\n"
		for part in relevant_parts:
			system += f"### {part.get('name', 'Unknown')}\n"
			system += f"- Model: {part.get('model', 'N/A')}\n"
			system += f"- Communication: {part.get('communication', 'N/A')}\n"
			system += f"- Recommended library: {part.get('recommended_library', 'N/A')}\n"
			system += f"- Key gotchas: {', '.join(part.get('known_gotchas', [])[:2])}\n"
			if part.get('last_integrated'):
				system += f"- Last used: {part.get('last_integrated')} (integration count: {part.get('integration_count', 0)})\n"
			system += "\n"

	layout = load_layout()
	if layout and "peripherals" in layout:
		system += f"\n\n## Current Robot Layout\n"
		system += f"Robot: {layout.get('robot', {}).get('name', 'unnamed')}\n"
		system += f"Registered peripherals: {', '.join(layout.get('peripherals', {}).keys())}\n"

	_append_conv(conv_id, "user", user_message)
	history = _get_conv_history(conv_id)

	# Filter messages to only include role and content (Claude API doesn't accept timestamp or other fields)
	api_messages = [{"role": m["role"], "content": m["content"]} for m in history]

	client_ai = anthropic.Anthropic(api_key=ANTHROPIC_API_KEY)

	full_text = ""
	try:
		with client_ai.messages.stream(
			model="claude-sonnet-4-5",
			max_tokens=4096,
			system=system,
			messages=api_messages,
		) as stream:
			for chunk in stream.text_stream:
				full_text += chunk
				await ws.send(json.dumps({
					"type": "ai_chunk",
					"conv_id": conv_id,
					"text": chunk,
				}))
	except Exception as e:
		await ws.send(json.dumps({
			"type": "ai_error",
			"conv_id": conv_id,
			"error": str(e),
		}))
		return None

	_append_conv(conv_id, "assistant", full_text)

	code_blocks = extract_code_blocks(full_text)
	tasks = extract_tasks(full_text)

	# Update metadata with code blocks and tasks
	_set_conv_metadata(conv_id, code_blocks=code_blocks, tasks=tasks)

	await ws.send(json.dumps({
		"type": "ai_message_done",
		"conv_id": conv_id,
		"code_blocks": code_blocks,
		"tasks": tasks,
	}))
	return full_text


def extract_code_blocks(text: str) -> List[dict]:
	"""Parse fenced code blocks from Claude's response."""
	pattern = re.compile(r"```(\w*)\n(.*?)```", re.DOTALL)
	blocks = []
	for m in pattern.finditer(text):
		lang = m.group(1) or "text"
		code = m.group(2)
		first_line = code.strip().split("\n")[0] if code.strip() else ""
		filename = ""
		if first_line.startswith("// FILE:"):
			filename = first_line.replace("// FILE:", "").strip()
		elif first_line.startswith("// env:") or first_line.startswith("[env:"):
			filename = "firmware/nodes/platformio.ini"
		blocks.append({"lang": lang, "filename": filename, "code": code})
	return blocks


def extract_tasks(text: str) -> List[dict]:
	"""Extract task items from Claude's response.
	
	Looks for patterns like:
	- Task: <description>
	- [ ] <task description> (checkbox format)
	- 1. <task description> (numbered tasks)
	"""
	tasks = []
	task_id_counter = 0
	
	# Pattern 1: "Task: ..." lines
	task_pattern = re.compile(r'^\s*(?:Task|ACTION|TODO):\s*(.+?)(?:\n|$)', re.MULTILINE | re.IGNORECASE)
	for m in task_pattern.finditer(text):
		task_text = m.group(1).strip()
		if task_text:
			tasks.append({
				"id": f"task_{task_id_counter}",
				"title": task_text,
				"status": "pending",
				"created": time.time(),
			})
			task_id_counter += 1
	
	# Pattern 2: "[ ] task text" checkbox format
	checkbox_pattern = re.compile(r'^\s*\[\s*\]\s+(.+?)(?:\n|$)', re.MULTILINE)
	for m in checkbox_pattern.finditer(text):
		task_text = m.group(1).strip()
		if task_text:
			tasks.append({
				"id": f"task_{task_id_counter}",
				"title": task_text,
				"status": "pending",
				"created": time.time(),
			})
			task_id_counter += 1
	
	return tasks


# ---------------------------------------------------------------------------
# Firmware compilation (PlatformIO)
# ---------------------------------------------------------------------------

async def compile_firmware(ws: ServerConnection, env_name: str, source_code: str, conv_id: str):
    """
    Write source_code to firmware/nodes/src/<env_name>/main.cpp,
    run pio run -e <env_name>, stream output, copy .bin to static/firmware/.
    Also records compilation metadata in conversation history.
    """
    src_dir = FIRMWARE_NODES_DIR / "src" / env_name
    src_dir.mkdir(parents=True, exist_ok=True)
    src_file = src_dir / "main.cpp"
    src_file.write_text(source_code, encoding="utf-8")
    log.info("Wrote firmware source to %s", src_file)

    FIRMWARE_DIR.mkdir(parents=True, exist_ok=True)

    # Track compilation in conversation metadata
    with conv_lock:
        if conv_id in conv_metadata:
            code_blocks = conv_metadata[conv_id].get("code_blocks", [])
            # Mark the code block as being compiled
            for block in code_blocks:
                if block.get("language") in ("cpp", "c"):
                    block["compiled_at"] = time.time()
                    block["status"] = "compiling"
            conv_metadata[conv_id]["code_blocks"] = code_blocks

    await ws.send(json.dumps({
        "type": "compile_start",
        "conv_id": conv_id,
        "env": env_name,
        "message": f"Starting PlatformIO build for env:{env_name} ...",
    }))

    success = False
    all_lines: list[str] = []
    try:
        proc = await asyncio.create_subprocess_exec(
            "pio", "run", "-e", env_name,
            cwd=str(FIRMWARE_NODES_DIR),
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
        )

        async for line in proc.stdout:
            decoded = line.decode(errors="replace").rstrip()
            all_lines.append(decoded)
            await ws.send(json.dumps({
                "type": "compile_output",
                "conv_id": conv_id,
                "line": decoded,
            }))

        await proc.wait()
        success = proc.returncode == 0

    except FileNotFoundError:
        msg_line = "ERROR: 'pio' not found. Install PlatformIO: pip install platformio"
        all_lines.append(msg_line)
        await ws.send(json.dumps({
            "type": "compile_output",
            "conv_id": conv_id,
            "line": msg_line,
        }))

    # Collect error/warning lines for AI fix feedback
    error_lines = [l for l in all_lines if re.search(r"error:|warning:|undefined|fatal", l, re.I)]

    bin_url = None
    if success:
        bin_src = FIRMWARE_NODES_DIR / ".pio" / "build" / env_name / "firmware.bin"
        if bin_src.exists():
            bin_dst = FIRMWARE_DIR / f"{env_name}.bin"
            bin_dst.write_bytes(bin_src.read_bytes())
            bin_url = f"http://{PI_IP}:{HTTP_PORT}/firmware/{env_name}.bin"
            log.info("Firmware ready at %s", bin_url)
            
            # Mark code block as successfully compiled
            with conv_lock:
                if conv_id in conv_metadata:
                    code_blocks = conv_metadata[conv_id].get("code_blocks", [])
                    for block in code_blocks:
                        if block.get("language") in ("cpp", "c"):
                            block["status"] = "compiled"
                            block["bin_url"] = bin_url
                    conv_metadata[conv_id]["code_blocks"] = code_blocks
            _save_conversation_to_disk(conv_id)

    await ws.send(json.dumps({
        "type": "compile_done",
        "conv_id": conv_id,
        "env": env_name,
        "success": success,
        "bin_url": bin_url,
        "error_lines": error_lines if not success else [],
    }))
    return bin_url


# ---------------------------------------------------------------------------
# OTA push via MQTT
# ---------------------------------------------------------------------------

def push_ota(node_id: str, bin_url: str, conv_id: str = None) -> bool:
    """Publish {"url": bin_url} to nodes/<node_id>/ota so the node downloads and flashes.
    Optionally records deployment in conversation metadata."""
    if not mqtt_client:
        return False
    payload = json.dumps({"url": bin_url})
    result = mqtt_client.publish(f"nodes/{node_id}/ota", payload, qos=1)
    success = result.rc == 0
    log.info("OTA push to %s: %s (rc=%s)", node_id, bin_url, result.rc)
    
    # Record deployment in conversation metadata if conv_id provided
    if success and conv_id:
        with conv_lock:
            if conv_id in conv_metadata:
                code_blocks = conv_metadata[conv_id].get("code_blocks", [])
                for block in code_blocks:
                    if block.get("status") == "compiled":
                        if "deployed_to" not in block:
                            block["deployed_to"] = []
                        if node_id not in block["deployed_to"]:
                            block["deployed_to"].append(node_id)
                        block["deployed_at"] = time.time()
                        block["status"] = "deployed"
                conv_metadata[conv_id]["code_blocks"] = code_blocks
        _save_conversation_to_disk(conv_id)
    
    return success


# ---------------------------------------------------------------------------
# Serial reader (USB tunnel from gateway)
# Reads JSON messages from gateway and publishes to MQTT broker
# ---------------------------------------------------------------------------

def find_gateway_port():
	"""Auto-detect the gateway ESP32-S3 on any available COM port."""
	try:
		import serial
		import serial.tools.list_ports
	except ImportError:
		log.warning("pyserial not installed. Serial tunnel disabled. Run: pip install pyserial")
		return None

	# Look for ESP32-S3 or CH343 (USB chip on gateway)
	for port_info in serial.tools.list_ports.comports():
		port = port_info.device
		# ESP32-S3 typically shows as "USB JTAG/serial debug unit" or "CH343"
		desc = port_info.description.lower()
		if "esp" in desc or "ch343" in desc or "usb" in desc:
			log.info("Found potential gateway: %s (%s)", port, port_info.description)
			return port
	
	# Fallback: try COM ports in order
	for i in range(3, 15):
		port = f"COM{i}"
		try:
			ser = serial.Serial(port, 115200, timeout=0.1)
			# Try to read banner to detect ESP32
			data = ser.read(256)
			if b"parley" in data.lower() or b"esp32" in data.lower():
				log.info("Detected gateway on %s", port)
				ser.close()
				return port
			ser.close()
		except Exception:
			pass
	
	log.warning("Gateway device not found on any COM port")
	return None


def start_serial_reader():
	"""Read JSON messages from gateway over USB serial and bridge to MQTT."""
	try:
		import serial
	except ImportError:
		log.warning("pyserial not installed. Serial tunnel disabled. Run: pip install pyserial")
		return

	port = None
	baudrate = 115200
	ser = None
	last_port_check = 0
	reconnect_delay = 3  # Start with 3 seconds (Windows USB needs time to release)
	max_reconnect_delay = 30  # Up to 30 seconds max

	# Initial wait to ensure port is released after previous process
	log.info("Waiting 2 seconds for USB port to stabilize...")
	time.sleep(2)

	while True:
		try:
			# Auto-detect port only when disconnected (not while connected)
			if port is None:
				port = find_gateway_port()
				if port is None:
					log.warning("Waiting for gateway device (USB)...")
					time.sleep(5)
					continue

			if ser is None:
				log.info("Opening %s at %d baud...", port, baudrate)
				try:
					ser = serial.Serial(port, baudrate, timeout=1.0)
					ser.reset_input_buffer()
					log.info("Connected to gateway on %s", port)
					reconnect_delay = 3  # Reset to 3s on successful connection
				except (serial.SerialException, PermissionError, OSError) as e:
					log.warning("Failed to open %s: %s (retrying in %ds)", port, e, reconnect_delay)
					ser = None
					time.sleep(reconnect_delay)
					reconnect_delay = min(reconnect_delay * 2.0, max_reconnect_delay)  # Double backoff instead of 1.5x
					continue

			# Read one complete line (JSON message terminated with \n)
			line = ser.readline().decode(errors="replace").strip()
			if not line:
				continue

			# Parse JSON
			try:
				msg = json.loads(line)
			except json.JSONDecodeError:
				log.debug("Invalid JSON from serial: %s", line)
				continue

			op = msg.get("op")
			topic = msg.get("topic")
			payload = msg.get("payload")

			log.info("Serial tunnel: op=%s topic=%s payload=%s", op, topic, payload)

			# --- gateway_boot ---
			if op == "gateway_boot":
				if topic and mqtt_client:
					mqtt_client.publish(topic, json.dumps({"status": "boot", "fw": payload}))
					log.info("Gateway boot announced: %s", payload)

			# --- client_connect ---
			elif op == "client_connect":
				# payload = node IP address (e.g. "192.168.4.2")
				if mqtt_client:
					discovery_msg = {
						"mac": payload,
						"ip": payload,
						"status": "factory",
						"fw": "factory",
					}
					mqtt_client.publish("system/discovery", json.dumps(discovery_msg))
					log.info("Node discovery: %s", payload)

			# --- mqtt_connect ---
			elif op == "mqtt_connect":
				if mqtt_client:
					log.info("Node MQTT connected")
					mqtt_client.publish("system/discovery", json.dumps({"event": "mqtt_connected"}))

			# --- publish ---
			elif op == "publish":
				if topic and mqtt_client:
					mqtt_client.publish(topic, payload or "")
					log.debug("Bridged publish: %s", topic)

			# --- client_disconnect ---
			elif op == "client_disconnect":
				if mqtt_client:
					log.info("Node MQTT disconnected")
					mqtt_client.publish("system/discovery", json.dumps({"event": "mqtt_disconnected"}))

		except (serial.SerialException, OSError) as e:
			log.warning("Serial port lost: %s, will reconnect...", e)
			if ser:
				try:
					ser.close()
				except Exception:
					pass
			ser = None
			port = None
			reconnect_delay = min(reconnect_delay * 1.5, max_reconnect_delay)
			time.sleep(1)
		except Exception as e:
			log.error("Serial reader error: %s", e)
			if ser:
				try:
					ser.close()
				except Exception:
					pass
			ser = None
			time.sleep(2)


# ---------------------------------------------------------------------------
# WebSocket server
# ---------------------------------------------------------------------------

async def handle_ws(ws: ServerConnection):
    async with ws_lock:
        ws_clients.add(ws)

    log.info("WebSocket client connected (%d total)", len(ws_clients))

    try:
        await ws.send(json.dumps({
            "type": "snapshot",
            "nodes": registry.all(),
            "anomalies": list(anomaly_feed[-20:]),
        }))
    except Exception:
        pass

    try:
        async for raw in ws:
            try:
                msg = json.loads(raw)
            except Exception:
                continue
            await handle_ws_message(ws, msg)
    except Exception:
        pass
    finally:
        async with ws_lock:
            ws_clients.discard(ws)
        log.info("WebSocket client disconnected (%d total)", len(ws_clients))


async def handle_ws_message(ws: ServerConnection, msg: dict):
    """Handle messages sent from the browser to the backend."""
    kind = msg.get("type")

    if kind == "ping":
        await ws.send(json.dumps({"type": "pong"}))

    elif kind == "get_nodes":
        await ws.send(json.dumps({
            "type": "snapshot",
            "nodes": registry.all(),
            "anomalies": list(anomaly_feed[-20:]),
        }))

    elif kind == "publish":
        topic   = msg.get("topic", "")
        payload = msg.get("payload", {})
        if topic and mqtt_client:
            mqtt_client.publish(topic, json.dumps(payload))
            log.info("Browser publish: %s", topic)

    elif kind == "send_command":
        node_id = msg.get("node_id", "")
        channel = msg.get("channel", "")
        payload = msg.get("payload", {})
        if node_id and channel and mqtt_client:
            topic = f"nodes/{node_id}/cmd/{channel}"
            mqtt_client.publish(topic, json.dumps(payload))
            log.info("Browser command: %s", topic)
            await ws.send(json.dumps({
                "type": "command_sent",
                "node_id": node_id,
                "channel": channel,
                "payload": payload,
                "ts": time.time(),
            }))

    elif kind == "chat_message":
        # { type: "chat_message", conv_id: "conv-1", message: "...", node_id: "imu-01", title: "...", kind: "..." }
        conv_id      = msg.get("conv_id", "default")
        user_message = msg.get("message", "").strip()
        node_id      = msg.get("node_id", "")
        title        = msg.get("title", "Conversation")
        conv_kind    = msg.get("kind", "other")
        if not user_message:
            return
        
        # Initialize conversation metadata on first message
        _set_conv_metadata(conv_id, title=title, kind=conv_kind, related_nodes=[node_id] if node_id else [])
        
        node_context = None
        if node_id:
            all_nodes = registry.all()
            node_context = next(
                (n for n in all_nodes if n.get("node_id") == node_id or n.get("mac") == node_id),
                None,
            )
        asyncio.create_task(stream_claude(ws, conv_id, user_message, node_context))

    elif kind == "compile":
        # { type: "compile", conv_id: "conv-1", env_name: "imu_node", source_code: "..." }
        conv_id     = msg.get("conv_id", "default")
        env_name    = re.sub(r"[^a-zA-Z0-9_]", "_", msg.get("env_name", "").strip())
        source_code = msg.get("source_code", "").strip()
        if not env_name or not source_code:
            await ws.send(json.dumps({"type": "compile_done", "conv_id": conv_id,
                                       "success": False, "error": "env_name and source_code required"}))
            return
        asyncio.create_task(compile_firmware(ws, env_name, source_code, conv_id))

    elif kind == "push_ota":
        # { type: "push_ota", node_id: "imu-01", bin_url: "http://...", conv_id: "conv-1" }
        node_id = msg.get("node_id", "").strip()
        bin_url = msg.get("bin_url", "").strip()
        conv_id = msg.get("conv_id", "").strip()
        if node_id and bin_url:
            ok = push_ota(node_id, bin_url, conv_id=conv_id if conv_id else None)
            await ws.send(json.dumps({
                "type": "ota_sent",
                "node_id": node_id,
                "bin_url": bin_url,
                "success": ok,
                "ts": time.time(),
            }))

    elif kind == "clear_conv":
        conv_id = msg.get("conv_id", "")
        with conv_lock:
            conv_sessions.pop(conv_id, None)

    elif kind == "get_part_library":
        # Browser requests part library (for UI display)
        library = load_part_library()
        await ws.send(json.dumps({
            "type": "part_library",
            "parts": library.get("parts", {}),
            "ontology_keys": list(library.get("capability_ontology", {}).keys()),
        }))

    elif kind == "get_layout":
        # Browser requests current robot layout
        layout = load_layout()
        await ws.send(json.dumps({
            "type": "layout",
            "robot_name": layout.get("robot", {}).get("name", "unnamed"),
            "peripherals": layout.get("peripherals", {}),
        }))

    elif kind == "search_parts":
        # { type: "search_parts", query: "IMU" }
        query = msg.get("query", "").strip().lower()
        library = load_part_library()
        results = []
        for part_id, part_data in library.get("parts", {}).items():
            if query in part_data.get("name", "").lower() or query in part_data.get("model", "").lower():
                results.append({"id": part_id, "name": part_data.get("name"), "model": part_data.get("model"), "type": part_data.get("type")})
        await ws.send(json.dumps({
            "type": "search_results",
            "query": query,
            "results": results,
        }))

    elif kind == "get_history":
        # Get list of past conversations with optional filters
        # { type: "get_history", limit: 50 }
        limit = msg.get("limit", 50)
        results = _search_conversations(query="", kind="", node_id="", limit=limit)
        await ws.send(json.dumps({
            "type": "history",
            "conversations": results,
        }))

    elif kind == "search_conversations":
        # Search past conversations
        # { type: "search_conversations", query: "GPS", kind: "", node_id: "", limit: 50 }
        query = msg.get("query", "").strip()
        kind = msg.get("kind", "").strip()
        node_id = msg.get("node_id", "").strip()
        limit = msg.get("limit", 50)
        results = _search_conversations(query=query, kind=kind, node_id=node_id, limit=limit)
        await ws.send(json.dumps({
            "type": "search_results",
            "query": query,
            "conversations": results,
        }))

    elif kind == "get_conversation":
        # Load a full past conversation
        # { type: "get_conversation", conv_id: "conv-1" }
        conv_id = msg.get("conv_id", "")
        with conv_lock:
            history = list(conv_sessions.get(conv_id, []))
            meta = conv_metadata.get(conv_id, {})
        
        conv_data = {
            "id": conv_id,
            "title": meta.get("title", "Untitled"),
            "kind": meta.get("kind", "other"),
            "created": meta.get("created"),
            "closed": meta.get("closed"),
            "related_nodes": meta.get("related_nodes", []),
            "messages": history,
            "code_blocks": meta.get("code_blocks", []),
            "tasks": meta.get("tasks", []),
            "summary": meta.get("summary"),
        }
        await ws.send(json.dumps({
            "type": "conversation",
            "data": conv_data,
        }))

    elif kind == "get_node_files":
        # Get source files for a node (gateway or firmware node)
        # { type: "get_node_files", node_id: "gateway" }
        node_id = msg.get("node_id", "")
        files = []
        
        log.info(f"[get_node_files] Requesting files for node_id={node_id}")
        log.info(f"[get_node_files] FIRMWARE_NODES_DIR={FIRMWARE_NODES_DIR}")
        
        # For gateway, show gateway main.cpp
        if node_id == "gateway":
            gateway_main = FIRMWARE_NODES_DIR / "src" / "gateway" / "main.cpp"
            log.info(f"[get_node_files] Checking gateway path: {gateway_main}")
            log.info(f"[get_node_files] Gateway path exists: {gateway_main.exists()}")
            if gateway_main.exists():
                try:
                    with open(gateway_main, 'r') as f:
                        content = f.read()
                        files.append({
                            "name": "src/gateway/main.cpp",
                            "content": content,
                        })
                    log.info(f"[get_node_files] Loaded gateway main.cpp: {len(content)} bytes")
                except Exception as e:
                    log.error(f"[get_node_files] Error reading gateway main.cpp: {e}")
            else:
                log.warning(f"[get_node_files] Gateway main.cpp not found at {gateway_main}")
        
        # For other nodes, show their main.cpp
        else:
            node_dir = FIRMWARE_NODES_DIR / "src" / node_id
            log.info(f"[get_node_files] Checking node path: {node_dir}")
            if node_dir.exists():
                main_cpp = node_dir / "main.cpp"
                log.info(f"[get_node_files] Node main.cpp exists: {main_cpp.exists()}")
                if main_cpp.exists():
                    try:
                        with open(main_cpp, 'r') as f:
                            content = f.read()
                            files.append({
                                "name": f"src/{node_id}/main.cpp",
                                "content": content,
                            })
                        log.info(f"[get_node_files] Loaded node main.cpp: {len(content)} bytes")
                    except Exception as e:
                        log.error(f"[get_node_files] Error reading node main.cpp: {e}")
            else:
                log.warning(f"[get_node_files] Node directory not found at {node_dir}")
        
        log.info(f"[get_node_files] Sending {len(files)} files to client")
        await ws.send(json.dumps({
            "type": "node_files",
            "node_id": node_id,
            "files": files,
            "selected_file": files[0]["name"] if files else None,
        }))



async def run_ws_server():
    """Start the WebSocket server and background tasks."""
    global _loop
    _loop = asyncio.get_running_loop()

    log.info("WebSocket server listening on ws://0.0.0.0:%d", WS_PORT)
    try:
        async with websockets.serve(handle_ws, "0.0.0.0", WS_PORT, reuse_address=True):
            # Age-out check every 30s
            while True:
                await asyncio.sleep(30)
                registry.mark_offline()
    except OSError as e:
        if "Address already in use" in str(e) or "10048" in str(e):
            log.error("Port %d is already in use. Try:", WS_PORT)
            log.error("  - Change WS_PORT environment variable: export WS_PORT=8766")
            log.error("  - Or kill existing server: lsof -i :%d | grep -v PID | awk '{print $2}' | xargs kill", WS_PORT)
        else:
            log.error("Failed to start WebSocket server: %s", e)
        raise


# ---------------------------------------------------------------------------
# Static file HTTP server (serves the frontend)
# ---------------------------------------------------------------------------

class StaticHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(STATIC_DIR), **kwargs)

    def log_message(self, format, *args):
        pass  # suppress per-request logs


def run_http_server():
    httpd = HTTPServer(("0.0.0.0", HTTP_PORT), StaticHandler)
    log.info("HTTP server listening on http://0.0.0.0:%d", HTTP_PORT)
    httpd.serve_forever()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

mqtt_client = None

def main():
    global mqtt_client

    if not STATIC_DIR.exists():
        STATIC_DIR.mkdir(parents=True)
        log.warning("Created empty static/ directory")

    # Load past conversations from disk
    _load_conversations_from_disk()
    log.info("Loaded conversations from disk")

    mqtt_client = start_mqtt()

    # Serial reader (USB tunnel from gateway) in a background thread
    serial_thread = threading.Thread(target=start_serial_reader, daemon=True)
    serial_thread.start()
    log.info("Serial reader thread started")

    # HTTP server in a background thread
    http_thread = threading.Thread(target=run_http_server, daemon=True)
    http_thread.start()

    # WebSocket server owns the asyncio event loop
    try:
        asyncio.run(run_ws_server())
    except KeyboardInterrupt:
        log.info("Shutting down")



if __name__ == "__main__":
    main()

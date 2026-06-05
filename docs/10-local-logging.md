# Local Logging and Post-Rollback Diagnostics

This document describes Parley's per-node logging subsystem: how logs are written locally on each ESP32, how they survive across firmware rollbacks, and how they enable the AI agent to diagnose problems that crashed the firmware before it could explain itself.

Read `02-node-firmware-design.md` for the universal template and `03-recovery-and-resilience.md` for the rollback architecture this system supports.

## Why Local Logging Matters

The Parley recovery cascade is good at recovering from failures but on its own produces little explanation. When a firmware update is rolled back automatically, the Pi sees "the node went from version X to version X-1" but typically has no useful information about why the new firmware failed. The handful of log lines that made it through MQTT before the crash are fragmentary — by definition, the firmware crashed before it could fully explain itself.

This gap matters because it weakens the AI agent's diagnostic role. Without insight into what the failed firmware was doing in its final moments, the agent can only guess at the cause. With access to the failed firmware's local log file, the agent has real diagnostic material to reason about.

The solution is straightforward: write logs locally to a partition that survives across firmware updates. After a rollback, the recovered firmware reads the failed firmware's log file and reports its contents to the Pi.

## Where Logs Live

Each ESP32 has a LittleFS filesystem in its own dedicated partition (see the partition layout in `02-node-firmware-design.md`). LittleFS is not touched by OTA updates — only the app slot partitions are written to during a firmware push. This is the key enabling property: log files written by one firmware version persist when a different firmware version takes over.

A portion of the LittleFS partition is reserved for logs:

| File | Purpose | Lifecycle |
|------|---------|-----------|
| `/logs/current.log` | Active log file, being written to by the running firmware | Rotated when size limit reached or on each boot |
| `/logs/previous.log` | The most recently rotated log file, available for inspection | Overwritten on next rotation |
| `/logs/meta.json` | Small metadata file: schema version, last rotation time, boot count when rotated | Updated on rotation |

The default size limit for each log file is 256 KB. With two files, total log storage is roughly 512 KB — a small fraction of the ~5 MB available in the LittleFS partition.

## Log Format

Each log entry is a single line of JSON. This format is compact enough for embedded use, machine-parseable by the AI agent, and human-readable for direct inspection. Example entries:

```json
{"t":47.312,"l":"INFO","tag":"template.wifi","msg":"WiFi connected, RSSI -52"}
{"t":47.481,"l":"INFO","tag":"template.mqtt","msg":"MQTT connected to broker"}
{"t":48.103,"l":"INFO","tag":"app.encoder","msg":"Encoder initialized on GPIO34/35"}
{"t":48.892,"l":"ERROR","tag":"app.encoder","msg":"Encoder read timeout","details":{"register":0x02,"expected":4,"received":0}}
```

Each entry has:

- `t`: timestamp as seconds since boot, with millisecond precision
- `l`: log level (DEBUG, INFO, WARN, ERROR, FATAL)
- `tag`: source identifier, by convention `<subsystem>.<component>` (e.g., `template.wifi`, `app.encoder`)
- `msg`: human-readable message
- `details` (optional): a JSON object with structured additional data

The format is forward-compatible — fields can be added without breaking parsers, and unknown fields are ignored on reading.

## Boot Markers

The first entry written after each boot is a structured boot marker:

```json
{"t":0.001,"l":"INFO","tag":"template.boot","msg":"BOOT","details":{"version":"1.4.2-7c3a4b1","slot":"A","reset_reason":"ESP_RST_TASK_WDT","free_heap":234567,"boot_count":1}}
```

This makes it easy to find boundaries between boots in a log file and immediately see the context of the boot (firmware version, which slot, why it restarted).

## The Logging API

The universal template exposes a single logging function to application code:

```cpp
// Write a log entry to the local circular buffer.
// Buffered in RAM, flushed to flash periodically.
void node_log_local(LogLevel level, const char* tag, const char* fmt, ...);

// Write a log entry with structured details.
void node_log_local_with_details(LogLevel level, const char* tag,
                                  const JsonDocument& details,
                                  const char* fmt, ...);
```

The template itself uses this same API to log infrastructure events (boot, WiFi state changes, MQTT state changes, OTA progress, recovery actions). Application code uses it for peripheral-specific events.

Levels follow standard convention:

- **DEBUG** — verbose detail, not normally interesting; useful when investigating a specific issue
- **INFO** — normal operational events worth recording
- **WARN** — something unexpected happened but the system continued
- **ERROR** — something failed; the system may continue but with reduced capability
- **FATAL** — unrecoverable problem; the system is about to crash or restart

The template can be configured to suppress logs below a threshold (default: INFO). DEBUG logs are skipped at production verbosity to reduce flash wear and storage use.

## Buffering and Flushing

Writes to flash are expensive — they consume time, power, and contribute to flash wear. The logging subsystem buffers in RAM and flushes to flash periodically.

Buffer behavior:

- An in-RAM ring buffer (default: 8 KB) accumulates log entries.
- Entries are flushed to `/logs/current.log` at an interval determined by the node's stability age (see Graduated Verbosity below). Default flush intervals range from 2 seconds during initial validation up to 60 seconds for long-stable firmware.
- ERROR and FATAL entries trigger an immediate flush regardless of interval.
- When the buffer fills, the oldest entries are flushed and overwritten.

This means a sudden crash can lose up to the flush interval of recent log entries. The mitigations:

**Panic handler flush.** The template registers a panic handler that attempts to flush the buffer to flash before the chip resets. This works for software panics, stack overflows, and most ESP-IDF abort conditions. It does not work if the chip is wedged in a way that prevents any code from running.

**Watchdog-triggered flush.** If practical, a watchdog warning hook flushes the buffer before the chip is reset. This works when the watchdog detects a hang far enough in advance.

**Immediate flush before risky operations.** Application code can call `node_log_flush()` before doing something it suspects might crash. Useful when calling new or poorly-tested libraries.

These cover most cases. The remaining edge cases (instant hardware failure, true silent hang) leave a gap, but the rotated `previous.log` from the prior boot is still available, and the boundary where logging stopped is itself diagnostic information.

## Rotation

The log file rotates in two situations:

**On every boot.** When the firmware starts, it renames the existing `/logs/current.log` to `/logs/previous.log` (deleting the older `previous.log` first if one exists), then creates a new empty `/logs/current.log`.

This is what makes post-rollback diagnostics work. The failed firmware's last activities end up in `previous.log` after the recovered firmware boots and rotates.

**On size limit.** When `/logs/current.log` reaches the size limit (default 256 KB), the same rotation happens — current becomes previous, a new current is created.

The rotation is atomic enough to survive power loss: `meta.json` is updated last, and an interrupted rotation can be cleanly recovered or discarded on the next boot.

## Post-Rollback Reporting

This is where the system earns its complexity. When the recovered firmware boots after a rollback:

1. It detects that a rollback occurred (the OTA status indicates the previous boot's firmware was invalidated).
2. It reads `/logs/previous.log` — which contains the failed firmware's activity right up until it crashed or failed to validate.
3. It publishes a summary to `system/recovery` on MQTT:

```json
{
  "node_id": "imu-01",
  "event": "rollback_diagnosis",
  "rolled_back_from": "1.4.3-9b22f1e",
  "current_version": "1.4.2-7c3a4b1",
  "log_size_bytes": 12480,
  "log_entries": 184,
  "last_log_timestamp": 47.892,
  "last_log_level": "ERROR",
  "last_log_tag": "app.encoder",
  "last_log_msg": "Encoder read timeout"
}
```

4. It then publishes the full log file contents in chunks to `system/recovery/log` — typically as base64-encoded segments of a few KB each, with a final "end" marker. The Pi reassembles the full log for the AI agent to read.

5. After successful transmission, the recovered firmware marks the log as "consumed" by writing to `meta.json`. On subsequent reboots, the consumed log is not republished.

The AI agent now has:

- The fact that a rollback happened
- Which version failed and which version is running now
- A summary highlighting the most likely diagnostic clue (the last ERROR or the entry immediately before things went silent)
- The full log file for deeper investigation if needed

A diagnostic conversation can begin immediately: the agent reads the log, forms a hypothesis, proposes a fix, and the cycle continues.

## On-Demand Log Retrieval

Beyond post-rollback reporting, the AI agent can request logs from any node at any time. The standard MQTT command interface includes:

| Topic | Action |
|-------|--------|
| `nodes/<node_id>/cmd/log_dump` | Publish the contents of `current.log` |
| `nodes/<node_id>/cmd/log_dump_previous` | Publish the contents of `previous.log` |
| `nodes/<node_id>/cmd/log_clear` | Clear `current.log` and `previous.log`, reset `meta.json` |
| `nodes/<node_id>/cmd/log_set_level` | Change runtime log level (DEBUG/INFO/WARN/ERROR) |

The dumps publish to `nodes/<node_id>/data/log` in the same chunked format as post-rollback reports. The AI agent can ask for logs proactively when investigating any issue, not just rollbacks.

`log_set_level` is useful for temporarily increasing verbosity during diagnosis. The agent can set a node to DEBUG, wait for the next occurrence of the problem being investigated, retrieve the log, and set the level back to INFO. This produces the deep visibility that a special "diagnostic firmware" would otherwise require, without actually deploying different firmware.

## Graduated Verbosity Based on Stability Age

Continuous verbose logging across a node's entire operational life would produce unnecessary flash wear during stable periods when diagnostic value is low. The logging subsystem addresses this by tracking each firmware's "stability age" — how long the current firmware has been validated and running healthily — and adjusting default log behavior accordingly.

The stability age is reset whenever a new firmware version starts running. It increases as long as the firmware continues operating without crashes, rollbacks, or significant errors.

Default behavior as stability age increases:

| Stability age | Default log level | Flush interval |
|---------------|-------------------|----------------|
| 0 to 5 minutes (during validation) | DEBUG | 2 seconds |
| 5 minutes to 24 hours | INFO | 5 seconds |
| 24 hours to 7 days | INFO | 30 seconds |
| 7 days and older | WARN | 60 seconds |

The thresholds are configurable. The principle behind them: recently-deployed firmware has high failure probability and high diagnostic value; long-stable firmware has low failure probability and lower routine diagnostic value, but still needs to capture meaningful events when they occur.

The flush interval changes matter more for flash wear than the log level changes. Going from 5-second flushes to 60-second flushes reduces flash write rate by 12x while still preserving the entries themselves.

### What Always Logs at Full Verbosity

Regardless of stability age, certain events always log at their actual level and flush immediately:

- ERROR and FATAL entries — always recorded, always flushed
- Boot markers — every boot has a clear starting point in the log
- The validation gate's activity — recovery cascade behavior is always traceable
- OTA progress and outcomes — firmware transitions are always logged
- Any application code that explicitly calls `node_log_local()` at higher levels

This means the wear reduction operates only on routine INFO-level chatter, which is the most expendable category. Anything that genuinely indicates a problem still gets captured fully.

### AI-Triggered Override

The graduated defaults can be overridden at any time using the `log_set_level` command (described below). When the AI agent suspects a problem on a long-stable node, it can raise the level back to DEBUG, wait for the next occurrence of the symptom, retrieve the detailed log, and lower the level again.

This pattern preserves diagnostic capability for problems that emerge after long stability: when an anomaly indicator appears (irregular heartbeats, unexpected MQTT reconnections, increasing watchdog resets), the AI proactively increases verbosity even before symptoms reach the WARN threshold. Steady-state operation is cheap; investigation has full power.

### Honest Trade-off

Graduated verbosity has a real cost. A node that has been stable for two months will have only WARN-and-above entries available when something starts going wrong. The first hours of "something is off but not yet alarming" may not be captured at INFO level, leaving a gap in the diagnostic record.

This trade-off is acceptable for two reasons. First, flash wear is real and a system designed to run for years cannot afford continuous high-rate writes. Second, the AI's ability to raise verbosity on demand, combined with the unconditional logging of error-level events, provides recovery paths for most situations where the gap matters.

The alternative — always logging verbosely — burns flash for diagnostic value that is rarely used. The alternative — stopping logging entirely after validation — closes the door on diagnosing any post-validation problem. Graduated verbosity balances these against each other.

## Privacy and Sensitive Information

Logs may inadvertently capture sensitive information — sensor readings that reveal location, command sequences that reveal usage patterns, debug output containing credentials or keys. A few practices reduce risk:

- Never log credentials, tokens, or secret keys. The template's WiFi connection routine logs "connected" but not the password.
- Avoid logging full sensor readings at INFO level. Use DEBUG for verbose data dumps that are only enabled during investigation.
- When transmitting log files over MQTT, the same trust assumptions apply as for any other MQTT traffic — the system is on a private network behind the gateway.

These are concerns to be aware of but do not change the architecture. Discipline in what gets logged is the main mitigation.

## What This Does Not Provide

Local logging closes the gap between "the firmware crashed" and "we know what it was doing when it crashed." It does not address:

- **Pre-existing problems that survived rollback.** If the rolled-back firmware also has a latent bug, the logs from the failed firmware do not help diagnose that.
- **Hardware-caused crashes.** A power glitch that causes a reset will appear as a watchdog timeout or external reset with no preceding ERROR entry. The log just stops. This is correct — there is nothing software-level to diagnose.
- **Issues that the firmware does not log.** Application code that fails silently (a sensor returning bad data that is treated as valid) leaves no log trail. The remedy is to log defensively at suspect boundaries; the architecture cannot force this.
- **Cross-node correlation.** Each node has its own log. A multi-node interaction that produces a problem requires the AI agent to correlate logs across nodes. The Pi can do this since it sees all nodes' MQTT traffic, but it is correlation work the agent must do explicitly.

These limits are real but acceptable. The logging subsystem dramatically improves the AI agent's ability to debug software problems, which is the largest class of issues during active development. Other issues need other approaches.

## Implementation Order

This subsystem is significant enough that incremental implementation makes sense:

**Phase 1 (essential):** Basic `node_log_local()` API writing JSON lines to `/logs/current.log`. Boot rotation. Boot markers. ERROR-triggered flushes.

**Phase 2 (rollback diagnostics):** Post-rollback detection and reporting on `system/recovery`. Reading `previous.log` and publishing summary plus full content.

**Phase 3 (on-demand retrieval):** MQTT commands for `log_dump`, `log_dump_previous`, `log_clear`. Chunked transmission.

**Phase 4 (verbosity control):** `log_set_level` command. Runtime adjustment without firmware update.

**Phase 5 (graduated verbosity):** Stability-age tracking. Automatic level and flush-interval adjustment based on how long the firmware has been validated and stable. Reduces flash wear during long stable periods while preserving full verbosity during initial validation.

**Phase 6 (refinements):** Panic handler flush. Watchdog warning flush. Other wear-rate tuning based on observed behavior.

Each phase produces value on its own. Phase 1 alone improves debuggability significantly even without the cross-rollback piece. Phase 2 is where the unique value lands — diagnostics surviving the failure. Phase 5 makes the subsystem deployable in long-running operational contexts where flash wear would otherwise be a concern.

## Summary

Local logging on LittleFS gives every node a small, persistent record of what it was doing. Because LittleFS is not touched by OTA updates, the log survives firmware rollbacks. After a rollback, the recovered firmware publishes the failed firmware's log to the Pi, giving the AI agent real diagnostic material to reason about.

This closes a gap in the recovery architecture: failures no longer recover silently. The system now both recovers from failures and explains them.

The subsystem is straightforward to implement, lives within the universal template, and requires no special "diagnostic mode" firmware. Logging is always on, always available, and uniformly accessible across all nodes.

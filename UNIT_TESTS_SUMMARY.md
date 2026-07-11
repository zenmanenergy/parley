# Unit Tests Summary

Created comprehensive unit tests for the Parley dashboard and firmware.

**Last Run:** 2026-07-10  
**Total Tests:** 132 (all passing ✓)

## Backend Tests (Python) ✅

**Files:** `pi/dashboard/test_server.py` and `pi/dashboard/test_integration.py`  
**Framework:** pytest  
**Tests:** 99 (all passing ✓)

### Test Coverage by Function (test_server.py)

1. **extract_code_blocks()** — 8 tests
   - Empty/missing code blocks
   - Single/multiple blocks
   - Language detection
   - File annotations

2. **extract_tasks()** — 10 tests
   - Task/ACTION/TODO prefix formats
   - Checkbox format [ ]
   - Multiple tasks
   - Timestamp validation
   - Unique IDs
   - Case-insensitive parsing

3. **extract_commands()** — 9 tests ✨ NEW
   - COMMAND/CMD markers
   - Single/multiple commands
   - JSON payload parsing
   - Error handling for invalid JSON
   - Unique IDs and timestamps
   - Case-insensitive markers

4. **extract_reasoning()** — 9 tests ✨ NEW
   - Reasoning block parsing
   - Field extraction (observed, hypothesis, etc.)
   - Confidence level validation
   - Multiple reasoning blocks
   - Full text preservation
   - Case-insensitive markers

5. **load_part_library()** — 4 tests
   - File existence
   - Valid/invalid JSON
   - Error handling

6. **load_layout()** — 4 tests
   - YAML parsing
   - File existence
   - Optional yaml module handling

7. **get_relevant_part_entries()** — 7 tests
   - Empty/missing library
   - Substring matching
   - Case-insensitive search
   - Multiple results
   - No matches

8. **push_anomaly()** — 3 tests
   - Anomaly addition
   - Max queue size
   - Timestamp validation

9. **Conversation helpers** — 5 tests
   - History retrieval
   - Message appending
   - Session isolation
   - Copy semantics

10. **push_ota()** — 2 tests
   - MQTT publishing
   - Message format

11. **NodeRegistry class** — 13 tests
   - Node upsert operations
   - Node updates by ID
   - Connectivity status calculation
   - Thread safety
   - Multiple node handling
   - Sorted list retrieval
   - State preservation

### Integration Tests (test_integration.py) — 24 tests

- WebSocket message routing (3 tests)
- MQTT subscription and publishing (3 tests)
- Claude AI conversation flow (3 tests)
- Error collection and fix workflow (3 tests)
- Recovery log handling (3 tests)
- Error handling and edge cases (3 tests)
- Part library and layout context injection (2 tests)
- Telemetry handling (2 tests)
- Node discovery and OTA updates (2 tests)

### Running Backend Tests

```bash
cd pi/dashboard
pip install -r requirements-dev.txt
pytest test_server.py test_integration.py -v
```

### Coverage

- Pure functions: ~75% coverage
- MQTT/WebSocket-dependent code: Integration tests with mocks
- Async functions: Proper handling with pytest-asyncio
- Node registry: Thread-safety verified

---

## Frontend Tests (JavaScript) ✅

**File:** `pi/dashboard/static/app.test.js`  
**Framework:** Jest  
**Tests:** 33 (all passing ✓)

### Test Coverage by Function

1. **escapeHtml()** — 8 tests
   - All HTML entities (&, <, >, ", ')
   - Multiple escapes
   - Empty strings
   - Safe characters

2. **buildTaskItem()** — 4 tests
   - Task properties
   - Status emoji mapping
   - Timestamp formatting
   - Unknown status

3. **getTelemetryChannels()** — 4 tests
   - Missing node
   - All channels
   - Empty channels
   - Node isolation

4. **escRegex()** — 6 tests
   - Dot, asterisk, parentheses, brackets, backslash
   - Multiple special characters

5. **formatTelemetryValue()** — 5 tests
   - Number formatting (2 decimals)
   - Object stringification
   - String conversion
   - Boolean/null handling

6. **findChannelNamesInText()** — 6 tests
   - Finding channels in text
   - Case insensitivity
   - Word boundaries
   - Empty edge cases

### Running Frontend Tests

```bash
cd pi/dashboard
npm install --save-dev jest @babel/core @babel/preset-env
npm test
```

### Configuration

- **jest.config.js** — Jest configuration with coverage thresholds
- **Coverage target:** 40% for frontend (extracted functions only)
- **Test environment:** jsdom

---

## Firmware Tests (C++) 🚧

**File:** `firmware/nodes/TESTING.md`
**Framework:** Unity (lightweight ESP32-compatible)
**Status:** Test plan defined, ready for implementation

### Test Categories Planned

1. **Recovery Cascade** (4 layers) — 15 tests
2. **Network** (WiFi/MQTT) — 8 tests
3. **Local Logging** — 12 tests
4. **CAN Bus** — 10 tests
5. **NVS Persistence** — 4 tests
6. **Heartbeat/Status** — 4 tests
7. **Command Handling** — 5 tests
8. **Optional Callbacks** — 4 tests
9. **Integration** — 5 tests

**Total:** ~70 firmware tests (not yet implemented)

---

## Summary

| Component | Tests | Status |
|-----------|-------|--------|
| Backend (Python) | 99 | ✅ All passing |
| Frontend (JavaScript) | 33 | ✅ All passing |
| Firmware (C++) | 0/~70 | 🚧 Plan only |
| **TOTAL** | **132** | **✅ All passing** |

### Latest Test Results (2026-07-10)

```
Backend: 99 passed in 0.41s
Frontend: 33 passed in 1.43s
Total: 132 tests passing ✓
```

### Recent Additions

- ✨ **extract_commands()** tests (9 tests) — Parse command blocks from Claude
- ✨ **extract_reasoning()** tests (9 tests) — Parse reasoning blocks for transparency
- ✨ **Integration tests** (24 tests) — Full system workflows including WebSocket, MQTT, Claude, and recovery

### Implementation Notes

- Requires ESP-IDF 5.x toolchain
- PlatformIO test support
- Can run on hardware or simulator
- Example test structure provided
- Configuration template in TESTING.md

---

## Quick Start

### Python Backend Tests
```bash
cd pi/dashboard
pip install -r requirements-dev.txt
pytest test_server.py -v
pytest test_server.py --cov=server --cov-report=html  # With coverage
```

### JavaScript Frontend Tests
```bash
cd pi/dashboard
npm install --save-dev jest @babel/core @babel/preset-env
npm test
npm run test:coverage
```

### Firmware Tests (future)
```bash
cd firmware/nodes
platformio test -e test
```

---

## Test Metrics

| Component | Tests | Status | Coverage |
|-----------|-------|--------|----------|
| Backend (Python) | 44 | ✅ Passing | ~75% |
| Frontend (JS) | 30+ | ✅ Ready | 40%+ |
| Firmware (C++) | 70 | 📋 Planned | TBD |
| **Total** | **144+** | **Partial** | **~50%** |

---

## Next Steps

1. **Run backend tests in CI/CD**
   ```bash
   pytest test_server.py --cov=server --cov-report=term
   ```

2. **Run frontend tests in CI/CD**
   ```bash
   npm test -- --coverage
   ```

3. **Implement firmware tests**
   - Create `firmware/nodes/test/` directory
   - Add Unity test files
   - Configure PlatformIO test environment
   - Run on hardware or simulator

4. **Add integration tests**
   - WebSocket message routing
   - MQTT message handling
   - Full conversation flow
   - Compilation and OTA process

5. **Add E2E tests**
   - Browser automation (Selenium/Puppeteer)
   - Full UI workflows
   - Multi-node scenarios

---

## Documentation

- **Backend:** `pi/dashboard/TESTING.md`
- **Firmware:** `firmware/nodes/TESTING.md`
- **Setup:** See `requirements-dev.txt` and `jest.config.js`

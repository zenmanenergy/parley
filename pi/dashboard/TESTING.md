# Unit Tests for Parley Dashboard

This directory contains unit tests for both the Python backend (`server.py`) and JavaScript frontend (`app.js`).

## Quick Start

### Python Backend Tests (pytest)

Install test dependencies:
```bash
pip install pytest pytest-cov
```

Run tests:
```bash
cd pi/dashboard
pytest test_server.py -v
```

Run with coverage report:
```bash
pytest test_server.py -v --cov=server --cov-report=html
```

### JavaScript Frontend Tests (Jest)

Install test dependencies:
```bash
cd pi/dashboard
npm install --save-dev jest @babel/core @babel/preset-env
```

Run tests:
```bash
npm test
```

Watch mode (re-run on file changes):
```bash
npm run test:watch
```

With coverage report:
```bash
npm run test:coverage
```

---

## Test Structure

### Backend Tests (`test_server.py`)

Organized into test classes for each major function:

1. **TestExtractCodeBlocks** — Parsing code blocks from markdown
   - Single/multiple blocks, language detection, file annotations

2. **TestExtractTasks** — Task extraction from AI responses
   - Task prefixes (Task:, ACTION:, TODO:), checkbox format, uniqueness

3. **TestLoadPartLibrary** — Loading hardware part database
   - Valid/invalid JSON, file existence, error handling

4. **TestLoadLayout** — Loading robot spatial layout
   - YAML parsing, file existence, error handling, optional yaml module

5. **TestGetRelevantPartEntries** — Searching part library
   - Keyword matching, case-insensitivity, multiple results

6. **TestPushAnomaly** — Anomaly queue management
   - Timestamp addition, max queue size, thread safety

7. **TestConversationHelpers** — Conversation session management
   - History retrieval, message appending, isolation between conversations

8. **TestPushOTA** — OTA firmware push
   - MQTT publishing, message format

### Frontend Tests (`static/app.test.js`)

Pure function tests for dashboard logic:

1. **escapeHtml** — HTML entity escaping for XSS prevention
2. **buildTaskItem** — Task object construction with formatting
3. **getTelemetryChannels** — Extracting sensor channels for a node
4. **escRegex** — Escaping special regex characters
5. **formatTelemetryValue** — Number/object formatting for display
6. **findChannelNamesInText** — Finding sensor references in chat

---

## Running Specific Test Categories

### Run only Python tests:
```bash
pytest test_server.py::TestExtractCodeBlocks -v
pytest test_server.py::TestExtractTasks -v
```

### Run only JavaScript tests:
```bash
npm test -- app.test.js
npm test -- app.test.js -t "escapeHtml"
```

---

## Test Coverage Goals

- **Backend (server.py):** >70% coverage for pure functions
- **Frontend (app.js):** >60% coverage for extractable functions

Main file (`app.js`) has significant DOM and WebSocket dependencies, so full coverage is deferred to integration tests.

---

## Adding New Tests

### For Python (pytest):

1. Add a new test class in `test_server.py`:
```python
class TestNewFeature:
    """Tests for new_feature() function."""
    
    def test_basic_case(self):
        result = server.new_feature("input")
        assert result == "expected"
    
    def test_edge_case(self):
        result = server.new_feature("")
        assert result == []
```

2. Run the tests:
```bash
pytest test_server.py::TestNewFeature -v
```

### For JavaScript (Jest):

1. Add test cases in `static/app.test.js`:
```javascript
describe('newFunction', () => {
    test('should do something', () => {
        expect(newFunction('input')).toBe('output');
    });
});
```

2. Run:
```bash
npm test
```

---

## Continuous Integration

These tests can be integrated into CI/CD pipelines:

```bash
# In CI script
pytest test_server.py -v --cov=server --cov-report=term
npm test -- --coverage
```

---

## Known Limitations

1. **Server tests don't test async functions** — `stream_claude()` and `compile_firmware()` require WebSocket and subprocess mocking; recommend integration tests for these.

2. **Server tests don't test MQTT** — Connection, subscription, and message routing are tested in isolation; full integration test recommended.

3. **Frontend tests don't test DOM manipulation** — Real DOM interactions should be tested in Selenium/Puppeteer integration tests.

4. **No mocking of external services** — Anthropic API calls, PlatformIO execution are not tested; integration tests needed.

---

## Future Improvements

- Add tests for error handling in compilation
- Add tests for WebSocket message routing
- Add integration tests for full conversation flow
- Add E2E tests with Selenium for browser interactions
- Add performance benchmarks for large firmware files
- Add tests for CAN frame handling in firmware

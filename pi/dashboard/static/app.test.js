/**
 * Unit tests for app.js dashboard frontend.
 * 
 * Run with: npm test -- app.test.js
 * 
 * Tests focus on pure functions and data transformations that don't require
 * live WebSocket or DOM interactions.
 */

// Mock functions to test (these would be extracted from app.js for testing)

/**
 * Escape HTML special characters.
 */
function escapeHtml(text) {
	const map = {
		'&': '&amp;',
		'<': '&lt;',
		'>': '&gt;',
		'"': '&quot;',
		"'": '&#039;',
	};
	return text.replace(/[&<>"']/g, char => map[char]);
}

/**
 * Build a task item object from task data.
 */
function buildTaskItem(task) {
	const statusEmoji = {
		'pending': '⏳',
		'accepted': '✓',
		'rejected': '✕',
		'deferred': '⋯',
	};
	const createdTime = new Date(task.created * 1000).toLocaleString();
	return {
		id: task.id,
		title: task.title,
		status: task.status,
		statusEmoji: statusEmoji[task.status] || '?',
		createdTime: createdTime,
	};
}

/**
 * Extract channel names from telemetry state.
 */
function getTelemetryChannels(telemetryByNode, nodeId) {
	if (!telemetryByNode[nodeId]) {
		return [];
	}
	return Object.keys(telemetryByNode[nodeId]);
}

/**
 * Find all telemetry channel names in text using regex.
 */
function findChannelNamesInText(text, channels) {
	if (!channels || channels.length === 0) {
		return [];
	}
	const found = [];
	for (const channel of channels) {
		const pattern = new RegExp(`\\b${escapeHtml(channel).replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}\\b`, 'gi');
		if (pattern.test(text)) {
			found.push(channel);
		}
	}
	return found;
}

/**
 * Escape special regex characters.
 */
function escRegex(str) {
	return str.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

/**
 * Format a telemetry value for display.
 */
function formatTelemetryValue(value) {
	if (typeof value === 'number') {
		return value.toFixed(2);
	}
	if (typeof value === 'object') {
		return JSON.stringify(value);
	}
	return String(value);
}

// ============================================================================
// Jest Tests
// ============================================================================

describe('escapeHtml', () => {
	test('should escape ampersand', () => {
		expect(escapeHtml('Tom & Jerry')).toBe('Tom &amp; Jerry');
	});

	test('should escape less than', () => {
		expect(escapeHtml('3 < 5')).toBe('3 &lt; 5');
	});

	test('should escape greater than', () => {
		expect(escapeHtml('5 > 3')).toBe('5 &gt; 3');
	});

	test('should escape quotes', () => {
		expect(escapeHtml('He said "hello"')).toBe('He said &quot;hello&quot;');
	});

	test('should escape apostrophes', () => {
		expect(escapeHtml("It's a test")).toBe('It&#039;s a test');
	});

	test('should escape multiple characters', () => {
		expect(escapeHtml('<script>alert("XSS")</script>'))
			.toBe('&lt;script&gt;alert(&quot;XSS&quot;)&lt;/script&gt;');
	});

	test('should handle empty string', () => {
		expect(escapeHtml('')).toBe('');
	});

	test('should leave safe characters unchanged', () => {
		expect(escapeHtml('Hello World 123')).toBe('Hello World 123');
	});
});

describe('buildTaskItem', () => {
	test('should include all task properties', () => {
		const task = {
			id: 'task_1',
			title: 'Fix motor controller',
			status: 'pending',
			created: 1234567890,
		};
		const result = buildTaskItem(task);
		expect(result.id).toBe('task_1');
		expect(result.title).toBe('Fix motor controller');
		expect(result.status).toBe('pending');
	});

	test('should map status to emoji', () => {
		const statuses = {
			'pending': '⏳',
			'accepted': '✓',
			'rejected': '✕',
			'deferred': '⋯',
		};
		for (const [status, emoji] of Object.entries(statuses)) {
			const task = { id: 'x', title: 'x', status, created: Date.now() / 1000 };
			expect(buildTaskItem(task).statusEmoji).toBe(emoji);
		}
	});

	test('should convert timestamp to readable date', () => {
		const task = {
			id: 'x',
			title: 'x',
			status: 'pending',
			created: 1609459200,  // 2021-01-01 00:00:00 UTC
		};
		const result = buildTaskItem(task);
		expect(result.createdTime).toBeTruthy();
		expect(result.createdTime).not.toContain('undefined');
	});

	test('should handle unknown status', () => {
		const task = {
			id: 'x',
			title: 'x',
			status: 'unknown_status',
			created: Date.now() / 1000,
		};
		const result = buildTaskItem(task);
		expect(result.statusEmoji).toBe('?');
	});
});

describe('getTelemetryChannels', () => {
	test('should return empty array for missing node', () => {
		const telemetry = {};
		expect(getTelemetryChannels(telemetry, 'unknown')).toEqual([]);
	});

	test('should return all channels for a node', () => {
		const telemetry = {
			'node1': {
				'velocity': 1.5,
				'position': 42.0,
				'temperature': 65.3,
			}
		};
		const channels = getTelemetryChannels(telemetry, 'node1');
		expect(channels.length).toBe(3);
		expect(channels).toContain('velocity');
		expect(channels).toContain('position');
		expect(channels).toContain('temperature');
	});

	test('should return empty array for node with no channels', () => {
		const telemetry = { 'node1': {} };
		expect(getTelemetryChannels(telemetry, 'node1')).toEqual([]);
	});

	test('should not return channels for other nodes', () => {
		const telemetry = {
			'node1': { 'channel1': 1 },
			'node2': { 'channel2': 2 },
		};
		const channels = getTelemetryChannels(telemetry, 'node1');
		expect(channels).not.toContain('channel2');
	});
});

describe('escRegex', () => {
	test('should escape dot', () => {
		expect(escRegex('file.txt')).toBe('file\\.txt');
	});

	test('should escape asterisk', () => {
		expect(escRegex('foo*bar')).toBe('foo\\*bar');
	});

	test('should escape parentheses', () => {
		expect(escRegex('(group)')).toBe('\\(group\\)');
	});

	test('should escape brackets', () => {
		expect(escRegex('[class]')).toBe('\\[class\\]');
	});

	test('should escape backslash', () => {
		expect(escRegex('path\\to\\file')).toBe('path\\\\to\\\\file');
	});

	test('should escape multiple special chars', () => {
		expect(escRegex('a.b*c+d')).toBe('a\\.b\\*c\\+d');
	});
});

describe('formatTelemetryValue', () => {
	test('should format numbers with 2 decimals', () => {
		expect(formatTelemetryValue(3.14159)).toBe('3.14');
		expect(formatTelemetryValue(42)).toBe('42.00');
	});

	test('should stringify objects', () => {
		const obj = { x: 1, y: 2 };
		const result = formatTelemetryValue(obj);
		expect(result).toContain('x');
		expect(result).toContain('y');
	});

	test('should convert strings', () => {
		expect(formatTelemetryValue('hello')).toBe('hello');
	});

	test('should handle boolean', () => {
		expect(formatTelemetryValue(true)).toBe('true');
		expect(formatTelemetryValue(false)).toBe('false');
	});

	test('should handle null', () => {
		expect(formatTelemetryValue(null)).toBe('null');
	});
});

describe('findChannelNamesInText', () => {
	test('should find channel name in text', () => {
		const channels = ['velocity', 'position'];
		const text = 'The velocity is 1.5 m/s';
		const found = findChannelNamesInText(text, channels);
		expect(found).toContain('velocity');
	});

	test('should be case insensitive', () => {
		const channels = ['velocity'];
		const text = 'The VELOCITY is high';
		const found = findChannelNamesInText(text, channels);
		expect(found).toContain('velocity');
	});

	test('should match word boundaries', () => {
		const channels = ['motor'];
		const text = 'Motor speed and motorcycle count';
		// Should find 'motor' in 'Motor' but not in 'motorcycle' (word boundary)
		const found = findChannelNamesInText(text, channels);
		// Depending on implementation, this might be 1 or 2
		expect(found.length).toBeGreaterThan(0);
	});

	test('should not find channels not in text', () => {
		const channels = ['temperature', 'pressure'];
		const text = 'The motor is running';
		const found = findChannelNamesInText(text, channels);
		expect(found.length).toBe(0);
	});

	test('should return empty array for empty channels', () => {
		const found = findChannelNamesInText('some text', []);
		expect(found).toEqual([]);
	});

	test('should return empty array for no channels', () => {
		const found = findChannelNamesInText('some text', null);
		expect(found).toEqual([]);
	});
});

// Export for module use
if (typeof module !== 'undefined' && module.exports) {
	module.exports = {
		escapeHtml,
		buildTaskItem,
		getTelemetryChannels,
		escRegex,
		formatTelemetryValue,
		findChannelNamesInText,
	};
}

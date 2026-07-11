/**
 * Parley dashboard frontend — app.js
 *
 * Connects to the backend WebSocket server, maintains local state
 * (node registry, anomaly feed, activity log), and updates the DOM
 * in response to incoming messages.
 *
 * No build toolchain required — vanilla JS, no dependencies.
 */

'use strict';

// ---------------------------------------------------------------------------
// Config — matches server.py defaults
// ---------------------------------------------------------------------------
const WS_URL = `ws://${location.hostname}:8765`;
const MAX_ACTIVITY_ITEMS = 200;
const MAX_LOG_ENTRIES    = 500;
const MAX_TELEMETRY_CHANNELS = 20;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
const state = {
	nodes: {},
	anomalies: [],
	selectedNodeId: null,
	logsByNode: {},
	telemetryByNode: {}, // node_id -> { channel -> { value, ts, history: [] } }
	activityCount: 0,
	activeConvId: 'conv-system',
};

const TELEMETRY_HISTORY_LEN = 60;

// ---------------------------------------------------------------------------
// WebSocket
// ---------------------------------------------------------------------------
let ws = null;
let wsReconnectTimer = null;

function connect() {
	const wsStatus = document.getElementById('ws-status');
	wsStatus.textContent = 'Connecting...';
	wsStatus.className = 'ws-status';

	ws = new WebSocket(WS_URL);

	ws.onopen = () => {
		wsStatus.textContent = 'Connected';
		wsStatus.className = 'ws-status connected';
		clearTimeout(wsReconnectTimer);
		ws.send(JSON.stringify({ type: 'get_nodes' }));
		// Load conversation history
		loadConversationHistory();
	};

	ws.onclose = () => {
		wsStatus.textContent = 'Disconnected';
		wsStatus.className = 'ws-status disconnected';
		updateHealthIndicators(false);
		wsReconnectTimer = setTimeout(connect, 3000);
	};

	ws.onerror = () => {
		ws.close();
	};

	ws.onmessage = (event) => {
		let msg;
		try { msg = JSON.parse(event.data); }
		catch { return; }
		handleMessage(msg);
	};
}

function send(msg) {
	if (ws && ws.readyState === WebSocket.OPEN) {
		ws.send(JSON.stringify(msg));
	}
}

// ---------------------------------------------------------------------------
// Message handling
// ---------------------------------------------------------------------------
function handleMessage(msg) {
	switch (msg.type) {

		case 'snapshot':
			state.nodes = {};
			(msg.nodes || []).forEach(n => {
				const key = n.node_id || n.mac;
				state.nodes[key] = n;
			});
			state.anomalies = msg.anomalies || [];
			renderNodeList();
			renderAnomalies();
			updateHealthIndicators(true);
			break;

		case 'node_update': {
			const n = msg.node;
			const key = n.node_id || n.mac;
			state.nodes[key] = n;
			renderNodeList();
			updateHealthIndicators(true);
			if (state.selectedNodeId === key) renderNodeDetail(n);
			pushActivity('status', `${key}`, formatNodeStatusLine(n));
			break;
		}

		case 'anomaly': {
			const a = msg.anomaly;
			state.anomalies.unshift(a);
			if (state.anomalies.length > 50) state.anomalies.pop();
			renderAnomalies();
			pushActivity('anomaly', a.type || 'anomaly', a.node_id || '');
			break;
		}

		case 'log': {
			const nodeId = msg.node_id;
			if (!state.logsByNode[nodeId]) state.logsByNode[nodeId] = [];
			const logs = state.logsByNode[nodeId];
			logs.push({ ts: msg.ts, ...(msg.entry || {}) });
			if (logs.length > MAX_LOG_ENTRIES) logs.shift();
			if (state.selectedNodeId === nodeId) appendLogEntry(msg.entry, msg.ts);
			pushActivity('log', nodeId, (msg.entry && msg.entry.msg) || '');
			break;
		}

		case 'telemetry': {
			const nodeId = msg.node_id;
			if (!state.telemetryByNode[nodeId]) state.telemetryByNode[nodeId] = {};
			const ch = state.telemetryByNode[nodeId][msg.channel] || { value: null, ts: 0, history: [] };
			ch.value = msg.data;
			ch.ts    = msg.ts;
			if (typeof msg.data === 'number') {
				ch.history.push(msg.data);
				if (ch.history.length > TELEMETRY_HISTORY_LEN) ch.history.shift();
			}
			state.telemetryByNode[nodeId][msg.channel] = ch;
			if (state.selectedNodeId === nodeId) updateTelemetryView(nodeId);
			pushActivity('telemetry', `${nodeId}/${msg.channel}`, JSON.stringify(msg.data));
			break;
		}

		case 'command_sent':
			pushActivity('command', `-> ${msg.node_id}/${msg.channel}`, JSON.stringify(msg.payload));
			appendCommandHistory(msg);
			break;

		// ------------------------------------------------------------------
		// Claude AI streaming
		// ------------------------------------------------------------------
		case 'ai_chunk':
			appendAiChunk(msg.conv_id, msg.text);
			break;

		case 'ai_message_done':
			finaliseAiMessage(msg.conv_id, msg.code_blocks || [], msg.tasks || [], msg.commands || [], msg.reasoning || []);
			break;

		case 'ai_error':
			appendAiError(msg.conv_id, msg.error);
			break;

		// ------------------------------------------------------------------
		// Compile feedback
		// ------------------------------------------------------------------
		case 'compile_start':
			appendChatMessage(msg.conv_id, 'ai',
				`Building env:${msg.env} ...\n\n<div class="compile-log" id="clog-${msg.conv_id}"></div>`);
			break;

		case 'compile_output':
			appendCompileLog(msg.conv_id, msg.line);
			break;

		case 'compile_done':
			onCompileDone(msg);
			break;

		case 'ota_sent':
			appendChatMessage(msg.conv_id, 'ai',
				msg.success
					? `OTA triggered for ${msg.node_id}. The node will download, flash, and reboot.`
					: `OTA publish failed for ${msg.node_id}. Check MQTT connection.`);
			pushActivity('command', `OTA -> ${msg.node_id}`, msg.bin_url);
			break;

		case 'pong':
			break;

		case 'layout':
			// Cache the layout for map rendering
			cachedLayout = msg.layout || {};
			// Render spatial map if a node is currently selected
			if (state.selectedNodeId) {
				renderSpatialMap(state.selectedNodeId);
			}
			break;

		// ------------------------------------------------------------------
		// Conversation history
		// ------------------------------------------------------------------
		case 'history':
			conversationHistory = msg.conversations || [];
			renderConversationHistory(conversationHistory);
			break;

		case 'search_results':
			if (msg.conversations) {
				conversationHistory = msg.conversations;
				renderConversationHistory(conversationHistory);
			}
			break;

		case 'conversation':
			if (msg.data) {
				displayPastConversation(msg.data);
			}
			break;

		case 'node_files':
			if (msg.files) {
				displayNodeFiles(msg.node_id, msg.files, msg.selected_file);
			}
			break;

		// ------------------------------------------------------------------
		// Recovery / rollback diagnostics
		// ------------------------------------------------------------------
		case 'recovery_diagnosis': {
			const d = msg.diagnosis || {};
			const nodeId = msg.node_id;
			// Surface in anomaly feed
			state.anomalies.unshift({ ts: msg.ts, type: 'rollback', ...d });
			if (state.anomalies.length > 50) state.anomalies.pop();
			renderAnomalies();
			pushActivity('anomaly', `ROLLBACK ${nodeId}`,
				`rolled back from ${d.rolled_back_from || '?'} — last log: [${d.last_log_level || '?'}] ${d.last_log_msg || ''}`);
			// Append a summary message to any open conversation for this node
			Object.values(state.conversations || {}).forEach(conv => {
				if (conv.nodeId === nodeId) {
					appendChatMessage(conv.id, 'ai',
						`**Rollback detected on ${nodeId}**\n` +
						`- Failed firmware: \`${d.rolled_back_from || '?'}\`\n` +
						`- Running: \`${d.current_version || '?'}\`\n` +
						`- Log entries in previous boot: ${d.log_entries ?? '?'} (${d.log_size_bytes ?? '?'} bytes)\n` +
						`- Last log: [${d.last_log_level || '?'}] \`${d.last_log_tag || ''}\` ${d.last_log_msg || ''}`);
				}
			});
			break;
		}

		case 'recovery_log_complete': {
			const nodeId = msg.node_id;
			// Store in logsByNode under a special key
			if (!state.logsByNode[nodeId]) state.logsByNode[nodeId] = [];
			// Prepend a header marker
			const header = `=== previous boot log for ${nodeId} (${new Date(msg.ts * 1000).toLocaleTimeString()}) ===`;
			const lines = (msg.log || '').split('\n').filter(l => l.trim());
			const entries = lines.map(line => {
				try { return JSON.parse(line); } catch { return { msg: line }; }
			});
			entries.forEach(e => state.logsByNode[nodeId].push({ ts: msg.ts, ...e }));
			if (state.logsByNode[nodeId].length > MAX_LOG_ENTRIES) {
				state.logsByNode[nodeId].splice(0, state.logsByNode[nodeId].length - MAX_LOG_ENTRIES);
			}
			if (state.selectedNodeId === nodeId) {
				appendLogEntry({ msg: header }, msg.ts);
				entries.forEach(e => appendLogEntry(e, msg.ts));
			}
			pushActivity('log', nodeId, `received ${lines.length} lines from previous boot`);
			break;
		}
	}
}

// ---------------------------------------------------------------------------
// AI chat helpers
// ---------------------------------------------------------------------------

// Per-conversation: { msgEl, full } while streaming
const _streamingMsg = {};

function appendAiChunk(convId, text) {
	if (!_streamingMsg[convId]) {
		// Start a new bubble
		const container = document.getElementById(`chat-${convId}`);
		if (!container) return;
		const empty = container.querySelector('.empty-state');
		if (empty) empty.remove();

		const el = document.createElement('div');
		el.className = 'chat-msg ai streaming';
		el.innerHTML = '<div class="chat-msg-bubble"><span class="stream-cursor">|</span></div><div class="chat-msg-meta">Claude</div>';
		container.appendChild(el);
		container.scrollTop = container.scrollHeight;
		_streamingMsg[convId] = { el, full: '' };
	}
	_streamingMsg[convId].full += text;
	const bubble = _streamingMsg[convId].el.querySelector('.chat-msg-bubble');
	bubble.innerHTML = renderMarkdownLite(_streamingMsg[convId].full) + '<span class="stream-cursor">|</span>';
	const container = document.getElementById(`chat-${convId}`);
	if (container) container.scrollTop = container.scrollHeight;
}

function finaliseAiMessage(convId, codeBlocks, tasks, commands, reasoning) {
	const sm = _streamingMsg[convId];
	if (sm) {
		const bubble = sm.el.querySelector('.chat-msg-bubble');
		bubble.innerHTML = renderMarkdownLite(sm.full);
		sm.el.classList.remove('streaming');
		delete _streamingMsg[convId];
	}

	// Display reasoning blocks if any
	if (reasoning && reasoning.length > 0) {
		const container = document.getElementById(`chat-${convId}`);
		if (container) {
			reasoning.forEach(reason => {
				displayReasoningBlock(container, reason);
			});
		}
	}

	// Display inline commands if any
	if (commands && commands.length > 0) {
		const container = document.getElementById(`chat-${convId}`);
		if (container) {
			commands.forEach(cmd => {
				displayInlineCommand(container, cmd, convId);
			});
		}
	}

	// Store code blocks on the conversation for triggerCompile()
	if (conversations[convId]) conversations[convId]._lastCodeBlocks = codeBlocks;

	// Add extracted tasks to the conversation
	if (tasks && tasks.length > 0) {
		const conv = conversations[convId];
		if (conv) {
			tasks.forEach(t => conv.tasks.push(t));
			renderRightPanel();
		}
	}

	if (codeBlocks.length > 0) {
		const conv = conversations[convId];
		const isHighTrust = conv && conv.trustMode === 'high-trust';

		// In high-trust mode, skip the diff review and go straight to "ready to compile"
		// In co-coding mode, show diffs for review
		if (!isHighTrust) {
			showCodeDiff(convId, null, codeBlocks.map(b => ({
				filename: b.filename || b.lang,
				reason: 'AI generated',
				confidence: 'medium',
				lines: b.code.split('\n').map(l => ({ type: 'add', text: l })),
				rawCode: b.code,
				lang: b.lang,
			})), 'medium');
		}

		const cppBlocks = codeBlocks.filter(b => b.lang === 'cpp' || b.lang === 'c');
		if (cppBlocks.length > 0) {
			const envName = conv && conv.nodeId
				? conv.nodeId.replace(/[^a-zA-Z0-9_]/g, '_')
				: 'node_app';

			if (isHighTrust) {
				// High-trust: show simple "accepted and ready" message with compile button
				appendChatMessage(convId, 'ai',
					`<div class="compile-actions">` +
					`<span style="color:var(--text-mute);font-size:12px">✓ Code auto-accepted (high-trust mode)</span> ` +
					`<button class="send-btn" style="padding:4px 12px;font-size:12px;background:#1a2f1e;border-color:var(--green);color:var(--green)" ` +
					`onclick="triggerCompile('${convId}','${escHtml(envName)}')">Compile for env:${escHtml(envName)}</button>` +
					`</div>`);
			} else {
				// Co-coding: show "ready for review" message (awaiting accept in diff panel)
				appendChatMessage(convId, 'ai',
					`<div class="compile-actions">` +
					`<span style="color:var(--text-mute);font-size:12px">Firmware is ready for review.</span> ` +
					`<button class="send-btn" style="padding:4px 12px;font-size:12px" ` +
					`onclick="triggerCompile('${convId}','${escHtml(envName)}')">Compile for env:${escHtml(envName)}</button>` +
					`</div>`);
			}
		}
	}
}

function appendAiError(convId, errorText) {
	const container = document.getElementById(`chat-${convId}`);
	if (!container) return;
	delete _streamingMsg[convId];
	const el = document.createElement('div');
	el.className = 'chat-msg ai';
	el.innerHTML = `<div class="chat-msg-bubble" style="color:var(--red)">[Error] ${escHtml(errorText)}</div>`;
	container.appendChild(el);
	container.scrollTop = container.scrollHeight;
}

function appendCompileLog(convId, line) {
	const logEl = document.getElementById(`clog-${convId}`);
	if (!logEl) return;
	const span = document.createElement('div');
	span.className = 'compile-log-line';
	const isErr = /error|failed/i.test(line);
	span.style.color = isErr ? 'var(--red)' : 'var(--text-mute)';
	span.textContent = line;
	logEl.appendChild(span);
	logEl.scrollTop = logEl.scrollHeight;
}

function onCompileDone(msg) {
	const logEl = document.getElementById(`clog-${msg.conv_id}`);
	if (logEl) {
		const summary = document.createElement('div');
		summary.style.cssText = `font-weight:600;margin-top:8px;color:${msg.success ? 'var(--green)' : 'var(--red)'}`;
		summary.textContent = msg.success ? '✓ Build succeeded!' : '✗ Build FAILED.';
		logEl.appendChild(summary);
	}

	if (msg.success && msg.bin_url) {
		const convEl = document.getElementById(`chat-${msg.conv_id}`);
		if (convEl) {
			const el = document.createElement('div');
			el.className = 'chat-msg ai';
			const conv = conversations[msg.conv_id];
			const nodeId = (conv && conv.nodeId) ? escHtml(conv.nodeId) : '';
			
			const binFileName = msg.bin_url.split('/').pop();
			
			el.innerHTML = `
				<div class="chat-msg-bubble">
					<div style="margin-bottom:8px">
						<strong>✓ Build succeeded</strong><br>
						<span style="color:var(--text-mute);font-size:11px">Firmware: ${escHtml(binFileName)}</span>
					</div>
					${nodeId ? `<div style="
						background: rgba(63, 185, 80, 0.1);
						border: 1px solid var(--green);
						border-radius: 6px;
						padding: 10px;
						display: flex;
						align-items: center;
						gap: 10px;
					">
						<span style="flex:1; font-size:12px; color:var(--green)">Ready to deploy?</span>
						<button class="send-btn" style="
							padding: 6px 14px;
							font-size: 12px;
							background: var(--green);
							color: #000;
							border: none;
							border-radius: 4px;
							font-weight: 600;
							cursor: pointer;
						" onclick="triggerOta('${msg.conv_id}','${nodeId}','${escHtml(msg.bin_url)}')">
							Deploy to ${nodeId}
						</button>
					</div>` : ''}
				</div>
				<div class="chat-msg-meta">Compiler</div>
			`;
			convEl.appendChild(el);
			convEl.scrollTop = convEl.scrollHeight;
		}
	}

	if (!msg.success) {
		const convEl = document.getElementById(`chat-${msg.conv_id}`);
		if (convEl) {
			const el = document.createElement('div');
			el.className = 'chat-msg ai';
			const errData = JSON.stringify(msg.error_lines || []);
			el.innerHTML = `
				<div class="chat-msg-bubble">
					<span style="color:var(--red)">Build failed.</span>
					<div class="compile-actions" style="margin-top:8px">
						<button class="send-btn" style="padding:4px 12px;font-size:12px;background:var(--orange)"
							onclick="triggerAiFix('${escHtml(msg.conv_id)}', '${escHtml(msg.env)}', ${errData.replace(/'/g, "&#39;")})">
							Fix with AI
						</button>
					</div>
				</div>
				<div class="chat-msg-meta">Compiler</div>
			`;
			convEl.appendChild(el);
			convEl.scrollTop = convEl.scrollHeight;
		}
	}
}

// Simple markdown renderer (bold, inline code, newlines only — no external lib)
function renderMarkdownLite(text) {
	return escHtml(text)
		.replace(/```[\w]*\n?([\s\S]*?)```/g, '<pre class="raw-json" style="margin:8px 0">$1</pre>')
		.replace(/`([^`]+)`/g, '<code style="background:var(--surface2);padding:1px 4px;border-radius:3px">$1</code>')
		.replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>')
		.replace(/\n/g, '<br>');
}

// Display an inline command from AI response
function displayInlineCommand(container, cmd, convId) {
	const el = document.createElement('div');
	el.className = 'chat-msg ai';
	el.id = `cmd-${cmd.id}`;
	el.innerHTML = `
		<div class="chat-msg-bubble" style="background: rgba(63, 137, 255, 0.08); border-left: 3px solid #3f89ff; padding: 10px">
			<div style="font-weight: 600; color: #3f89ff; margin-bottom: 6px">↓ Command</div>
			<div style="font-family: monospace; font-size: 12px; color: var(--text); margin-bottom: 8px">
				<span style="color: var(--text-mute)">→</span> <strong>nodes/${escHtml(cmd.node_id)}/cmd/${escHtml(cmd.channel)}</strong> ${escHtml(JSON.stringify(cmd.payload))}
			</div>
			<div style="display: flex; gap: 8px">
				<button class="send-btn" style="padding: 4px 10px; font-size: 11px; background: var(--green); color: #000; border: none; border-radius: 4px; cursor: pointer; font-weight: 600" 
					onclick="executeAiCommand('${convId}', '${escHtml(cmd.id)}', '${escHtml(cmd.node_id)}', '${escHtml(cmd.channel)}', ${JSON.stringify(cmd.payload).replace(/'/g, "&#39;")})">
					Execute
				</button>
				<button class="send-btn" style="padding: 4px 10px; font-size: 11px; background: var(--surface2); color: var(--text); border: 1px solid var(--border); border-radius: 4px; cursor: pointer" 
					onclick="cancelAiCommand('${convId}', '${escHtml(cmd.id)}')">
					Skip
				</button>
			</div>
			<div class="cmd-status" id="status-${cmd.id}" style="margin-top: 8px; font-size: 11px; color: var(--text-mute)"></div>
		</div>
		<div class="chat-msg-meta">AI</div>
	`;
	container.appendChild(el);
	container.scrollTop = container.scrollHeight;
}

// Execute a command that was issued by AI
function executeAiCommand(convId, cmdId, nodeId, channel, payload) {
	const statusEl = document.getElementById(`status-${cmdId}`);
	if (statusEl) statusEl.innerHTML = '⟳ Executing...';
	
	// Send the command
	send({ type: 'send_command', node_id: nodeId, channel: channel, payload: payload });
	
	// Mark as executed (simple state)
	setTimeout(() => {
		if (statusEl) statusEl.innerHTML = '✓ Sent (waiting for response)';
	}, 300);
}

// Cancel an inline command
function cancelAiCommand(convId, cmdId) {
	const el = document.getElementById(`cmd-${cmdId}`);
	if (el) {
		el.style.opacity = '0.6';
		const statusEl = document.getElementById(`status-${cmdId}`);
		if (statusEl) statusEl.innerHTML = '(skipped)';
	}
}

// Display reasoning block from AI response
function displayReasoningBlock(container, reason) {
	const el = document.createElement('div');
	el.className = 'chat-msg ai';
	el.id = `reason-${reason.id}`;
	
	const confidenceColor = {
		'high': '#3fbf50',
		'medium': '#f5a623',
		'low': '#e74c3c'
	}[reason.confidence] || '#888';
	
	let reasoningHtml = `
		<div class="chat-msg-bubble" style="background: rgba(100, 100, 255, 0.08); border-left: 3px solid #6464ff; padding: 12px">
			<div style="font-weight: 600; color: #6464ff; margin-bottom: 8px; display: flex; align-items: center; gap: 6px">
				<span>⚙ Reasoning Chain</span>
				<span style="font-size: 10px; background: ${confidenceColor}; color: white; padding: 2px 6px; border-radius: 3px; font-weight: 600; text-transform: uppercase">${reason.confidence}</span>
			</div>
	`;
	
	if (reason.observed) {
		reasoningHtml += `<div style="margin-bottom: 6px; font-size: 12px"><strong>📊 Observed:</strong> ${escHtml(reason.observed)}</div>`;
	}
	if (reason.context) {
		reasoningHtml += `<div style="margin-bottom: 6px; font-size: 12px"><strong>📋 Context:</strong> ${escHtml(reason.context)}</div>`;
	}
	if (reason.hypothesis) {
		reasoningHtml += `<div style="margin-bottom: 6px; font-size: 12px"><strong>💡 Hypothesis:</strong> ${escHtml(reason.hypothesis)}</div>`;
	}
	if (reason.next_step) {
		reasoningHtml += `<div style="margin-top: 8px; padding-top: 8px; border-top: 1px solid rgba(100,100,255,0.2); font-size: 12px"><strong>→ Next Step:</strong> ${escHtml(reason.next_step)}</div>`;
	}
	
	reasoningHtml += `</div>`;
	
	el.innerHTML = `
		${reasoningHtml}
		<div class="chat-msg-meta">Reasoning</div>
	`;
	
	container.appendChild(el);
	container.scrollTop = container.scrollHeight;
}

// Global helpers called from inline onclick
function triggerCompile(convId, envName) {
	const conv = conversations[convId];
	if (!conv) return;
	// Find the latest cpp code block from conversation messages
	const lastBlock = (conv._lastCodeBlocks || []).find(b => b.lang === 'cpp' || b.lang === 'c');
	if (!lastBlock) { alert('No C++ code block found in conversation.'); return; }
	send({ type: 'compile', conv_id: convId, env_name: envName, source_code: lastBlock.rawCode || lastBlock.code });
}

function triggerAiFix(convId, envName, errorLines) {
	const conv = conversations[convId];
	if (!conv) return;
	const errText = Array.isArray(errorLines) ? errorLines.join('\n') : String(errorLines);
	const prompt = `The PlatformIO build for env:${envName} failed with the following errors:\n\`\`\`\n${errText}\n\`\`\`\nPlease fix the code and return the complete corrected main.cpp.`;
	// Append as a user message visible in the chat
	appendChatMessage(convId, 'user', prompt);
	// Send to Claude via the normal chat_message path
	send({ type: 'chat_message', conv_id: convId, text: prompt });
}

function triggerOta(convId, nodeId, binUrl) {
	if (!confirm(`Deploy firmware to ${nodeId}? The node will download, reboot, and run the new version.`)) return;
	
	send({ type: 'push_ota', node_id: nodeId, bin_url: binUrl });
	
	// Show deployment in progress message
	const convEl = document.getElementById(`chat-${convId}`);
	if (convEl) {
		const el = document.createElement('div');
		el.className = 'chat-msg ai';
		el.innerHTML = `
			<div class="chat-msg-bubble">
				<div style="display:flex; align-items:center; gap:8px">
					<span style="color:var(--yellow);font-size:12px">⟳ Deploying to ${escHtml(nodeId)}...</span>
					<span style="font-size:10px;color:var(--text-mute)">Pushing firmware URL to node</span>
				</div>
			</div>
			<div class="chat-msg-meta">Deploy</div>
		`;
		convEl.appendChild(el);
		convEl.scrollTop = convEl.scrollHeight;
	}
	
	pushActivity('deploy', nodeId, 'OTA started');
}

// ---------------------------------------------------------------------------
// Node list rendering
// ---------------------------------------------------------------------------
function renderNodeList() {
	const container = document.getElementById('node-list');
	const countEl   = document.getElementById('node-count');
	const tpl       = document.getElementById('tpl-node-card');

	const nodes = Object.values(state.nodes);
	countEl.textContent = `${nodes.length} node${nodes.length === 1 ? '' : 's'}`;

	const existingCards = {};
	container.querySelectorAll('.node-card').forEach(el => {
		existingCards[el.dataset.nodeKey] = el;
	});

	const seen = new Set();
	nodes.forEach(node => {
		const key  = node.node_id || node.mac;
		const conn = node.connectivity || 'offline';
		seen.add(key);

		let card = existingCards[key];
		if (!card) {
			card = tpl.content.cloneNode(true).querySelector('.node-card');
			card.dataset.nodeKey = key;
			card.addEventListener('click', () => selectNode(key));
			container.appendChild(card);
		}

		card.className = `node-card ${conn}`;
		if (state.selectedNodeId === key) card.classList.add('selected');

		card.querySelector('.node-card-id').textContent = node.node_id || node.mac || 'unknown';
		card.querySelector('.node-card-meta').textContent = formatNodeMeta(node);

		const badge = card.querySelector('.node-card-badge');
		badge.textContent = conn === 'factory'
			? (node.status === 'needs_provisioning' ? 'New' : 'Factory')
			: conn === 'offline' ? 'Offline' : '';
	});

	Object.keys(existingCards).forEach(key => {
		if (!seen.has(key)) existingCards[key].remove();
	});
}

function formatNodeMeta(node) {
	const parts = [];
	if (node.fw) parts.push(node.fw);
	if (node.ip) parts.push(node.ip);
	if (node.rssi) parts.push(`${node.rssi} dBm`);
	return parts.join(' | ');
}

function formatNodeStatusLine(node) {
	const parts = [];
	if (node.status)    parts.push(node.status);
	if (node.uptime_s)  parts.push(`up ${formatUptime(node.uptime_s)}`);
	if (node.free_heap) parts.push(`${Math.round(node.free_heap / 1024)}KB heap`);
	return parts.join(', ');
}

function formatUptime(s) {
	if (s < 60)    return `${s}s`;
	if (s < 3600)  return `${Math.floor(s / 60)}m`;
	return `${Math.floor(s / 3600)}h ${Math.floor((s % 3600) / 60)}m`;
}

// ---------------------------------------------------------------------------
// Node selection
// ---------------------------------------------------------------------------
function selectNode(key) {
	state.selectedNodeId = key;

	// Update card selection highlight
	document.querySelectorAll('.node-card').forEach(el => {
		el.classList.toggle('selected', el.dataset.nodeKey === key);
	});

	const node = state.nodes[key];
	if (!node) return;

	// Switch to System tab if we're not already on it
	switchConvTab('conv-system');

	document.getElementById('center-title').textContent = node.node_id || node.mac || key;
	document.getElementById('no-selection').classList.add('hidden');
	document.getElementById('node-detail').classList.remove('hidden');
	document.getElementById('open-conv-btn').classList.remove('hidden');

	// Auto-add node to current conversation's context
	addNodeContext(node.node_id || node.mac || key);

	renderNodeDetail(node);
	
	// Switch to Code tab by default
	document.querySelectorAll('.tab-btn').forEach(b => {
		b.classList.toggle('active', b.dataset.tab === 'tab-code');
	});
	document.querySelectorAll('.tab-content').forEach(p => {
		p.classList.toggle('hidden', p.id !== 'tab-code');
	});
	
	// Request source files for this node
	requestNodeFiles(node.node_id || node.mac || key);
}

document.getElementById('open-conv-btn').addEventListener('click', () => {
	if (!state.selectedNodeId) return;
	const node = state.nodes[state.selectedNodeId];
	if (!node) return;
	const nodeId = node.node_id || node.mac;
	createConversation(`Debug: ${nodeId}`, 'debug', nodeId);
});

function renderNodeDetail(node) {
	renderDetailGrid(node);
	document.getElementById('node-raw-json').textContent =
		JSON.stringify(node, null, 2);
	populateLogView(node.node_id || node.mac);
	updateTelemetryView(node.node_id || node.mac);
	renderSpatialMap(node.node_id || node.mac);
}

// ---------------------------------------------------------------------------
// Detail grid
// ---------------------------------------------------------------------------
function renderDetailGrid(node) {
	const grid = document.getElementById('detail-grid');
	grid.innerHTML = '';

	const lastSeen = node.last_seen
		? new Date(node.last_seen * 1000).toLocaleTimeString()
		: '—';

	const cells = [
		{ label: 'Status',     value: node.status       || '—',    colorClass: statusColor(node) },
		{ label: 'FW',         value: node.fw            || '—'                },
		{ label: 'Uptime',     value: node.uptime_s != null ? formatUptime(node.uptime_s) : '—' },
		{ label: 'Boot count', value: node.boot_count    != null ? node.boot_count : '—', colorClass: bootCountColor(node.boot_count) },
		{ label: 'Reset',      value: node.reset_reason  || '—'                },
		{ label: 'RSSI',       value: node.rssi != null ? `${node.rssi} dBm` : '—', colorClass: rssiColor(node.rssi) },
		{ label: 'Free heap',  value: node.free_heap != null ? `${Math.round(node.free_heap / 1024)} KB` : '—' },
		{ label: 'Last seen',  value: lastSeen           },
		{ label: 'IP',         value: node.ip            || '—'                },
		{ label: 'MAC',        value: node.mac           || '—'                },
		{ label: 'Validated',  value: node.validated != null ? (node.validated ? 'Yes' : 'No') : '—', colorClass: node.validated ? 'ok' : '' },
	];

	cells.forEach(({ label, value, colorClass }) => {
		const cell = document.createElement('div');
		cell.className = 'detail-cell';
		cell.innerHTML = `
			<div class="detail-cell-label">${label}</div>
			<div class="detail-cell-value ${colorClass || ''}">${value}</div>
		`;
		grid.appendChild(cell);
	});
}

function statusColor(node) {
	const c = node.connectivity;
	if (c === 'online')  return 'ok';
	if (c === 'factory') return 'warn';
	if (c === 'offline') return 'error';
	return '';
}

function bootCountColor(count) {
	if (count == null) return '';
	if (count >= 3) return 'error';
	if (count >= 1) return 'warn';
	return 'ok';
}

function rssiColor(rssi) {
	if (rssi == null) return '';
	if (rssi >= -65) return 'ok';
	if (rssi >= -80) return 'warn';
	return 'error';
}

// ---------------------------------------------------------------------------
// Tabs
// ---------------------------------------------------------------------------
document.querySelectorAll('.tab-btn').forEach(btn => {
	btn.addEventListener('click', () => {
		const target = btn.dataset.tab;
		document.querySelectorAll('.tab-btn').forEach(b => b.classList.toggle('active', b === btn));
		document.querySelectorAll('.tab-content').forEach(el => {
			el.classList.toggle('hidden', el.id !== target);
		});
	});
});

// ---------------------------------------------------------------------------
// Log view
// ---------------------------------------------------------------------------
function populateLogView(nodeId) {
	const container = document.getElementById('log-stream');
	container.innerHTML = '';

	const logs = state.logsByNode[nodeId] || [];
	if (logs.length === 0) {
		container.innerHTML = '<div class="empty-state">No log entries yet</div>';
		return;
	}
	logs.forEach(entry => {
		container.appendChild(buildLogEntry(entry, entry.ts));
	});
	container.scrollTop = container.scrollHeight;
}

function appendLogEntry(entry, ts) {
	const container = document.getElementById('log-stream');
	const emptyState = container.querySelector('.empty-state');
	if (emptyState) emptyState.remove();

	const el = buildLogEntry(entry, ts);
	container.appendChild(el);

	// Cap displayed entries
	while (container.children.length > MAX_LOG_ENTRIES) {
		container.firstChild.remove();
	}
	container.scrollTop = container.scrollHeight;
}

function buildLogEntry(entry, ts) {
	const el = document.createElement('div');
	el.className = 'log-entry';
	const level = (entry && entry.level) || 'INFO';
	const msg   = (entry && entry.msg)   || JSON.stringify(entry);
	const time  = ts ? new Date(ts * 1000).toLocaleTimeString() : '';
	el.innerHTML = `
		<span class="log-time">${time}</span>
		<span class="log-level ${level}">${level}</span>
		<span class="log-msg">${escHtml(msg)}</span>
	`;
	return el;
}

// ---------------------------------------------------------------------------
// Telemetry view with sparklines
// ---------------------------------------------------------------------------
function updateTelemetryView(nodeId) {
	const container = document.getElementById('telemetry-list');
	const channels  = state.telemetryByNode[nodeId] || {};
	const keys      = Object.keys(channels);

	if (keys.length === 0) {
		container.innerHTML = '<div class="empty-state">No telemetry yet</div>';
		return;
	}

	container.innerHTML = '';
	keys.slice(0, MAX_TELEMETRY_CHANNELS).forEach(channel => {
		const { value, ts, history } = channels[channel];
		const el = document.createElement('div');
		el.className = 'telemetry-entry';
		const time = ts ? new Date(ts * 1000).toLocaleTimeString() : '';
		el.innerHTML = `
			<div class="telemetry-channel">${escHtml(channel)} <span style="color:var(--text-mute);font-size:10px">${time}</span></div>
			<div class="telemetry-value">${escHtml(JSON.stringify(value, null, 2))}</div>
		`;
		// Draw sparkline if we have numeric history
		if (history && history.length > 1) {
			const wrap   = document.createElement('div');
			wrap.className = 'sparkline-wrap';
			const canvas = document.createElement('canvas');
			canvas.className = 'sparkline';
			canvas.width  = 180;
			canvas.height = 36;
			drawSparkline(canvas, history);
			wrap.appendChild(canvas);
			el.appendChild(wrap);
		}
		container.appendChild(el);
	});
}

function drawSparkline(canvas, data) {
	const ctx = canvas.getContext('2d');
	const W   = canvas.width;
	const H   = canvas.height;
	const pad = 3;
	const min = Math.min(...data);
	const max = Math.max(...data);
	const range = max - min || 1;

	ctx.clearRect(0, 0, W, H);

	// Fill under line
	ctx.beginPath();
	data.forEach((v, i) => {
		const x = pad + (i / (data.length - 1)) * (W - pad * 2);
		const y = H - pad - ((v - min) / range) * (H - pad * 2);
		if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
	});
	ctx.strokeStyle = '#2f81f7';
	ctx.lineWidth   = 1.5;
	ctx.stroke();

	// Dot at last point
	const lx = pad + ((data.length - 1) / (data.length - 1)) * (W - pad * 2);
	const lv = data[data.length - 1];
	const ly = H - pad - ((lv - min) / range) * (H - pad * 2);
	ctx.beginPath();
	ctx.arc(lx, ly, 3, 0, Math.PI * 2);
	ctx.fillStyle = '#2f81f7';
	ctx.fill();
}

// ---------------------------------------------------------------------------
// Commands — auth levels: free / confirmed / locked
// ---------------------------------------------------------------------------

// Pending locked command (stored while modal is open)
let _pendingLockedCmd = null;

function dispatchCommand(nodeId, channel, payload, auth) {
	const authLevel = (auth || 'free').toLowerCase();

	if (authLevel === 'free') {
		_sendCommand(nodeId, channel, payload);

	} else if (authLevel === 'confirmed') {
		if (!confirm(`Send "${channel}" to ${nodeId}?`)) return;
		_sendCommand(nodeId, channel, payload);

	} else if (authLevel === 'locked') {
		_pendingLockedCmd = { nodeId, channel, payload };
		document.getElementById('locked-cmd-preview').textContent =
			`nodes/${nodeId}/cmd/${channel}  ${JSON.stringify(payload)}`;
		document.getElementById('locked-phrase').value = '';
		document.getElementById('locked-cmd-modal').classList.remove('hidden');
	}
}

function _sendCommand(nodeId, channel, payload) {
	send({ type: 'send_command', node_id: nodeId, channel, payload });
}

document.getElementById('cmd-send').addEventListener('click', () => {
	if (!state.selectedNodeId) return;
	const node = state.nodes[state.selectedNodeId];
	if (!node) return;

	const channel = document.getElementById('cmd-channel').value.trim();
	let payload = {};
	const rawPayload = document.getElementById('cmd-payload').value.trim();
	if (rawPayload) {
		try { payload = JSON.parse(rawPayload); }
		catch { alert('Payload is not valid JSON'); return; }
	}
	if (!channel) { alert('Channel is required'); return; }

	const auth = document.getElementById('cmd-auth').value;
	dispatchCommand(node.node_id || node.mac, channel, payload, auth);
});

// Preset buttons inside command panel
document.getElementById('command-presets').addEventListener('click', (e) => {
	const btn = e.target.closest('.cmd-preset-btn');
	if (!btn || !state.selectedNodeId) return;
	const node = state.nodes[state.selectedNodeId];
	if (!node) return;

	const channel = btn.dataset.channel;
	const payload = JSON.parse(btn.dataset.payload || '{}');
	const auth    = btn.dataset.auth || 'free';
	dispatchCommand(node.node_id || node.mac, channel, payload, auth);
});

// Locked command modal
document.getElementById('locked-confirm-btn').addEventListener('click', () => {
	const phrase = document.getElementById('locked-phrase').value.trim();
	if (phrase !== 'confirm') {
		alert('You must type exactly "confirm" to proceed.');
		return;
	}
	if (_pendingLockedCmd) {
		_sendCommand(_pendingLockedCmd.nodeId, _pendingLockedCmd.channel, _pendingLockedCmd.payload);
		_pendingLockedCmd = null;
	}
	document.getElementById('locked-cmd-modal').classList.add('hidden');
});

document.getElementById('locked-modal-close').addEventListener('click', () => {
	_pendingLockedCmd = null;
	document.getElementById('locked-cmd-modal').classList.add('hidden');
});

function appendCommandHistory(msg) {
	const container = document.getElementById('command-history');
	if (!container) return;
	const el = document.createElement('div');
	el.className = 'cmd-history-entry';
	const time = new Date(msg.ts * 1000).toLocaleTimeString();
	el.innerHTML = `<span class="cmd-dir">-></span>${escHtml(msg.node_id)}/${escHtml(msg.channel)} ${escHtml(JSON.stringify(msg.payload))} <span style="color:var(--text-mute)">${time}</span>`;
	const label = container.querySelector('.section-label');
	if (label) label.after(el);
	else container.appendChild(el);
}

// ---------------------------------------------------------------------------
// Anomaly feed
// ---------------------------------------------------------------------------
function renderAnomalies() {
	const container = document.getElementById('anomaly-feed');
	container.innerHTML = '';

	if (state.anomalies.length === 0) {
		container.innerHTML = '<div class="empty-state">No anomalies</div>';
		return;
	}

	state.anomalies.slice(0, 20).forEach((a, idx) => {
		const el = document.createElement('div');
		el.className = 'anomaly-item';

		const body = document.createElement('div');
		body.className = 'anomaly-item-body';
		body.innerHTML = `
			<div class="anomaly-type">${escHtml(a.type || 'anomaly')}</div>
			<div class="anomaly-body">${escHtml(a.node_id || '')} ${formatAnomalyDetail(a)}</div>
		`;
		// Click → open a conversation pre-loaded with anomaly context
		body.addEventListener('click', () => {
			const title  = `Anomaly: ${a.type || 'unknown'} on ${a.node_id || 'system'}`;
			const nodeId = a.node_id || '';
			const convId = createConversation(title, 'investigate', nodeId);
			// Add the raw anomaly as context seed
			addContextItem(`Anomaly: ${a.type}`, 'anomaly', convId);
			// Open a first message describing the anomaly
			appendChatMessage(convId, 'ai',
				`Anomaly detected on ${a.node_id || 'system'}: ${a.type || 'unknown'}. ${formatAnomalyDetail(a)}`);
		});

		const dismissBtn = document.createElement('button');
		dismissBtn.className = 'anomaly-dismiss';
		dismissBtn.title = 'Dismiss';
		dismissBtn.textContent = 'x';
		dismissBtn.addEventListener('click', (e) => {
			e.stopPropagation();
			state.anomalies.splice(idx, 1);
			renderAnomalies();
		});

		el.appendChild(body);
		el.appendChild(dismissBtn);
		container.appendChild(el);
	});
}

function formatAnomalyDetail(a) {
	const parts = [];
	if (a.silent_s)    parts.push(`silent ${Math.round(a.silent_s)}s`);
	if (a.boot_count)  parts.push(`boot_count=${a.boot_count}`);
	if (a.ip)          parts.push(a.ip);
	return parts.join(' ');
}

// ---------------------------------------------------------------------------
// Conversation tab management
// ---------------------------------------------------------------------------
const conversations = {};  // id -> { id, title, kind, nodeId, messages, tasks, context, trustMode, deployOnAccept }
let convCounter = 0;
let _closingConvId = null; // conv being closed (for summary modal)

function openNewConvModal() {
	const sel = document.getElementById('conv-node');
	sel.innerHTML = '<option value="">-- none --</option>';
	Object.values(state.nodes).forEach(n => {
		const key = n.node_id || n.mac;
		const opt = document.createElement('option');
		opt.value = key;
		opt.textContent = key;
		sel.appendChild(opt);
	});
	document.getElementById('conv-title').value = '';
	document.getElementById('conv-kind').value  = '';
	document.getElementById('new-conv-modal').classList.remove('hidden');
	document.getElementById('conv-title').focus();
}

function createConversation(title, kind, nodeId) {
	convCounter++;
	const id = `conv-${convCounter}`;
	conversations[id] = {
		id, title, kind, nodeId,
		messages: [],
		tasks: [],
		context: nodeId ? [{ name: nodeId, type: 'node', key: nodeId }] : [],
		trustMode: 'co-coding',
		deployOnAccept: false,
	};

	// Build tab
	const tabBar = document.getElementById('conv-tabbar');
	const tab = document.createElement('button');
	tab.className = 'conv-tab';
	tab.dataset.conv = id;
	tab.innerHTML = `${escHtml(title)} <span class="conv-tab-close" data-conv-close="${id}">&times;</span>`;
	tabBar.insertBefore(tab, document.getElementById('new-conv-btn'));

	// Build panel
	const panel = document.createElement('div');
	panel.className = 'conv-panel';
	panel.id = id;
	panel.innerHTML = `
		<div class="conv-info-bar">
			${kind ? `<span class="conv-kind-badge">${escHtml(kind)}</span>` : ''}
			${nodeId ? `<span>Node: <strong>${escHtml(nodeId)}</strong></span>` : ''}
			<span style="margin-left:auto;display:flex;align-items:center;gap:10px">
				<span class="toggle-row">
					<label class="toggle-switch">
						<input type="checkbox" class="trust-toggle" />
						<span class="toggle-slider"></span>
					</label>
					<span class="trust-label" title="High-trust: accept without review. Co-coding: review every change.">Co-coding</span>
				</span>
				<span class="toggle-row">
					<label class="toggle-switch">
						<input type="checkbox" class="deploy-toggle" />
						<span class="toggle-slider"></span>
					</label>
					<span title="Deploy on accept: push OTA immediately when a code change is accepted.">Deploy on accept</span>
				</span>
			</span>
		</div>
		<div class="chat-area">
			<div class="chat-messages" id="chat-${id}">
				<div class="empty-state">Conversation started. Send a message to begin.</div>
			</div>
			<div class="chat-input-row">
				<textarea class="chat-input" placeholder="Ask anything..." rows="1"></textarea>
				<button class="chat-send-btn">Send</button>
			</div>
		</div>
	`;

	// Trust mode toggle
	const trustToggle = panel.querySelector('.trust-toggle');
	const trustLabel  = panel.querySelector('.trust-label');
	trustToggle.addEventListener('change', () => {
		conversations[id].trustMode = trustToggle.checked ? 'high-trust' : 'co-coding';
		trustLabel.textContent = trustToggle.checked ? 'High-trust' : 'Co-coding';
	});

	// Deploy-on-accept toggle
	const deployToggle = panel.querySelector('.deploy-toggle');
	deployToggle.addEventListener('change', () => {
		conversations[id].deployOnAccept = deployToggle.checked;
	});

	// Chat send
	const sendBtn = panel.querySelector('.chat-send-btn');
	const inputEl = panel.querySelector('.chat-input');
	const sendMsg = () => {
		const text = inputEl.value.trim();
		if (!text) return;
		appendChatMessage(id, 'human', text);
		inputEl.value = '';

		// Send to backend → Claude
		const conv = conversations[id];
		send({
			type:    'chat_message',
			conv_id: id,
			message: text,
			node_id: (conv && conv.nodeId) || '',
		});
	};
	sendBtn.addEventListener('click', sendMsg);
	inputEl.addEventListener('keydown', e => {
		if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); sendMsg(); }
	});

	document.getElementById('panel-center').appendChild(panel);
	switchConvTab(id);
	return id;
}

function appendChatMessage(convId, role, text) {
	const container = document.getElementById(`chat-${convId}`);
	if (!container) return;
	const emptyState = container.querySelector('.empty-state');
	if (emptyState) emptyState.remove();

	const msg = document.createElement('div');
	msg.className = `chat-msg ${role}`;
	const bubble = document.createElement('div');
	bubble.className = 'chat-msg-bubble';
	
	// Process text to add telemetry references (for AI messages)
	if (role === 'ai') {
		bubble.innerHTML = makeTextWithTelemetryRefs(text, convId);
	} else {
		bubble.innerHTML = escHtml(text);
	}
	
	msg.appendChild(bubble);
	const meta = document.createElement('div');
	meta.className = 'chat-msg-meta';
	meta.textContent = new Date().toLocaleTimeString();
	msg.appendChild(meta);
	
	container.appendChild(msg);
	container.scrollTop = container.scrollHeight;

	// Attach event listeners to telemetry refs
	setTimeout(() => {
		bubble.querySelectorAll('.telemetry-ref').forEach(ref => {
			ref.addEventListener('click', (e) => {
				e.stopPropagation();
				showTelemetryTooltip(ref);
			});
		});
	}, 0);

	// Store in state
	if (conversations[convId]) {
		conversations[convId].messages.push({ role, text, ts: Date.now() });
	}
}

function makeTextWithTelemetryRefs(text, convId) {
	const conv = conversations[convId];
	if (!conv || !conv.nodeId) return escHtml(text);
	
	const nodeId = conv.nodeId;
	const telemetry = state.telemetryByNode[nodeId] || {};
	const channels = Object.keys(telemetry);
	
	let html = escHtml(text);
	
	// Replace channel names with clickable links
	channels.forEach(channel => {
		const regex = new RegExp(`\\b${escRegex(channel)}\\b`, 'gi');
		html = html.replace(regex, `<span class="telemetry-ref" data-channel="${escHtml(channel)}" data-node="${escHtml(nodeId)}">${channel}</span>`);
	});
	
	return html;
}

function escRegex(str) {
	return str.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

function showTelemetryTooltip(refElement) {
	// Remove any existing tooltips
	document.querySelectorAll('.telemetry-tooltip').forEach(t => t.remove());
	
	const channel = refElement.dataset.channel;
	const nodeId = refElement.dataset.node;
	const telemetry = state.telemetryByNode[nodeId];
	
	if (!telemetry || !telemetry[channel]) return;
	
	const data = telemetry[channel];
	const tooltip = document.createElement('div');
	tooltip.className = 'telemetry-tooltip';
	
	const value = typeof data.value === 'object' 
		? JSON.stringify(data.value) 
		: String(data.value);
	const time = data.ts ? new Date(data.ts * 1000).toLocaleTimeString() : 'unknown';
	
	tooltip.innerHTML = `${channel}: ${escHtml(value)} <span style="color:var(--text-mute)">${time}</span>`;
	tooltip.style.position = 'fixed';
	
	// Position tooltip near the reference
	const rect = refElement.getBoundingClientRect();
	tooltip.style.top = (rect.bottom + 5) + 'px';
	tooltip.style.left = (rect.left) + 'px';
	
	document.body.appendChild(tooltip);
	
	// Auto-hide after 4 seconds
	setTimeout(() => tooltip.remove(), 4000);
	
	// Hide on click elsewhere
	const dismissHandler = (e) => {
		if (e.target !== refElement) {
			tooltip.remove();
			document.removeEventListener('click', dismissHandler);
		}
	};
	document.addEventListener('click', dismissHandler);
}

// Cached layout
let cachedLayout = null;

function renderSpatialMap(nodeId) {
	const svg = document.getElementById('spatial-map');
	if (!svg) return;
	
	// Fetch layout if not already cached
	if (!cachedLayout) {
		send({ type: 'get_layout' });
		// Will be received and cached when the response comes back
		// For now, show placeholder
		svg.innerHTML = '<text x="50%" y="50%" text-anchor="middle" dominant-baseline="middle" style="fill:var(--text-mute);font-size:12px">Loading layout...</text>';
		return;
	}
	
	const layout = cachedLayout;
	svg.innerHTML = ''; // Clear
	
	// Add background
	const bg = document.createElementNS('http://www.w3.org/2000/svg', 'rect');
	bg.setAttribute('width', '100%');
	bg.setAttribute('height', '100%');
	bg.setAttribute('fill', 'var(--surface)');
	svg.appendChild(bg);
	
	// Add grid
	for (let x = 0; x <= 100; x += 10) {
		const line = document.createElementNS('http://www.w3.org/2000/svg', 'line');
		line.setAttribute('x1', x + '%');
		line.setAttribute('y1', '0%');
		line.setAttribute('x2', x + '%');
		line.setAttribute('y2', '100%');
		line.setAttribute('stroke', 'var(--border)');
		line.setAttribute('stroke-width', '0.5');
		svg.appendChild(line);
	}
	for (let y = 0; y <= 100; y += 10) {
		const line = document.createElementNS('http://www.w3.org/2000/svg', 'line');
		line.setAttribute('x1', '0%');
		line.setAttribute('y1', y + '%');
		line.setAttribute('x2', '100%');
		line.setAttribute('y2', y + '%');
		line.setAttribute('stroke', 'var(--border)');
		line.setAttribute('stroke-width', '0.5');
		svg.appendChild(line);
	}
	
	// Draw coordinator (center)
	const cx = 50, cy = 50;
	const coordCircle = document.createElementNS('http://www.w3.org/2000/svg', 'circle');
	coordCircle.setAttribute('cx', cx + '%');
	coordCircle.setAttribute('cy', cy + '%');
	coordCircle.setAttribute('r', '15');
	coordCircle.setAttribute('fill', '#2f81f7');
	svg.appendChild(coordCircle);
	
	const coordLabel = document.createElementNS('http://www.w3.org/2000/svg', 'text');
	coordLabel.setAttribute('x', cx + '%');
	coordLabel.setAttribute('y', (cy + 25) + '%');
	coordLabel.setAttribute('text-anchor', 'middle');
	coordLabel.setAttribute('class', 'map-node-label');
	coordLabel.textContent = 'Coordinator';
	svg.appendChild(coordLabel);
	
	// Draw peripherals from layout
	const peripherals = layout.peripherals || {};
	const peripheralIds = Object.keys(peripherals);
	
	if (peripheralIds.length === 0) {
		const nodata = document.createElementNS('http://www.w3.org/2000/svg', 'text');
		nodata.setAttribute('x', '50%');
		nodata.setAttribute('y', '50%');
		nodata.setAttribute('text-anchor', 'middle');
		nodata.setAttribute('dominant-baseline', 'middle');
		nodata.setAttribute('style', 'fill:var(--text-mute);font-size:12px');
		nodata.textContent = 'No peripherals in layout';
		svg.appendChild(nodata);
		return;
	}
	
	// Distribute nodes around the coordinator in a circle
	const radius = 35; // % of svg
	peripheralIds.forEach((id, idx) => {
		const angle = (idx / peripheralIds.length) * 2 * Math.PI;
		const px = cx + radius * Math.cos(angle);
		const py = cy + radius * Math.sin(angle);
		
		// Draw node
		const node = state.nodes[id];
		const isOnline = node && node.connectivity === 'online';
		const color = isOnline ? '#22c55e' : '#ef4444';
		
		const g = document.createElementNS('http://www.w3.org/2000/svg', 'g');
		g.setAttribute('class', 'map-node');
		g.setAttribute('data-node', id);
		
		const circle = document.createElementNS('http://www.w3.org/2000/svg', 'circle');
		circle.setAttribute('cx', px + '%');
		circle.setAttribute('cy', py + '%');
		circle.setAttribute('r', '12');
		circle.setAttribute('fill', color);
		circle.setAttribute('opacity', '0.8');
		g.appendChild(circle);
		
		const label = document.createElementNS('http://www.w3.org/2000/svg', 'text');
		label.setAttribute('x', px + '%');
		label.setAttribute('y', (py + 20) + '%');
		label.setAttribute('class', 'map-node-label');
		label.textContent = id;
		g.appendChild(label);
		
		g.addEventListener('click', () => selectNode(id));
		svg.appendChild(g);
	});
}

function switchConvTab(id) {
	state.activeConvId = id;
	document.querySelectorAll('.conv-tab').forEach(t =>
		t.classList.toggle('active', t.dataset.conv === id)
	);
	document.querySelectorAll('.conv-panel').forEach(p =>
		p.classList.toggle('active', p.id === id)
	);
	// Refresh right panel to show the active conversation's tasks and context
	renderRightPanel();
}

function renderRightPanel() {
	const id   = state.activeConvId;
	const conv = conversations[id]; // undefined for 'conv-system'

	// Tasks
	const taskList = document.getElementById('task-list');
	taskList.innerHTML = '';
	const tasks = conv ? conv.tasks : [];
	if (tasks.length === 0) {
		taskList.innerHTML = '<div class="empty-state">No tasks yet</div>';
	} else {
		tasks.forEach(t => taskList.appendChild(buildTaskItem(t, id)));
	}

	// Context
	const ctxList = document.getElementById('context-list');
	ctxList.innerHTML = '';
	const ctx = conv ? conv.context : [];
	if (ctx.length === 0) {
		ctxList.innerHTML = '<div class="empty-state">No context loaded</div>';
	} else {
		ctx.forEach(c => ctxList.appendChild(buildContextItem(c, id)));
	}
}

function closeConvTab(id) {
	_closingConvId = id;
	document.getElementById('summary-text').value = '';
	document.getElementById('summary-modal').classList.remove('hidden');
}

function _doCloseConv(id) {
	const tab   = document.querySelector(`.conv-tab[data-conv="${id}"]`);
	const panel = document.getElementById(id);
	if (tab)   tab.remove();
	if (panel) panel.remove();
	delete conversations[id];
	_closingConvId = null;
	switchConvTab('conv-system');
}

document.getElementById('summary-save-btn').addEventListener('click', () => {
	const notes = document.getElementById('summary-text').value.trim();
	if (notes && _closingConvId) {
		const conv = conversations[_closingConvId];
		const title = conv ? conv.title : _closingConvId;
		// In a real deployment the backend would persist this
		console.info(`[summary] ${title}:\n${notes}`);
	}
	const id = _closingConvId;
	document.getElementById('summary-modal').classList.add('hidden');
	if (id) _doCloseConv(id);
});

document.getElementById('summary-discard-btn').addEventListener('click', () => {
	const id = _closingConvId;
	document.getElementById('summary-modal').classList.add('hidden');
	if (id) _doCloseConv(id);
});

document.getElementById('new-conv-btn').addEventListener('click', openNewConvModal);

document.getElementById('modal-close').addEventListener('click', () => {
	document.getElementById('new-conv-modal').classList.add('hidden');
});

document.getElementById('conv-create-btn').addEventListener('click', () => {
	const title  = document.getElementById('conv-title').value.trim() || 'Untitled';
	const kind   = document.getElementById('conv-kind').value;
	const nodeId = document.getElementById('conv-node').value;
	document.getElementById('new-conv-modal').classList.add('hidden');
	createConversation(title, kind, nodeId);
});

// Close modal on Enter in title field
document.getElementById('conv-title').addEventListener('keydown', e => {
	if (e.key === 'Enter') document.getElementById('conv-create-btn').click();
});

// Tab click and close-x click (delegated from tabbar)
document.getElementById('conv-tabbar').addEventListener('click', e => {
	const closeBtn = e.target.closest('[data-conv-close]');
	if (closeBtn) { e.stopPropagation(); closeConvTab(closeBtn.dataset.convClose); return; }
	const tab = e.target.closest('.conv-tab');
	if (tab) switchConvTab(tab.dataset.conv);
});

// ---------------------------------------------------------------------------
// Task management — per-conversation, stored in conversations[id].tasks
// ---------------------------------------------------------------------------
function addTask(title, convId) {
	convId = convId || state.activeConvId;
	const conv = conversations[convId];
	if (!conv) return;

	const taskObj = { title, status: 'pending', id: Date.now() };
	conv.tasks.push(taskObj);

	// Only update the DOM if this conversation is currently shown
	if (state.activeConvId === convId) {
		const list = document.getElementById('task-list');
		const emptyState = list.querySelector('.empty-state');
		if (emptyState) emptyState.remove();
		list.appendChild(buildTaskItem(taskObj, convId));
	}
}

function buildTaskItem(taskObj, convId) {
	const item = document.createElement('div');
	item.className = `task-item${taskObj.status !== 'pending' ? ' ' + taskObj.status : ''}`;
	item.dataset.taskId = taskObj.id;

	const createdAt = taskObj.created ? new Date(taskObj.created * 1000).toLocaleTimeString() : '';

	if (taskObj.status === 'pending') {
		item.innerHTML = `
			<div class="task-item-title">${escHtml(taskObj.title)}</div>
			${createdAt ? `<div class="task-item-meta" style="font-size:11px;color:var(--text-mute)">${escHtml(createdAt)}</div>` : ''}
			<div class="task-actions">
				<button class="task-btn accept">✓ Accept</button>
				<button class="task-btn focus-task">→ Focus</button>
				<button class="task-btn defer">⋯ Defer</button>
				<button class="task-btn reject">✕ Reject</button>
			</div>
		`;

		item.querySelector('.accept').addEventListener('click', () => {
			taskObj.status = 'accepted';
			renderRightPanel();
		});

		item.querySelector('.focus-task').addEventListener('click', () => {
			// Switch to that conversation tab and append a focus note
			if (convId) switchConvTab(convId);
			appendChatMessage(convId, 'ai', `Focusing on task: "${taskObj.title}".`);
		});

		item.querySelector('.defer').addEventListener('click', () => {
			taskObj.status = 'deferred';
			renderRightPanel();
		});

		item.querySelector('.reject').addEventListener('click', () => {
			const conv = conversations[convId];
			if (conv) conv.tasks = conv.tasks.filter(t => t.id !== taskObj.id);
			renderRightPanel();
		});
	} else {
		const statusColor = taskObj.status === 'accepted' ? 'var(--green)' : 'var(--yellow)';
		item.innerHTML = `
			<div class="task-item-title">${escHtml(taskObj.title)}</div>
			<div style="font-size:11px;color:${statusColor};margin-top:2px">${taskObj.status}</div>
			${createdAt ? `<div class="task-item-meta" style="font-size:10px;color:var(--text-mute)">${escHtml(createdAt)}</div>` : ''}
		`;
	}
	return item;
}

// ---------------------------------------------------------------------------
// Context items — per-conversation, stored in conversations[id].context
// ---------------------------------------------------------------------------
function addContextItem(name, type, convId) {
	convId = convId || state.activeConvId;
	const conv = conversations[convId];
	if (!conv) return;
	if (conv.context.find(c => c.name === name)) return;  // deduplicate
	conv.context.push({ name, type });
	if (state.activeConvId === convId) {
		const list = document.getElementById('context-list');
		const emptyState = list.querySelector('.empty-state');
		if (emptyState) emptyState.remove();
		list.appendChild(buildContextItem({ name, type }, convId));
	}
}

function buildContextItem(c, convId) {
	const icon = c.type === 'node' ? '⬤' : c.type === 'file' ? '📄' : c.type === 'anomaly' ? '⚠' : '📌';
	const typeLabel = c.type === 'node' ? 'Node' : c.type === 'file' ? 'File' : c.type === 'anomaly' ? 'Anomaly' : 'Data';
	const item = document.createElement('div');
	item.className = 'context-item';
	item.innerHTML = `
		<span class="context-item-icon" title="${typeLabel}">${icon}</span>
		<span class="context-item-name" title="${escHtml(c.name)}">${escHtml(c.name)}</span>
		<button class="context-item-remove" title="Remove from context">&times;</button>
	`;
	item.querySelector('.context-item-remove').addEventListener('click', () => {
		const conv = conversations[convId];
		if (conv) conv.context = conv.context.filter(x => x.name !== c.name);
		renderRightPanel();
	});
	return item;
}

function addNodeContext(nodeId) {
	addContextItem(nodeId, 'node', state.activeConvId);
}

// Right panel tab switching
document.querySelectorAll('.rp-tab').forEach(btn => {
	btn.addEventListener('click', () => {
		const target = btn.dataset.rp;
		document.querySelectorAll('.rp-tab').forEach(b => b.classList.toggle('active', b === btn));
		document.querySelectorAll('.rp-pane').forEach(p => p.classList.toggle('hidden', p.id !== target));
	});
});

// ---------------------------------------------------------------------------
// Code diff display (right panel Code tab)
// Spec: diffs appear in right panel Code tab with Accept/Reject/Modify per hunk
// ---------------------------------------------------------------------------
function showCodeDiff(convId, filename, hunks, confidence) {
	const area = document.getElementById('code-content-area');
	if (!area) return;
	
	area.innerHTML = '';

	// Title
	const title = document.createElement('div');
	title.style.cssText = `
		padding: 12px 12px;
		font-weight: 600;
		font-size: 13px;
		border-bottom: 1px solid var(--border);
		background: var(--surface);
	`;
	title.innerHTML = `${hunks.length} file${hunks.length !== 1 ? 's' : ''} changed`;
	area.appendChild(title);

	// Confidence banner
	if (confidence) {
		const conf = document.createElement('div');
		conf.className = `diff-confidence ${confidence}`;
		const labels = { high: 'Confidence: high', medium: 'Confidence: medium', low: 'Confidence: low' };
		conf.textContent = labels[confidence] || confidence;
		area.appendChild(conf);
	}

	let acceptedCount = 0;

	hunks.forEach((hunk, idx) => {
		const el = document.createElement('div');
		el.className = 'diff-hunk';
		el.id = `diff-hunk-${idx}`;

		// File header with proper unified diff format
		const fileHeader = document.createElement('div');
		fileHeader.className = 'diff-file-header';
		fileHeader.style.cssText = `
			padding: 10px 12px;
			background: var(--surface);
			border-bottom: 1px solid var(--border);
			font-family: var(--font-mono);
			font-size: 11px;
			color: var(--text-mute);
			cursor: pointer;
			user-select: none;
		`;
		
		const fileName = hunk.filename || 'code';
		fileHeader.innerHTML = `
			<div style="display:flex; align-items:center; gap:8px">
				<span>▼</span>
				<span style="color:var(--accent);font-weight:600">${escHtml(fileName)}</span>
				<span style="margin-left:auto">@@ new file @@</span>
			</div>
		`;

		// Action buttons
		const actionBar = document.createElement('div');
		actionBar.style.cssText = `
			display: flex;
			gap: 6px;
			padding: 8px 12px;
			background: var(--surface2);
			border-bottom: 1px solid var(--border);
			flex-shrink: 0;
		`;

		const acceptBtn = document.createElement('button');
		acceptBtn.className = 'diff-action-btn accept';
		acceptBtn.textContent = '✓ Accept';
		acceptBtn.style.cssText = `
			background: rgba(63, 185, 80, 0.1);
			border: 1px solid var(--green);
			color: var(--green);
			padding: 4px 10px;
			border-radius: 4px;
			font-size: 11px;
			cursor: pointer;
			flex: 1;
		`;

		const rejectBtn = document.createElement('button');
		rejectBtn.className = 'diff-action-btn reject';
		rejectBtn.textContent = '✗ Reject';
		rejectBtn.style.cssText = `
			background: rgba(248, 81, 73, 0.1);
			border: 1px solid var(--red);
			color: var(--red);
			padding: 4px 10px;
			border-radius: 4px;
			font-size: 11px;
			cursor: pointer;
			flex: 1;
		`;

		actionBar.appendChild(acceptBtn);
		actionBar.appendChild(rejectBtn);

		// Code diff with line numbers
		const lines = document.createElement('div');
		lines.className = 'diff-lines';
		lines.style.cssText = `
			font-family: var(--font-mono);
			font-size: 11px;
			background: var(--bg);
			line-height: 1.6;
			max-height: 400px;
			overflow-y: auto;
			border-bottom: 1px solid var(--border);
		`;
		
		// Show original file is empty, then all lines are additions
		if (hunk.rawCode) {
			const codeLines = hunk.rawCode.split('\n');
			codeLines.forEach((line, lineNum) => {
				const lineEl = document.createElement('div');
				lineEl.style.cssText = `
					display: flex;
					background: rgba(63, 185, 80, 0.08);
					border-left: 3px solid var(--green);
				`;

				const lineNumEl = document.createElement('span');
				lineNumEl.style.cssText = `
					width: 40px;
					text-align: right;
					padding-right: 10px;
					color: var(--text-mute);
					flex-shrink: 0;
					user-select: none;
					padding-left: 4px;
				`;
				lineNumEl.textContent = String(lineNum + 1);

				const codeEl = document.createElement('span');
				codeEl.style.cssText = `
					flex: 1;
					color: var(--green);
					white-space: pre-wrap;
					word-break: break-all;
					padding-right: 10px;
				`;
				codeEl.textContent = '+ ' + line;

				lineEl.appendChild(lineNumEl);
				lineEl.appendChild(codeEl);
				lines.appendChild(lineEl);
			});
		} else {
			// Fallback to line-based rendering
			(hunk.lines || []).forEach((line, lineNum) => {
				const lineEl = document.createElement('div');
				
				if (line.type === 'add') {
					lineEl.style.cssText = `
						display: flex;
						background: rgba(63, 185, 80, 0.08);
						border-left: 3px solid var(--green);
					`;

					const lineNumSpan = document.createElement('span');
					lineNumSpan.style.cssText = `
						width: 40px;
						text-align: right;
						padding-right: 10px;
						color: var(--text-mute);
						flex-shrink: 0;
						user-select: none;
						padding-left: 4px;
					`;
					lineNumSpan.textContent = String(lineNum + 1);

					const codeSpan = document.createElement('span');
					codeSpan.style.cssText = `
						flex: 1;
						color: var(--green);
						white-space: pre-wrap;
						word-break: break-all;
						padding-right: 10px;
					`;
					codeSpan.textContent = '+ ' + (line.text || '');

					lineEl.appendChild(lineNumSpan);
					lineEl.appendChild(codeSpan);
				} else if (line.type === 'remove') {
					lineEl.style.cssText = `
						display: flex;
						background: rgba(248, 81, 73, 0.08);
						border-left: 3px solid var(--red);
						opacity: 0.7;
					`;

					const lineNumSpan = document.createElement('span');
					lineNumSpan.style.cssText = `
						width: 40px;
						text-align: right;
						padding-right: 10px;
						color: var(--text-mute);
						flex-shrink: 0;
						user-select: none;
						padding-left: 4px;
					`;
					lineNumSpan.textContent = String(lineNum + 1);

					const codeSpan = document.createElement('span');
					codeSpan.style.cssText = `
						flex: 1;
						color: var(--red);
						text-decoration: line-through;
						white-space: pre-wrap;
						word-break: break-all;
						padding-right: 10px;
					`;
					codeSpan.textContent = '- ' + (line.text || '');

					lineEl.appendChild(lineNumSpan);
					lineEl.appendChild(codeSpan);
				} else {
					lineEl.style.cssText = `
						display: flex;
						color: var(--text-mute);
					`;

					const lineNumSpan = document.createElement('span');
					lineNumSpan.style.cssText = `
						width: 40px;
						text-align: right;
						padding-right: 10px;
						color: var(--text-mute);
						flex-shrink: 0;
						user-select: none;
						padding-left: 4px;
					`;
					lineNumSpan.textContent = String(lineNum + 1);

					const codeSpan = document.createElement('span');
					codeSpan.style.cssText = `
						flex: 1;
						white-space: pre-wrap;
						word-break: break-all;
						padding-right: 10px;
					`;
					codeSpan.textContent = '  ' + (line.text || '');

					lineEl.appendChild(lineNumSpan);
					lineEl.appendChild(codeSpan);
				}

				lines.appendChild(lineEl);
			});
		}

		acceptBtn.addEventListener('click', () => {
			el.style.opacity = '0.5';
			el.style.pointerEvents = 'none';
			actionBar.innerHTML = '<span style="color:var(--green);font-size:11px">✓ Accepted</span>';
			acceptedCount++;
			
			// Check if all are accepted, then show compile button
			if (acceptedCount === hunks.length) {
				showCompilePrompt(convId, hunk.lang);
			}
		});

		rejectBtn.addEventListener('click', () => {
			el.style.opacity = '0.3';
			el.style.pointerEvents = 'none';
			actionBar.innerHTML = '<span style="color:var(--red);font-size:11px">✗ Rejected</span>';
		});

		// Make header clickable to collapse/expand
		fileHeader.addEventListener('click', () => {
			const isHidden = lines.style.display === 'none';
			lines.style.display = isHidden ? 'block' : 'none';
			actionBar.style.display = isHidden ? 'flex' : 'none';
			const arrow = fileHeader.querySelector('span');
			if (arrow) arrow.textContent = isHidden ? '▼' : '▶';
		});

		el.appendChild(fileHeader);
		el.appendChild(actionBar);
		el.appendChild(lines);
		area.appendChild(el);
	});
}

function showCompilePrompt(convId, lang) {
	const area = document.getElementById('diff-area');
	const prompt = document.createElement('div');
	prompt.style.cssText = `
		padding: 12px;
		background: rgba(63, 185, 80, 0.1);
		border: 1px solid var(--green);
		border-radius: 6px;
		margin-top: 12px;
		display: flex;
		align-items: center;
		gap: 12px;
	`;

	const text = document.createElement('span');
	text.style.cssText = `flex: 1; font-size: 12px; color: var(--green)`;
	text.textContent = '✓ All changes accepted. Ready to compile?';
	prompt.appendChild(text);

	const compileBtn = document.createElement('button');
	compileBtn.style.cssText = `
		background: var(--green);
		color: #000;
		border: none;
		padding: 6px 14px;
		border-radius: 4px;
		font-size: 12px;
		font-weight: 600;
		cursor: pointer;
	`;
	compileBtn.textContent = 'Compile';
	
	compileBtn.addEventListener('click', () => {
		const conv = conversations[convId];
		const envName = conv && conv.nodeId
			? conv.nodeId.replace(/[^a-zA-Z0-9_]/g, '_')
			: 'node_app';
		triggerCompile(convId, envName);
		prompt.remove();
	});
	
	prompt.appendChild(compileBtn);
	area.appendChild(prompt);
}

// ---------------------------------------------------------------------------
// Activity feed (right panel)
// ---------------------------------------------------------------------------
function pushActivity(tag, label, text) {
	const feed = document.getElementById('activity-feed');
	const tpl  = document.getElementById('tpl-activity-item');
	const el   = tpl.content.cloneNode(true).querySelector('.activity-item');

	el.querySelector('.activity-time').textContent =
		new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });

	const tagEl = el.querySelector('.activity-tag');
	tagEl.textContent  = tag;
	tagEl.className    = `activity-tag ${tag}`;

	el.querySelector('.activity-text').textContent = `${label}${text ? ' — ' + text.slice(0, 80) : ''}`;

	feed.insertBefore(el, feed.firstChild);
	state.activityCount++;

	// Cap list length
	while (feed.children.length > MAX_ACTIVITY_ITEMS) {
		feed.lastChild.remove();
	}
}

document.getElementById('clear-activity').addEventListener('click', () => {
	document.getElementById('activity-feed').innerHTML = '';
});

// ---------------------------------------------------------------------------
// Health indicators
// ---------------------------------------------------------------------------
function updateHealthIndicators(wsOk) {
	const broker  = document.getElementById('ind-broker');
	const gateway = document.getElementById('ind-gateway');
	const nodes   = document.getElementById('ind-nodes');

	broker.className = `indicator ${wsOk ? 'ok' : 'error'}`;

	// Gateway: check if a node with node_id === 'gateway' is online
	const gw = Object.values(state.nodes).find(n => n.node_id === 'gateway');
	if (gw && gw.connectivity === 'online') {
		gateway.className = 'indicator ok';
	} else if (gw) {
		gateway.className = 'indicator warn';
	} else {
		gateway.className = 'indicator error';
	}

	// Nodes: all online?
	const peripheral = Object.values(state.nodes).filter(n => n.node_id !== 'gateway');
	const allOnline  = peripheral.length > 0 && peripheral.every(n => n.connectivity === 'online');
	const anyOffline = peripheral.some(n => n.connectivity === 'offline');
	if (allOnline)        nodes.className = 'indicator ok';
	else if (anyOffline)  nodes.className = 'indicator warn';
	else                  nodes.className = 'indicator';
}

// ---------------------------------------------------------------------------
// Emergency stop
// ---------------------------------------------------------------------------
document.getElementById('estop-btn').addEventListener('click', () => {
	if (!confirm('Send EMERGENCY STOP to all actuator nodes?')) return;
	send({
		type:    'publish',
		topic:   'system/estop',
		payload: { stop: true, ts: Date.now() / 1000 },
	});
	pushActivity('command', 'ESTOP', 'Emergency stop published to system/estop');
});

// Deploy button handler
const deployBtn = document.getElementById('code-deploy-btn');
if (deployBtn) {
	deployBtn.addEventListener('click', () => {
		// Deploy the latest compiled firmware to the selected node
		if (!state.selectedNodeId) {
			alert('Select a node first');
			return;
		}
		if (!state.activeConvId) {
			alert('No active conversation');
			return;
		}
		// Trigger OTA deployment using the latest binary from the active conversation
		triggerOta(state.activeConvId, state.selectedNodeId, null);
	});
}

// Past conversation selector handler
const pastConvSelector = document.getElementById('past-conv-selector');
if (pastConvSelector) {
	pastConvSelector.addEventListener('change', (e) => {
		if (e.target.value) {
			loadPastConversation(e.target.value);
		}
	});
}

// Resizable right panel divider
const rpDivider = document.getElementById('rp-divider');
const rightFooter = document.getElementById('right-footer');
const panelRight = document.getElementById('panel-right');

if (rpDivider && rightFooter && panelRight) {
	let isResizing = false;
	let startY = 0;
	let startHeight = 0;

	rpDivider.addEventListener('mousedown', (e) => {
		isResizing = true;
		startY = e.clientY;
		startHeight = rightFooter.offsetHeight;
		document.body.style.userSelect = 'none';
		document.body.style.cursor = 'row-resize';
	});

	document.addEventListener('mousemove', (e) => {
		if (!isResizing) return;
		
		const delta = e.clientY - startY;
		const newHeight = Math.max(40, startHeight - delta); // Min 40px
		const panelHeight = panelRight.offsetHeight;
		const maxHeight = panelHeight - 100; // Leave room for tabs and divider
		const finalHeight = Math.min(newHeight, maxHeight);
		
		rightFooter.style.flex = `0 0 ${finalHeight}px`;
		rightFooter.style.maxHeight = `${finalHeight}px`;
	});

	document.addEventListener('mouseup', () => {
		if (isResizing) {
			isResizing = false;
			document.body.style.userSelect = 'auto';
			document.body.style.cursor = 'auto';
		}
	});
}

// Resizable center-right panel divider (width)
const cpDivider = document.getElementById('cp-divider');
const panelCenter = document.getElementById('panel-center');
const panels = document.querySelector('.panels');

if (cpDivider && panelCenter && panels) {
	let isResizing = false;
	let startX = 0;
	let startWidth = 0;

	cpDivider.addEventListener('mousedown', (e) => {
		isResizing = true;
		startX = e.clientX;
		startWidth = panelCenter.offsetWidth;
		document.body.style.userSelect = 'none';
		document.body.style.cursor = 'col-resize';
	});

	document.addEventListener('mousemove', (e) => {
		if (!isResizing) return;
		
		const delta = e.clientX - startX;
		const newWidth = Math.max(300, startWidth + delta); // Min 300px
		const panelsWidth = panels.offsetWidth;
		const rightPanelMinWidth = 200; // Minimum width for right panel
		const maxWidth = panelsWidth - rightPanelMinWidth - 280 - 6; // Account for left panel, divider, right panel minimum
		const finalWidth = Math.min(newWidth, maxWidth);
		
		panelCenter.style.flex = `0 0 ${finalWidth}px`;
	});

	document.addEventListener('mouseup', () => {
		if (isResizing) {
			isResizing = false;
			document.body.style.userSelect = 'auto';
			document.body.style.cursor = 'auto';
		}
	});
}

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------
function escHtml(str) {
	if (str == null) return '';
	return String(str)
		.replace(/&/g, '&amp;')
		.replace(/</g, '&lt;')
		.replace(/>/g, '&gt;')
		.replace(/"/g, '&quot;');
}

// ---------------------------------------------------------------------------
// Code editor (tab-code) — edit firmware before compiling
// ---------------------------------------------------------------------------
const codeEditState = {
	editing: false,
	originalCode: '',
	currentCode: '',
};

const codeEditBtn = document.getElementById('code-edit-btn');
if (codeEditBtn) {
	codeEditBtn.addEventListener('click', () => {
		const viewer = document.getElementById('code-viewer');
		const editor = document.getElementById('code-editor');
		const editBtn = document.getElementById('code-edit-btn');
		const saveBtn = document.getElementById('code-save-btn');
		const cancelBtn = document.getElementById('code-cancel-btn');

		if (!codeEditState.editing) {
			// Enter edit mode
			codeEditState.originalCode = viewer.textContent;
			codeEditState.currentCode = codeEditState.originalCode;
			editor.value = codeEditState.currentCode;
			
			viewer.classList.add('hidden');
			editor.classList.remove('hidden');
			editBtn.classList.add('hidden');
			saveBtn.classList.remove('hidden');
			cancelBtn.classList.remove('hidden');
			editor.focus();
			codeEditState.editing = true;
		}
	});
}

const codeSaveBtn = document.getElementById('code-save-btn');
if (codeSaveBtn) {
	codeSaveBtn.addEventListener('click', () => {
		const viewer = document.getElementById('code-viewer');
		const editor = document.getElementById('code-editor');
		const editBtn = document.getElementById('code-edit-btn');
		const saveBtn = document.getElementById('code-save-btn');
		const cancelBtn = document.getElementById('code-cancel-btn');

		// Update the displayed code
		codeEditState.currentCode = editor.value;
		viewer.textContent = codeEditState.currentCode;
		
		// Update the last code block in the conversation so compile uses edited version
		const conv = conversations[state.activeConvId];
		if (conv && conv._lastCodeBlocks && conv._lastCodeBlocks.length > 0) {
			const lastCppBlock = conv._lastCodeBlocks.find(b => b.lang === 'cpp' || b.lang === 'c');
			if (lastCppBlock) {
				lastCppBlock.code = codeEditState.currentCode;
				lastCppBlock.rawCode = codeEditState.currentCode;
			}
		}

		// Exit edit mode
		editor.classList.add('hidden');
		viewer.classList.remove('hidden');
		editBtn.classList.remove('hidden');
		saveBtn.classList.add('hidden');
		cancelBtn.classList.add('hidden');
		codeEditState.editing = false;
		
		appendChatMessage(state.activeConvId, 'human', 'I\'ve edited the code. Ready to compile when you are.');
	});
}

const codeCancelBtn = document.getElementById('code-cancel-btn');
if (codeCancelBtn) {
	codeCancelBtn.addEventListener('click', () => {
		const viewer = document.getElementById('code-viewer');
		const editor = document.getElementById('code-editor');
		const editBtn = document.getElementById('code-edit-btn');
		const saveBtn = document.getElementById('code-save-btn');
		const cancelBtn = document.getElementById('code-cancel-btn');

		// Discard changes
		codeEditState.currentCode = codeEditState.originalCode;
		editor.value = codeEditState.originalCode;

		// Exit edit mode
		editor.classList.add('hidden');
		viewer.classList.remove('hidden');
		editBtn.classList.remove('hidden');
		saveBtn.classList.add('hidden');
		cancelBtn.classList.add('hidden');
		codeEditState.editing = false;
	});
}

// ---------------------------------------------------------------------------
// Conversation History / Past Conversations
// ---------------------------------------------------------------------------
let conversationHistory = [];

function loadConversationHistory() {
	send({ type: 'get_history', limit: 50 });
}

function searchConversations(query = '', kind = '', nodeId = '') {
	send({
		type: 'search_conversations',
		query: query,
		kind: kind,
		node_id: nodeId,
		limit: 50,
	});
}

function loadPastConversation(convId) {
	send({ type: 'get_conversation', conv_id: convId });
}

function renderConversationHistory(conversations) {
	const container = document.getElementById('history-list');
	const selector = document.getElementById('past-conv-selector');
	
	if (!conversations || conversations.length === 0) {
		if (container) container.innerHTML = '<div class="empty-state">No conversations yet</div>';
		if (selector) {
			while (selector.options.length > 1) selector.remove(1);
		}
		return;
	}

	// Update the dropdown in the right panel chat area
	if (selector) {
		// Keep the "Select a conversation..." option, remove the rest
		while (selector.options.length > 1) selector.remove(1);
		
		conversations.forEach(conv => {
			const option = document.createElement('option');
			option.value = conv.id;
			option.textContent = `${conv.title} (${new Date(conv.created * 1000).toLocaleDateString()})`;
			selector.appendChild(option);
		});
	}
	
	// Also update the history list in the left panel (for History view)
	if (container) {
		container.innerHTML = '';
		conversations.forEach(conv => {
			const card = document.createElement('div');
			card.className = 'history-card';
			
			const createdDate = new Date(conv.created * 1000).toLocaleDateString();
			const nodeLabels = (conv.related_nodes || []).map(n => `<span class="node-tag">${escHtml(n)}</span>`).join('');
			const codeCount = conv.code_blocks || 0;
			const deployed = conv.deployed_to ? ` → deployed` : '';
			
			card.innerHTML = `
				<div class="history-card-title" onclick="loadPastConversation('${escHtml(conv.id)}')">${escHtml(conv.title)}</div>
				<div class="history-card-meta">
					${createdDate}
					<span class="kind-badge">${escHtml(conv.kind)}</span>
					${nodeLabels}
					${codeCount > 0 ? `<span class="code-badge">${codeCount} code${deployed}</span>` : ''}
				</div>
			${conv.summary ? `<div class="history-card-summary">${escHtml(conv.summary.substring(0, 100))}...</div>` : ''}
		`;
		
		container.appendChild(card);
		});
	}
}

function displayPastConversation(conv) {
	// Display a past conversation in the right panel chat area
	if (!conv || !conv.id) return;
	
	// Display in right panel chat area
	const chatArea = document.getElementById('rp-chat-area');
	if (chatArea) {
		chatArea.innerHTML = '';
		
		// Title
		const title = document.createElement('div');
		title.style.cssText = `
			padding: 10px;
			border-bottom: 1px solid var(--border);
			font-weight: 600;
			font-size: 13px;
			background: var(--surface2);
		`;
		title.textContent = `${conv.title}`;
		chatArea.appendChild(title);
		
		// Messages container
		const messagesContainer = document.createElement('div');
		messagesContainer.style.cssText = `
			flex: 1;
			overflow-y: auto;
			padding: 10px;
			display: flex;
			flex-direction: column;
			gap: 8px;
		`;
		
		// Render all messages
		if (conv.messages && conv.messages.length > 0) {
			conv.messages.forEach(msg => {
				const msgEl = document.createElement('div');
				msgEl.style.cssText = `
					display: flex;
					flex-direction: column;
					gap: 4px;
					max-width: 85%;
					${msg.role === 'user' ? 'align-self: flex-end;' : 'align-self: flex-start;'}
				`;
				
				const bubble = document.createElement('div');
				bubble.style.cssText = `
					padding: 10px 14px;
					border-radius: 6px;
					font-size: 13px;
					line-height: 1.6;
					word-wrap: break-word;
					${msg.role === 'user' 
						? 'background: var(--accent); color: #fff; border-bottom-right-radius: 2px;' 
						: 'background: var(--surface); border: 1px solid var(--border); border-bottom-left-radius: 2px;'
					}
				`;
				bubble.textContent = msg.content || msg.text || '';
				
				msgEl.appendChild(bubble);
				messagesContainer.appendChild(msgEl);
			});
		} else {
			const empty = document.createElement('div');
			empty.style.cssText = 'color: var(--text-mute); font-size: 12px; text-align: center;';
			empty.textContent = 'No messages in this conversation';
			messagesContainer.appendChild(empty);
		}
		
		// Render code blocks
		if (conv.code_blocks && conv.code_blocks.length > 0) {
			conv.code_blocks.forEach(block => {
				if (block.language && block.code) {
					const status = block.status || 'proposed';
					const statusBadge = {
						'proposed': '📝',
						'accepted': '✓',
						'compiled': '✓ Built',
						'deployed': '✓ Deployed'
					}[status] || status;
					
					const deployedTo = block.deployed_to ? ` to ${block.deployed_to.join(', ')}` : '';
					
					const codeEl = document.createElement('div');
					codeEl.style.cssText = `
						background: var(--surface);
						border: 1px solid var(--border);
						border-radius: 6px;
						padding: 10px;
						font-family: var(--font-mono);
						font-size: 11px;
						overflow-x: auto;
						margin: 8px 0;
					`;
					codeEl.innerHTML = `
						<div style="color: var(--text-mute); font-size: 10px; margin-bottom: 6px;">${statusBadge} ${block.filename || 'code'} (${block.language})${deployedTo}</div>
						<pre style="margin: 0; white-space: pre-wrap; word-break: break-all;">${escHtml(block.code)}</pre>
					`;
					messagesContainer.appendChild(codeEl);
				}
			});
		}
		
		chatArea.appendChild(messagesContainer);
	}
}

function displayNodeFiles(nodeId, files, selectedFile) {
	// Display source files for a node in the center code view with edit capability
	const container = document.getElementById('code-content-area');
	if (!container) return;
	
	if (!files || files.length === 0) {
		return; // No files to display
	}

	container.innerHTML = '';

	// Store current file state for edit mode
	let currentFile = null;
	let editMode = false;

	// Header with file tabs and edit button
	const header = document.createElement('div');
	header.style.cssText = `
		display: flex;
		gap: 0;
		background: var(--surface);
		border-bottom: 1px solid var(--border);
		padding: 0;
		overflow-x: auto;
		align-items: center;
	`;

	files.forEach((file, idx) => {
		const tab = document.createElement('button');
		const isSelected = selectedFile === file.name || idx === 0;
		tab.style.cssText = `
			padding: 10px 16px;
			background: ${isSelected ? 'var(--bg)' : 'var(--surface)'};
			border: none;
			border-bottom: ${isSelected ? '2px solid var(--accent)' : '1px solid var(--border)'};
			color: ${isSelected ? 'var(--accent)' : 'var(--text-mute)'};
			font-family: var(--font-mono);
			font-size: 12px;
			cursor: pointer;
			white-space: nowrap;
			flex-shrink: 0;
		`;
		tab.textContent = file.name;

		tab.addEventListener('click', () => {
			if (editMode) return; // Prevent switching files while editing
			currentFile = file;
			// Update all tabs
			header.querySelectorAll('button.file-tab').forEach(t => {
				t.style.background = 'var(--surface)';
				t.style.borderBottom = '1px solid var(--border)';
				t.style.color = 'var(--text-mute)';
			});
			tab.style.background = 'var(--bg)';
			tab.style.borderBottom = '2px solid var(--accent)';
			tab.style.color = 'var(--accent)';

			// Show content
			viewer.textContent = file.content || '';
			editor.value = file.content || '';
		});

		tab.className = 'file-tab';
		header.appendChild(tab);
	});

	// Edit/Save/Cancel buttons
	const actionBar = document.createElement('div');
	actionBar.style.cssText = `
		display: flex;
		gap: 6px;
		margin-left: auto;
		padding: 8px 12px;
		flex-shrink: 0;
	`;

	const editBtn = document.createElement('button');
	editBtn.textContent = 'Edit';
	editBtn.style.cssText = `
		background: var(--accent);
		color: #fff;
		border: none;
		border-radius: 4px;
		padding: 6px 12px;
		font-size: 12px;
		cursor: pointer;
	`;

	const saveBtn = document.createElement('button');
	saveBtn.textContent = 'Save';
	saveBtn.style.cssText = `
		background: var(--green);
		color: #000;
		border: none;
		border-radius: 4px;
		padding: 6px 12px;
		font-size: 12px;
		cursor: pointer;
		display: none;
	`;

	const cancelBtn = document.createElement('button');
	cancelBtn.textContent = 'Cancel';
	cancelBtn.style.cssText = `
		background: var(--surface2);
		color: var(--text);
		border: 1px solid var(--border);
		border-radius: 4px;
		padding: 6px 12px;
		font-size: 12px;
		cursor: pointer;
		display: none;
	`;

	editBtn.addEventListener('click', () => {
		editMode = true;
		viewer.style.display = 'none';
		editor.style.display = 'block';
		editBtn.style.display = 'none';
		saveBtn.style.display = 'block';
		cancelBtn.style.display = 'block';
	});

	saveBtn.addEventListener('click', () => {
		if (currentFile) {
			currentFile.content = editor.value;
		}
		editMode = false;
		viewer.textContent = editor.value;
		viewer.style.display = 'block';
		editor.style.display = 'none';
		editBtn.style.display = 'block';
		saveBtn.style.display = 'none';
		cancelBtn.style.display = 'none';
	});

	cancelBtn.addEventListener('click', () => {
		editMode = false;
		editor.value = currentFile ? currentFile.content : '';
		viewer.style.display = 'block';
		editor.style.display = 'none';
		editBtn.style.display = 'block';
		saveBtn.style.display = 'none';
		cancelBtn.style.display = 'none';
	});

	actionBar.appendChild(editBtn);
	actionBar.appendChild(saveBtn);
	actionBar.appendChild(cancelBtn);
	header.appendChild(actionBar);

	// Code viewer (read-only)
	const viewer = document.createElement('pre');
	viewer.style.cssText = `
		padding: 16px;
		margin: 0;
		background: var(--bg);
		font-family: var(--font-mono);
		font-size: 12px;
		line-height: 1.6;
		color: var(--text);
		white-space: pre-wrap;
		word-wrap: break-word;
		flex: 1;
		overflow: auto;
		display: none;
	`;

	// Code editor (textarea)
	const editor = document.createElement('textarea');
	editor.style.cssText = `
		padding: 16px;
		margin: 0;
		background: var(--surface2);
		font-family: var(--font-mono);
		font-size: 12px;
		line-height: 1.6;
		color: var(--text);
		border: none;
		flex: 1;
		overflow: auto;
		display: none;
		resize: none;
	`;
	editor.addEventListener('focus', () => {
		editor.style.borderColor = 'var(--accent)';
	});
	editor.addEventListener('blur', () => {
		editor.style.borderColor = 'transparent';
	});

	container.appendChild(header);
	container.appendChild(viewer);
	container.appendChild(editor);

	// Load first file by default
	if (files.length > 0) {
		setTimeout(() => {
			currentFile = files[0];
			viewer.style.display = 'block';
			viewer.textContent = files[0].content || '';
			editor.value = files[0].content || '';
			header.querySelector('button.file-tab').click();
		}, 0);
	}
}

// Node file requests
function requestNodeFiles(nodeId) {
	send({ type: 'get_node_files', node_id: nodeId });
}

// ---------------------------------------------------------------------------
// Heartbeat ping to keep connection alive
// ---------------------------------------------------------------------------
setInterval(() => {
	if (ws && ws.readyState === WebSocket.OPEN) {
		ws.send(JSON.stringify({ type: 'ping' }));
	}
}, 25000);

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------
connect();

// Initialize mobile drawer and swipe navigation
initMobileEnvironment();

// ---------------------------------------------------------------------------
// Mobile drawer and swipe navigation
// ---------------------------------------------------------------------------

function initMobileEnvironment() {
	const isMobile = window.innerWidth <= 800;
	if (!isMobile) return;
	
	// Update viewport for mobile
	let viewportMeta = document.querySelector('meta[name="viewport"]');
	if (!viewportMeta) {
		viewportMeta = document.createElement('meta');
		viewportMeta.name = 'viewport';
		viewportMeta.content = 'width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no';
		document.head.appendChild(viewportMeta);
	}
	
	// Create drawer toggle button
	const toggle = document.createElement('button');
	toggle.className = 'drawer-toggle';
	toggle.id = 'drawer-toggle-btn';
	toggle.textContent = '⚙';
	toggle.title = 'Show/hide context & tasks';
	document.body.appendChild(toggle);
	
	toggle.addEventListener('click', () => {
		const rightPanel = document.getElementById('panel-right');
		if (rightPanel) {
			const isOpen = rightPanel.classList.contains('drawer-open');
			if (isOpen) {
				rightPanel.classList.remove('drawer-open');
				rightPanel.classList.add('drawer-closed');
				toggle.style.background = 'var(--accent)';
			} else {
				rightPanel.classList.remove('drawer-closed');
				rightPanel.classList.add('drawer-open');
				toggle.style.background = 'var(--green)';
			}
		}
	});
	
	// Initialize right panel as drawer (closed)
	const rightPanel = document.getElementById('panel-right');
	if (rightPanel) {
		rightPanel.classList.add('drawer-closed');
	}
	
	// Initialize swipe detection
	initSwipeNavigation();
}

let touchStartX = 0;
let touchStartY = 0;

function initSwipeNavigation() {
	document.addEventListener('touchstart', (e) => {
		touchStartX = e.touches[0].clientX;
		touchStartY = e.touches[0].clientY;
	}, { passive: true });
	
	document.addEventListener('touchend', (e) => {
		if (!e.changedTouches.length) return;
		
		const touchEndX = e.changedTouches[0].clientX;
		const touchEndY = e.changedTouches[0].clientY;
		
		const deltaX = touchEndX - touchStartX;
		const deltaY = touchEndY - touchStartY;
		
		// Only trigger swipe if X movement is significant and Y movement is small
		if (Math.abs(deltaX) > 50 && Math.abs(deltaY) < 50) {
			if (deltaX > 50) {
				onSwipeRight();
			} else if (deltaX < -50) {
				onSwipeLeft();
			}
		}
	}, { passive: true });
}

function onSwipeRight() {
	const rightPanel = document.getElementById('panel-right');
	if (rightPanel && rightPanel.classList.contains('drawer-open')) {
		rightPanel.classList.remove('drawer-open');
		rightPanel.classList.add('drawer-closed');
		const toggle = document.getElementById('drawer-toggle-btn');
		if (toggle) toggle.style.background = 'var(--accent)';
	}
}

function onSwipeLeft() {
	const rightPanel = document.getElementById('panel-right');
	if (rightPanel) {
		rightPanel.classList.remove('drawer-closed');
		rightPanel.classList.add('drawer-open');
		const toggle = document.getElementById('drawer-toggle-btn');
		if (toggle) toggle.style.background = 'var(--green)';
	}
}

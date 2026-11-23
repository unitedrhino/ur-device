// 文件说明：显式开启的正式C++设备/后端/模型客户端主题验收；只经桌面UI输入，观察器不修改协议或主题。
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {spawn, execFile} = require('node:child_process');
const {randomUUID, createHash} = require('node:crypto');
const {StringDecoder} = require('node:string_decoder');
const {performance} = require('node:perf_hooks'); // 单调相对时点只关联本次控制流，不输出墙钟或进程身份。
const {createDeviceFailureObserver,createDeviceLifecycleObserver,endSessionViaUI} = require('./mcp_file_media_live.cjs');

// rounds固定两轮不同可见主题；不用直接赋DOM、RPC结果或假重放绕过原模型工具选择。
const rounds = Object.freeze([
  {theme: 'dark', input: '请调用客户端工具set_local_display_theme，将当前设备界面切换为深色主题dark。实际成功后请简短确认，不要调用其他工具。'},
  {theme: 'light', input: '请调用客户端工具set_local_display_theme，将当前设备界面切换为浅色主题light。实际成功后请简短确认，不要调用其他工具。'},
]);

// validateEnvironment仅准入串联UDP／None或带固定语料摘要的Realtime UDP，不允许文字替代原生语音。
function validateEnvironment(env) {
  assert.equal(env.UR_CLIENT_TOOL_LIVE, '1', 'explicit_live_opt_in_required');
  assert.ok(['cascaded_agentruntime', 'realtime_native'].includes(env.UR_CLIENT_TOOL_CHAIN));
  assert.ok(['udp', 'none'].includes(env.UR_MEDIA_TRANSPORT));
  if (env.UR_CLIENT_TOOL_AUDIO_REPLY !== undefined) {
    assert.equal(env.UR_CLIENT_TOOL_AUDIO_REPLY, '1');
    assert.equal(env.UR_MEDIA_TRANSPORT, 'udp');
    assert.equal(env.UR_CLIENT_TOOL_CHAIN, 'cascaded_agentruntime');
  }
  if (env.UR_CLIENT_TOOL_CANCEL_LATE !== undefined) {
    assert.equal(env.UR_CLIENT_TOOL_CANCEL_LATE,'1');
    assert.equal(env.UR_MEDIA_TRANSPORT,'udp');
  }
  if (env.UR_CLIENT_TOOL_DELAY_EXECUTE !== undefined) {
    assert.equal(env.UR_CLIENT_TOOL_DELAY_EXECUTE,'1');
    assert.equal(env.UR_MEDIA_TRANSPORT,'udp');
    assert.equal(env.UR_MQTT_HOST,'127.0.0.1');
    assert.equal(env.UR_DEVICE_MESSAGE_DIAGNOSTICS,'1');
    assert.equal(env.UR_CLIENT_TOOL_CANCEL_LATE,undefined,'independent_fault_injection_required');
  }
  if (env.UR_CLIENT_TOOL_CHAIN === 'realtime_native') {
    assert.equal(env.UR_MEDIA_TRANSPORT, 'udp');
    for (const name of ['DARK', 'LIGHT']) {
      assert.ok(path.isAbsolute(env['UR_CLIENT_TOOL_' + name + '_WAV'] || ''));
      assert.ok(/^[a-f0-9]{64}$/.test(env['UR_CLIENT_TOOL_' + name + '_SHA256'] || ''));
    }
  }
  assert.equal(env.AGENT_BROWSER_HEADED, '1', 'headed_browser_required');
  assert.ok(typeof env.DISPLAY === 'string' && env.DISPLAY.length > 0);
  assert.ok(path.isAbsolute(env.XAUTHORITY || ''));
  assert.ok(!env.UR_OUTPUT_FOCUS && !env.UR_FILE_CATALOG && !env.UR_LIVE_CATALOG);
  for (const key of ['UR_DEVICE_PRODUCT', 'UR_DEVICE_NAME', 'UR_DEVICE_SECRET', 'UR_MQTT_HOST', 'UR_MQTT_PORT'])
    assert.ok(typeof env[key] === 'string' && env[key].length > 0 && !/[\r\n\x00]/.test(env[key]), 'device_configuration_missing');
  assert.ok(/^[1-9][0-9]{3,4}$/.test(env.UR_BROWSER_PORT || '') && Number(env.UR_BROWSER_PORT) >= 1024 && Number(env.UR_BROWSER_PORT) <= 65535);
  assert.ok(path.isAbsolute(env.UR_DEVICE_BROWSER_BIN || '') && fs.statSync(env.UR_DEVICE_BROWSER_BIN).isFile());
  return env;
}

// installObserver仅观察正式收发事实，原函数只执行一次，返回/参数/网络发送保持不变。
function installObserver(projectMessages = () => null) {
  if (globalThis.__urClientToolLive) throw Error('observer_already_installed');
  const observed = {accepted: 0, submitted: 0, rejected: 0, localTextAttempts: 0, localTextSent: 0,
    recognized: 0, expectedKeyword: '', recognizedThisRound: false,
    overflow: false, tools: [], messageDiagnostics: null,releaseCategory:null,failedIncomingKind:null};
  globalThis.__urClientToolLive = observed;
  // 原release第一调用仅匹配现有固定文案；不保存自由reason，也不把调用意图称作已释放。
  if(typeof release==='function') {
    const originalRelease=release;
    const categories=new Map([['本地媒体报文非法，已停止采集与播放。','invalid_local_message'],
      ['浏览器音频驱动失败，已释放麦克风。','audio_driver'],['浏览器采集实态无效，已释放媒体。','audio_driver'],
      ['浏览器音频消费事实无效，已释放媒体。','audio_driver'],['本机连接已关闭，麦克风已释放。','bridge_closed'],
      ['本机连接故障，麦克风已释放。','bridge_error'],['会话已结束，麦克风已释放。','session_ended'],
      ['设备链路失败，麦克风已释放。','device_error'],['页面已关闭。','pagehide']]);
    release=function(reason) {
      if(observed.releaseCategory===null)observed.releaseCategory=typeof reason==='string'&&categories.has(reason)?categories.get(reason):'unknown';
      return originalRelease.apply(this,arguments);
    };
  }
  const original = incoming;
  incoming = function(message) {
    let result;
    try {result=original(message);}catch(error) {
      // 只保留首次失败原kind的固定分类；读取旁证失败不能替换原异常或重复业务处理。
      if(observed.failedIncomingKind===null) {
        try {const kind=Object.getOwnPropertyDescriptor(message,'kind')?.value;
          observed.failedIncomingKind=['state','text','toolStatus','clientThemeSet','clientToolSubmitted','messageDiagnostic',
            'responseCompleted','responseMediaFinal','responseCancelled','textStatus','textSubmitted','captureStart','captureStop',
            'playbackStart','playbackStop','playbackFrame','playbackGate','ended','error'].includes(kind)?kind:'unknown';
        }catch{observed.failedIncomingKind='unknown';}
      }
      throw error;
    }
    if (message.kind === 'messageDiagnostic')
      observed.messageDiagnostics = projectMessages(message); // 只保存固定投影，不留原消息引用。
    if (message.kind === 'text' && typeof message.text === 'string' && message.text.startsWith('\n用户：') &&
        observed.expectedKeyword && !observed.recognizedThisRound &&
        message.text.slice(4).split('\n助手：')[0].includes(observed.expectedKeyword)) {
      ++observed.recognized; observed.recognizedThisRound = true;
    }
    if (message.kind === 'textStatus' || message.kind === 'textSubmitted') {
      if (message.kind === 'textStatus' && message.result === 'accepted') ++observed.accepted;
      else if (message.kind === 'textSubmitted' && message.result === 'submitted') ++observed.submitted;
      else ++observed.rejected;
    }
    if (message.kind === 'toolStatus') {
      if (observed.tools.length >= 16) observed.overflow = true;
      else observed.tools.push({id: message.id, response: message.responseId, state: message.state,
        ...(message.state === 'started' ? {approved: message.name === 'set_local_display_theme'} : {})});
    }
    return result;
  };
  const originalSend = send; // 只保留原函数关联；计数不能替代C++提交、云端受理或设备实际结果。
  send = function(message) {
    const textAttempt = message?.kind === 'sendText';
    if (textAttempt) {
      if (observed.localTextAttempts >= 16) observed.overflow = true;
      else ++observed.localTextAttempts;
    }
    const result = originalSend(message);
    if (textAttempt && result === true) {
      if (observed.localTextSent >= 16) observed.overflow = true;
      else ++observed.localTextSent;
    }
    return result;
  };
}

// readFacts只读当前正式DOM、原ACK/显示工具/原回复关联；标识仅验收进程内比较，不写报告。
function readFacts() {
  const observer = globalThis.__urClientToolLive;
  const records = clientTheme ? Array.from(clientTheme.records.values()) : [];
  const last = records.at(-1), style = getComputedStyle(document.body), rect = document.body.getBoundingClientRect();
  return {accepted: observer.accepted, submitted: observer.submitted, rejected: observer.rejected,
    localTextAttempts: observer.localTextAttempts, localTextSent: observer.localTextSent,
    overflow: observer.overflow, tools: observer.tools, messageDiagnostics: observer.messageDiagnostics,
    releaseCategory:observer.releaseCategory,failedIncomingKind:observer.failedIncomingKind,
    completed: metrics.completedResponses, cancelled: metrics.cancelledResponses,
    completedID: completedResponseId, active: responseId !== '',
    audioState: metrics.lastAudioState, captureClosed: metrics.captureOpen === false,
    captureOpen: metrics.captureOpen === true, captured: metrics.captured, recognized: observer.recognized,
    interactionMode: metrics.interactionMode, playableResponses: metrics.playableResponses,
    consumedNonSilent: metrics.consumedNonSilent, pendingPlayback: pendingPlayback.size,
    playableResponse: metrics.lastPlayableResponseId, consumedResponse: metrics.lastPlayableConsumedResponseId,
    consumedResponseFrames: metrics.lastVoicedConsumedResponseFrames,
    receiptIndex: metrics.clientToolReceiptIndex, receipts: metrics.clientToolReceipts,
    actualRecords: records.length, actualStatus: last?.fact?.status || 'missing', actualTheme: last?.fact?.theme || '',
    theme: document.body.dataset.clientTheme || '', background: style.backgroundColor, foreground: style.color,
    visible: document.visibilityState === 'visible' && rect.width > 0 && rect.height > 0,
    driverReady: !!clientTheme && !clientTheme.closed, contextRunning: context?.state === 'running',
    textOnly: typeof textOnlyDevice !== 'undefined' && textOnlyDevice === true,
    mediaAbsent: typeof context === 'undefined' && typeof stream === 'undefined' && typeof rtcPeer === 'undefined',
    textEnabled: document.getElementById('sendText').disabled === false,
    textFieldEnabled: document.getElementById('textInput').disabled === false,
    audioChoiceEnabled: document.getElementById('textAudio').disabled === false,
    inputPresent: document.getElementById('textInput').value.trim().length > 0,
    localSocketOpen: !!socket && socket.readyState === WebSocket.OPEN, terminated: terminated === true};
}

// readStartupFacts 仅固定准备实态，无观察器时仍可诊断，不能保存输入、身份或自由异常正文。
function readStartupFacts() {
  return {captureState: metrics.captureOpen === true ? 'open' : metrics.captureOpen === false ? 'closed' : 'unknown',
    contextRunning: context?.state === 'running', driverReady: !!clientTheme && !clientTheme.closed,
    textEnabled: document.getElementById('sendText').disabled === false,
    activeReply: responseId !== '', terminated: terminated === true};
}

// verifyRound同时核唯一副作用、原Submit ACK明确续答和同原模型回复终态；未知/仅展示/其他工具均失败。
function verifyRound(round, before, after, transport = 'udp', chain = 'cascaded_agentruntime', audioReply = false) {
  assert.ok(['udp', 'none'].includes(transport));
  assert.ok(['cascaded_agentruntime', 'realtime_native'].includes(chain));
  const voice = chain === 'realtime_native';
  assert.equal(typeof audioReply, 'boolean');
  if (audioReply) assert.ok(transport === 'udp' && !voice);
  const audible = voice || audioReply; // 输入方式与输出实际播报分别验证，不能以文字终态代替音频。
  if (voice) assert.equal(transport, 'udp');
  for (const facts of [before, after]) {
    for (const key of ['accepted', 'submitted', 'rejected', 'localTextAttempts', 'localTextSent', 'completed', 'receiptIndex', 'actualRecords'])
      assert.ok(Number.isSafeInteger(facts[key]) && facts[key] >= 0, 'invalid_fixed_counter');
    assert.ok(Array.isArray(facts.tools) && facts.tools.length <= 16);
    assert.ok(Array.isArray(facts.receipts) && facts.receipts.length <= 32);
  }
  assert.equal(after.overflow, false);
  for (const key of ['accepted', 'submitted', 'localTextAttempts', 'localTextSent'])
    assert.equal(after[key], before[key] + (voice ? 0 : 1));
  assert.equal(after.rejected, before.rejected);
  assert.equal(after.completed, before.completed + 1);
  assert.equal(after.active, false);
  if (!audible) assert.equal(after.audioState, 'none');
  if (audioReply) {
    for (const key of ['playableResponses', 'consumedNonSilent'])
      assert.ok(Number.isSafeInteger(before[key]) && before[key] >= 0 &&
        Number.isSafeInteger(after[key]) && after[key] > before[key], 'decoded_reply_required');
    assert.equal(after.pendingPlayback, 0);
  }
  if (transport === 'none') {
    // None没有音频线程停止回执；必须检查两个时点都未创建任何媒体对象，而不是伪造captureClosed。
    for (const facts of [before, after]) {
      assert.equal(facts.textOnly, true);
      assert.equal(facts.mediaAbsent, true);
      assert.equal(facts.contextRunning, false);
      assert.equal(facts.audioChoiceEnabled, false);
    }
  } else {
    if (voice) {
      assert.equal(after.captureClosed, false);
      assert.equal(before.captureOpen, true);
      assert.equal(after.captureOpen, true);
      assert.equal(after.interactionMode, 'fullDuplex');
      for (const key of ['captured', 'recognized', 'playableResponses', 'consumedNonSilent']) {
        assert.ok(Number.isSafeInteger(before[key]) && before[key] >= 0 &&
          Number.isSafeInteger(after[key]) && after[key] > before[key], 'voice_media_fact_required');
      }
      assert.equal(after.recognized, before.recognized + 1);
      assert.equal(after.pendingPlayback, 0);
    } else assert.equal(after.captureClosed, true);
    assert.equal(after.contextRunning, true);
  }
  assert.equal(after.visible, true);
  assert.equal(after.driverReady, true);
  assert.equal(after.actualRecords, before.actualRecords + 1);
  assert.equal(after.actualStatus, 'succeeded');
  assert.equal(after.actualTheme, round.theme);
  assert.equal(after.theme, round.theme);
  const palettes = {dark: ['rgb(15, 23, 42)', 'rgb(248, 250, 252)'], light: ['rgb(248, 250, 252)', 'rgb(15, 23, 42)']};
  assert.deepEqual([after.background, after.foreground], palettes[round.theme]);
  assert.equal(after.receiptIndex, before.receiptIndex + 1);
  assert.equal(after.receipts.length, before.receipts.length + 1);
  const receipt = after.receipts.at(-1), previous = before.receipts.at(-1);
  assert.equal(receipt.receiptIndex, after.receiptIndex);
  assert.equal(receipt.requestMethod, 'toolCallSubmit');
  assert.equal(receipt.code, 200);
  assert.equal(receipt.continuation, 'accepted');
  for (const key of ['sessionId', 'responseId', 'toolCallId']) assert.ok(typeof receipt[key] === 'string' && receipt[key].length > 0);
  if (previous) {
    assert.equal(receipt.sessionId, previous.sessionId, 'same_original_session_required');
    assert.notEqual(receipt.toolCallId, previous.toolCallId);
  }
  assert.equal(after.completedID, receipt.responseId);
  if (audible) {
    assert.equal(after.playableResponse, receipt.responseId);
    assert.equal(after.consumedResponse, receipt.responseId);
    assert.ok(Number.isSafeInteger(after.consumedResponseFrames) && after.consumedResponseFrames >= 2);
  }
  assert.notEqual(after.completedID, before.completedID);
  const tools = after.tools.slice(before.tools.length);
  assert.equal(tools.length, 2, 'one_original_tool_required');
  assert.equal(tools[0].state, 'started');
  assert.equal(tools[0].approved, true);
  assert.equal(tools[1].state, 'succeeded');
  for (const tool of tools) {
    assert.equal(tool.id, receipt.toolCallId);
    assert.equal(tool.response, receipt.responseId);
  }
  return {theme: round.theme, passed: true, actualVisibleTheme: true, uniqueLocalEffect: true,
    originalSubmitAccepted: true, continuation: 'accepted', originalReplyCompleted: true,
    ...(audible ? {originalReplyDecodedAndConsumed: true} : {}),
    ...(voice ? {recognizedSpeech: true, continuousInput: true} : {})};
}

// voiceWave只接受有界16k／PCM16单声道及连续两个有效语音帧，输入摘要必须匹配原固定语料。
function voiceWave(bytes, expected) {
  assert.ok(Buffer.isBuffer(bytes) && bytes.length > 44 && bytes.length <= 384128);
  assert.equal(createHash('sha256').update(bytes).digest('hex'), expected, 'voice_fixture_hash_changed');
  assert.equal(bytes.toString('ascii', 0, 4), 'RIFF');
  assert.equal(bytes.toString('ascii', 8, 12), 'WAVE');
  assert.equal(bytes.readUInt32LE(4), bytes.length - 8);
  let offset = 12, format = null, samples = null;
  while (offset + 8 <= bytes.length) {
    const name = bytes.toString('ascii', offset, offset + 4), size = bytes.readUInt32LE(offset + 4);
    assert.ok(offset + 8 + size <= bytes.length);
    const part = bytes.subarray(offset + 8, offset + 8 + size);
    if (name === 'fmt ') { assert.equal(format, null); assert.ok(size >= 16); format = part; }
    if (name === 'data') { assert.equal(samples, null); samples = part; }
    offset += 8 + size + size % 2;
  }
  assert.equal(offset, bytes.length);
  assert.ok(format && samples && samples.length >= 1280 && samples.length <= 384000 && samples.length % 2 === 0);
  assert.equal(format.readUInt16LE(0), 1); assert.equal(format.readUInt16LE(2), 1);
  assert.equal(format.readUInt32LE(4), 16000); assert.equal(format.readUInt32LE(8), 32000);
  assert.equal(format.readUInt16LE(12), 2); assert.equal(format.readUInt16LE(14), 16);
  let consecutive = 0, playable = false;
  for (let start = 0; start + 640 <= samples.length; start += 640) {
    let power = 0;
    for (let index = start; index < start + 640; index += 2) power += samples.readInt16LE(index) ** 2;
    consecutive = power >= 320 * 128 ** 2 ? consecutive + 1 : 0;
    if (consecutive >= 2) playable = true;
  }
  assert.ok(playable, 'voice_fixture_silent');
  return bytes;
}

// prepareVoiceInput将Chromium模拟麦克风的周期测试音替换为零源；仍由原Worklet持续采集和编码。
// 固定语料不得与浏览器自带音混合，否则供应商VAD和插话检测面对的不是报告中的原语料。
function prepareVoiceInput() {
  if (globalThis.__urClientToolSilence) throw Error('voice_input_already_prepared');
  source.disconnect();
  const silence = context.createConstantSource();
  silence.offset.value = 0;
  silence.connect(worklet);
  silence.start();
  globalThis.__urClientToolSilence = silence;
  return true;
}

// prepareSpeech仅把固定WAV接入原Worklet输入，不注入协议帧、识别结果或工具事实。
async function prepareSpeech(parts, keyword) {
  const chunks = parts.map(value => Uint8Array.from(atob(value), c => c.charCodeAt(0)));
  const bytes = new Uint8Array(chunks.reduce((n, value) => n + value.length, 0));
  globalThis.__urClientToolVoiceParts = []; // 清除分片，只让当前解码持有原语料。
  let offset = 0;
  for (const part of chunks) { bytes.set(part, offset); offset += part.length; }
  const decoded = await context.decodeAudioData(bytes.buffer);
  if (decoded.numberOfChannels !== 1 || decoded.duration <= 0 || decoded.duration > 12) throw Error('invalid_voice_fixture');
  const speech = context.createBufferSource(); speech.buffer = decoded; speech.connect(worklet);
  __urClientToolLive.expectedKeyword = keyword; __urClientToolLive.recognizedThisRound = false;
  globalThis.__urClientToolSpeech = {speech, ended: false};
  speech.onended = () => { speech.disconnect(); __urClientToolSpeech.ended = true; };
  speech.start(context.currentTime + 0.05);
  return true;
}

// projectMessageDiagnostics校验有界固定端接收快照；缺失/非法时保持未知，不反射报文或自由错误。
function projectMessageDiagnostics(value) {
  if (!value || typeof value !== 'object' || !Array.isArray(value.records) ||
      value.records.length > 32 || typeof value.overflow !== 'boolean') return null;
  const methods = new Set(['unknown', 'requestAck', 'respCreated', 'respSttDelta', 'respSttDone',
    'respTextDelta', 'respTextDone', 'respEmotion', 'respDone', 'toolCallStart', 'toolCallExecute', 'toolCallResult']);
  const outcomes = new Set(['no_session', 'draining', 'envelope_rejected', 'unsupported',
    'content_rejected', 'session_mismatch', 'watermark_discarded', 'tool_unavailable',
    'execute_accepted', 'execute_replay', 'execute_denied', 'execute_invalid',
    'ack_consumed', 'ack_rejected', 'scope_rejected', 'queue_full', 'content_queued']);
  const result = {overflow: value.overflow, records: []};
  for (const key of ['received', 'taken', 'hostFiltered', 'hostSent', 'hostSendFailed']) {
    if (!Number.isSafeInteger(value[key]) || value[key] < 0) return null;
    result[key] = value[key];
  }
  for (const row of value.records) {
    if (!row || !methods.has(row.method) || !outcomes.has(row.outcome)) return null;
    const record = {method: row.method, outcome: row.outcome};
    for (const key of ['sequence', 'watermark']) {
      if (!Number.isSafeInteger(row[key]) || row[key] < 0) return null;
      record[key] = row[key];
    }
    for (const key of ['parsed', 'sessionMatches', 'taken', 'hostObserved', 'projected', 'sent']) {
      if (typeof row[key] !== 'boolean') return null;
      record[key] = row[key];
    }
    result.records.push(record);
  }
  return result;
}

// projectDiagnostic仅固定枚举/布尔/有界计数，失败时不落原scope、主题票据、工具名称或页面正文。
function projectDiagnostic(facts) {
  if (!facts || typeof facts !== 'object') return null;
  const count = value => Number.isSafeInteger(value) && value >= 0 && value <= 512 ? value : null;
  const receipts = Array.isArray(facts.receipts) ? facts.receipts.slice(0, 32) : [];
  const tools = Array.isArray(facts.tools) ? facts.tools.slice(0, 16) : [];
  const original = receipts.at(-1);
  return {messageReception: projectMessageDiagnostics(facts.messageDiagnostics),
    accepted: count(facts.accepted), submitted: count(facts.submitted), rejected: count(facts.rejected),
    localTextAttempts: count(facts.localTextAttempts), localTextSent: count(facts.localTextSent),
    completedReplies: count(facts.completed), cancelledReplies: count(facts.cancelled),
    capturedFrames: Number.isSafeInteger(facts.captured) && facts.captured >= 0 ? facts.captured : null,
    recognizedUtterances: count(facts.recognized), playableResponses: count(facts.playableResponses),
    consumedNonSilentFrames: Number.isSafeInteger(facts.consumedNonSilent) && facts.consumedNonSilent >= 0 ? facts.consumedNonSilent : null,
    pendingPlayback: count(facts.pendingPlayback),
    lastReceiptMatchesPlayableReply: !!original && facts.playableResponse === original.responseId,
    lastReceiptMatchesConsumedReply: !!original && facts.consumedResponse === original.responseId,
    actualRecords: count(facts.actualRecords), receiptIndex: count(facts.receiptIndex),
    receiptCount: receipts.length, toolEvents: tools.length,
    releaseCategory:facts.releaseCategory===null||facts.releaseCategory===undefined?null:
      ['invalid_local_message','audio_driver','bridge_closed','bridge_error','session_ended','device_error','pagehide','unknown'].includes(facts.releaseCategory)?facts.releaseCategory:'unknown',
    failedIncomingKind:facts.failedIncomingKind===null||facts.failedIncomingKind===undefined?null:
      ['state','text','toolStatus','clientThemeSet','clientToolSubmitted','messageDiagnostic','responseCompleted','responseMediaFinal',
        'responseCancelled','textStatus','textSubmitted','captureStart','captureStop','playbackStart','playbackStop','playbackFrame',
        'playbackGate','ended','error','unknown'].includes(facts.failedIncomingKind)?facts.failedIncomingKind:'unknown',
    theme: ['light', 'dark'].includes(facts.theme) ? facts.theme : 'unknown',
    actualStatus: ['succeeded', 'failed', 'unknown', 'missing'].includes(facts.actualStatus) ? facts.actualStatus : 'unknown',
    driverReady: facts.driverReady === true, visible: facts.visible === true, captureClosed: facts.captureClosed === true,
    contextRunning: facts.contextRunning === true, activeReply: facts.active === true,
    textEnabled: facts.textEnabled === true, textFieldEnabled: facts.textFieldEnabled === true,
    audioChoiceEnabled: facts.audioChoiceEnabled === true,
    inputPresent: facts.inputPresent === true, localSocketOpen: facts.localSocketOpen === true,
    terminated: facts.terminated === true,
    acceptedContinuations: receipts.filter(value => value.code === 200 && value.continuation === 'accepted').length,
    unknownContinuations: receipts.filter(value => value.continuation === 'unknown').length,
    approvedStarts: tools.filter(value => value.state === 'started' && value.approved === true).length,
    succeededToolResults: tools.filter(value => value.state === 'succeeded').length,
    lastReceiptMatchesCompletedReply: !!original && typeof facts.completedID === 'string' && facts.completedID.length > 0 && facts.completedID === original.responseId};
}

// parseJSON仅接纳CLI固定对象（可有一层字符串包装），不回显自由失败正文。
function parseJSON(raw) {
  let value = JSON.parse(raw);
  if (typeof value === 'string') value = JSON.parse(value);
  assert.ok(value && typeof value === 'object' && !Array.isArray(value));
  return value;
}
// waitClosed等待真实close，不以SIGTERM受理代替释放；清掉本函数的等待计时器。
async function waitClosed(promise, timeout) {
  let timer;
  try { return await Promise.race([promise.then(() => true), new Promise(resolve => { timer = setTimeout(() => resolve(false), timeout); })]); }
  finally { clearTimeout(timer); }
}

// run复用真实device_browser/agent-browser；依赖注入仅用于离线控制面测试，环境不能替换实现。
async function run(env = process.env, deps = {}) {
  validateEnvironment(env);
  const voice = env.UR_CLIENT_TOOL_CHAIN === 'realtime_native';
  // 所有语料在spawn前验证并持有原字节，运行中不重读被替换的路径。
  const voices = voice ? rounds.map(round => {
    const name = round.theme.toUpperCase(), file = env['UR_CLIENT_TOOL_' + name + '_WAV'];
    const stat = fs.lstatSync(file);
    assert.ok(stat.isFile() && !stat.isSymbolicLink() && stat.size <= 384128);
    return voiceWave(fs.readFileSync(file), env['UR_CLIENT_TOOL_' + name + '_SHA256']);
  }) : [];
  // timing只记录真实原事件；未观察到的exit等保持null，最后成功快照时点不代表失败当前实态。
  const origin=performance.now(),timing={childExitMs:null,childCloseMs:null,waitFailedMs:null,
    finallyMs:null,browserCloseRequestedMs:null,latestFactsObservedMs:null,freshAfterFailure:null};
  // relative返回本次同单调时钟的安全毫秒值；异常时钟仅失去旁证，不能影响业务。
  function relative() {const elapsed=performance.now()-origin;return Number.isFinite(elapsed)&&elapsed>=0&&elapsed<=Number.MAX_SAFE_INTEGER?Math.floor(elapsed):null;}
  const mqttDelay = env.UR_CLIENT_TOOL_DELAY_EXECUTE === '1' ? await require('./mqtt_execute_delay.cjs').createExecuteDelay({
    targetPort:Number(env.UR_MQTT_PORT),topic:'$thing/down/message/' + env.UR_DEVICE_PRODUCT + '/' + env.UR_DEVICE_NAME}) : null;
  let child;
  try {child = (deps.spawn || spawn)(env.UR_DEVICE_BROWSER_BIN, [env.UR_BROWSER_PORT], {
    env:mqttDelay ? {...env,UR_MQTT_PORT:String(mqttDelay.port)} : env, stdio:['ignore','pipe','pipe']});}
  catch {await mqttDelay?.close();throw Error('device_spawn_failed');}
  const session = 'device-client-tool-' + randomUUID(), url = 'http://127.0.0.1:' + env.UR_BROWSER_PORT + '/';
  const deviceFailure = createDeviceFailureObserver(), decoder = new StringDecoder('utf8');
  const deviceLifecycle=createDeviceLifecycleObserver(env.UR_DEVICE_MESSAGE_DIAGNOSTICS === '1'); // 原显式开关才投影固定首因。
  let lifecycleUnavailable=false; // 旁证失败仅保留null，不改变原业务成功/失败或双流状态。
  let closed = false, exitCode = null, stdoutEnded = false, stderrEnded = false, streamFailed = false;
  let ready = false, buffered = '', phase = 'deviceStart', successful = false, failureReport, latestFacts = null, startupFacts = null;
  let roundStep = ''; // 仅固定原UI步骤，失败时定位最后等待点，不改变请求、重试或任何通过条件。
  const results = []; let cancellationProbe,executeLateProbe; // 独立故障注入，后续两轮正常模型回复保持原会话。
  child.once('exit',()=>{timing.childExitMs=relative();}); // exit不冒充双流结束或原close。
  const exited = new Promise(resolve => child.once('close', code => { timing.childCloseMs=relative();closed = true; exitCode = code; resolve(); }));
  child.once('error', () => { streamFailed = true; });
  for (const stream of [child.stdout, child.stderr]) stream.on('error', () => { streamFailed = true; });
  child.stdout.on('data', bytes => {
    try {deviceLifecycle.push(bytes);}catch{lifecycleUnavailable=true;}
    buffered += decoder.write(bytes);
    if (buffered.length > 8192) { streamFailed = true; buffered = ''; return; }
    let end;
    while ((end = buffered.indexOf('\n')) >= 0) {
      if (buffered.slice(0, end) === url) ready = true;
      buffered = buffered.slice(end + 1);
    }
  });
  child.stdout.once('end', () => { stdoutEnded = true; if (decoder.end() || buffered.length) streamFailed = true;
    try {deviceLifecycle.finish();}catch{lifecycleUnavailable=true;} });
  child.stderr.on('data', bytes => deviceFailure.push(bytes));
  child.stderr.once('end', () => { stderrEnded = true; deviceFailure.finish(); });
  const invoke = (...args) => new Promise((resolve, reject) => {
    const command = (deps.execFile || execFile)('agent-browser', ['--session', session, '--headed', ...args],
      {env: {...env, AGENT_BROWSER_ARGS: [env.AGENT_BROWSER_ARGS || '--no-sandbox', '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream'].join(',')},
        timeout: args[0] === 'wait' ? 60000 : 20000, maxBuffer: 65536},
      (error, stdout) => error ? reject(Error('browser_command_failed')) : resolve(stdout.trim()));
    if (deps.onBrowserCommand && command?.once) command.once('close', () => { try { deps.onBrowserCommand(); } catch { streamFailed = true; } });
  });
  const snapshot = async () => {
    latestFacts = parseJSON(await invoke('eval', '(' + readFacts.toString() + ')()'));
    timing.latestFactsObservedMs=relative();
    return latestFacts;
  };
  const timer = setTimeout(() => { streamFailed = true; if (!closed) child.kill('SIGTERM'); }, 180000);
  try {
    if (deps.onSpawn) await deps.onSpawn(child);
    const started = Date.now();
    while (!ready) {
      assert.ok(!closed && !streamFailed && Date.now() - started < 10000, 'device_start_failed');
      await new Promise(resolve => setTimeout(resolve, 10));
    }
    phase = 'sessionReady';
    await invoke('open', url);
    await invoke('click', '#activate');
    await invoke('wait', '--fn', (voice ? '!document.getElementById("start").disabled' :
      '!document.getElementById("sendText").disabled') + ' && clientTheme && !clientTheme.closed');
    // 建会不等于开始输入；初始零输入或原停止均须音频线程实态，本用例不额外制造录音轮次。
    phase = 'capturePause';
    if (env.UR_MEDIA_TRANSPORT === 'none')
      await invoke('wait', '--fn', 'textOnlyDevice === true && typeof context === "undefined" && typeof stream === "undefined" && typeof rtcPeer === "undefined" && responseId === ""');
    else if (!voice) {
      await invoke('click', '#stop');
      await invoke('wait', '--fn', 'metrics.captureOpen === false && responseId === ""');
    }
    phase = 'observerReady';
    await invoke('eval', '(' + installObserver.toString() + ')(' + projectMessageDiagnostics.toString() + ')');
    if (voice) {
      await invoke('eval', '(' + prepareVoiceInput.toString() + ')()');
      await invoke('click', '#start');
      await invoke('wait', '--fn', 'metrics.captureOpen === true && metrics.interactionMode === "fullDuplex"');
    }
    if (env.UR_CLIENT_TOOL_CANCEL_LATE === '1') {
      phase = 'cancelLate';
      cancellationProbe = await require('./browser_client_tool_cancel_live.cjs').runProbe({
        invoke,snapshot,voice,wave:voice ? voices[0] : null,prepareSpeech,parseJSON});
    }
    if (mqttDelay) {
      phase = 'executeLate';
      executeLateProbe = await require('./browser_client_tool_execute_late_live.cjs').runExecuteLateProbe({
        invoke,snapshot,voice,wave:voice ? voices[0] : null,prepareSpeech,parseJSON,delay:mqttDelay});
    }
    for (const round of rounds) {
      phase = round.theme;
      roundStep = 'observeBefore';
      const before = await snapshot();
      roundStep = 'validateInput';
      if (env.UR_MEDIA_TRANSPORT === 'none') assert.equal(before.mediaAbsent, true);
      else assert.equal(voice ? before.captureOpen : before.captureClosed, true);
      assert.equal(before.active, false);
      roundStep = 'fillInput';
      if (voice) {
        const bytes = voices[rounds.indexOf(round)], parts = [];
        for (let offset = 0; offset < bytes.length; offset += 16384)
          parts.push(bytes.subarray(offset, offset + 16384).toString('base64'));
        roundStep = 'submitVoice';
        // 单次CLI有界输入WAV，不操作原incoming／send或给设备构造任何报文。
        await invoke('eval', 'globalThis.__urClientToolVoiceParts=[];true');
        for (const part of parts) await invoke('eval', '__urClientToolVoiceParts.push(' + JSON.stringify(part) + ');true');
        await invoke('eval', '(' + prepareSpeech.toString() + ')(__urClientToolVoiceParts,' +
          JSON.stringify(round.theme === 'dark' ? '深色' : '浅色') + ')');
        await invoke('wait', '--fn', '__urClientToolSpeech.ended === true');
      } else {
        await invoke('fill', '#textInput', round.input);
        roundStep = 'audioChoice';
        if (env.UR_MEDIA_TRANSPORT !== 'none')
          await invoke(env.UR_CLIENT_TOOL_AUDIO_REPLY === '1' ? 'check' : 'uncheck', '#textAudio');
        roundStep = 'submitInput';
        await invoke('click', '#sendText');
        roundStep = 'observeSubmission';
        const sent = await snapshot(); // 只读原send实态，CLI点击返回不能代替按钮处理或WebSocket发送受理。
        assert.equal(sent.localTextAttempts, before.localTextAttempts + 1);
        assert.equal(sent.localTextSent, before.localTextSent + 1);
      }
      roundStep = 'awaitReply';
      try {await invoke('wait', '--fn', 'metrics.completedResponses === ' + (before.completed + 1) + ' && responseId === ""');}
      catch(error){timing.waitFailedMs=relative();throw error;}
      roundStep = 'verifyRound';
      if (voice || env.UR_CLIENT_TOOL_AUDIO_REPLY === '1') await invoke('wait', '--fn', 'pendingPlayback.size === 0');
      results.push(verifyRound(round, before, await snapshot(), env.UR_MEDIA_TRANSPORT, env.UR_CLIENT_TOOL_CHAIN,
        env.UR_CLIENT_TOOL_AUDIO_REPLY === '1'));
    }
    phase = 'sessionClose';
    await endSessionViaUI(invoke);
    assert.equal(await waitClosed(exited, deps.closeTimeoutMs || 10000), true);
    assert.equal(exitCode, 0);
    successful = true;
  } catch {
    failureReport = true; // 自由异常/页面正文只在内存，不进入日志或诊断。
    if (['dark', 'light','cancelLate','executeLate','sessionClose'].includes(phase)) {
      latestFacts = null; // 失败时必须重新采样；未知实态不能沿用提交前计数解释真实失败。
      timing.freshAfterFailure=false;
      // 页面可在原CPP退出后仍存在；这次成功读取只证明页面新快照，不证明设备仍在线。
      await snapshot().then(()=>{timing.freshAfterFailure=true;},()=>null);
      if (phase === 'cancelLate') {
        // 取消旁证只保留固定事实，避免把未发送、无目标和后端未终结混成同一超时。
        try {
          const facts = parseJSON(await invoke('eval','(' + require('./browser_client_tool_cancel_live.cjs').cancellationFacts.toString() + ')()'));
          const diagnostic = {};
          for (const key of ['held','commandHeld','cancelSent','active','terminated','bridgeOpen','controlReady','cancelEnabled'])
            diagnostic[key] = typeof facts[key] === 'boolean' ? facts[key] : null;
          cancellationProbe = {passed:false,diagnostic};
        } catch {} // 观察失败保持未知，不生成原取消事实。
      }
    }
    if (!closed && ['sessionReady', 'capturePause', 'observerReady'].includes(phase)) {
      try {
        const value = parseJSON(await invoke('eval', '(' + readStartupFacts.toString() + ')()'));
        assert.deepEqual(Object.keys(value).sort(), ['activeReply', 'captureState', 'contextRunning', 'driverReady', 'terminated', 'textEnabled'].sort());
        assert.ok(['open', 'closed', 'unknown'].includes(value.captureState));
        for (const key of ['activeReply', 'contextRunning', 'driverReady', 'terminated', 'textEnabled']) assert.equal(typeof value[key], 'boolean');
        startupFacts = value;
      } catch {} // 未成功观察保留null，不补造失败前的页面实态。
    }
  } finally {
    timing.finallyMs=relative();
    clearTimeout(timer);
    timing.browserCloseRequestedMs=relative();
    const browserClosed = await invoke('close').then(() => true, () => false);
    if (!closed) {
      child.kill('SIGTERM');
      if (!await waitClosed(exited, deps.closeTimeoutMs || 8000) && !closed) {
        child.kill('SIGKILL');
        await waitClosed(exited, deps.closeTimeoutMs || 8000);
      }
    }
    if (mqttDelay) {
      await mqttDelay.close().catch(() => {streamFailed = true;});
      if (mqttDelay.facts().failed || !mqttDelay.facts().connectionsClosed) streamFailed = true;
    }
    const cleanup = {browserClosed, childClosed: closed, stdoutEnded, stderrEnded};
    const passed = successful && !failureReport && !streamFailed && browserClosed && closed && stdoutEnded && stderrEnded && exitCode === 0;
    const report = {passed, scenario: 'clientDisplayTheme', evidence: deps.spawn || deps.execFile ? 'offline-control-fixture' : 'cpp-backend-model',
      phase: passed ? 'completed' : phase, results, cleanup, timing:{...timing},
      ...(cancellationProbe ? {cancellationProbe} : {}),
      ...(mqttDelay ? {executeLateProbe:executeLateProbe || {passed:false},mqttDelay:mqttDelay.facts()} : {}),
      deviceLifecycle:lifecycleUnavailable?null:deviceLifecycle.facts(),
      ...(!passed ? {diagnostic: {roundStep, startupFacts, uiFacts: projectDiagnostic(latestFacts), deviceFailure: stderrEnded ? deviceFailure.finish() : null, deviceExitCode: exitCode}} : {})};
    (deps.report || console.log)(JSON.stringify(report));
    if (!passed) throw Error('client_tool_live_failed');
    return report;
  }
}
module.exports = {rounds, validateEnvironment, installObserver, readFacts, readStartupFacts, verifyRound,
  voiceWave, prepareVoiceInput, prepareSpeech, projectMessageDiagnostics, projectDiagnostic, parseJSON, waitClosed, run};
if (require.main === module) run().catch(() => { console.error('client_tool_live_failed'); process.exitCode = 1; });

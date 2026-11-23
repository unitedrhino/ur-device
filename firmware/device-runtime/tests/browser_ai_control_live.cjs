// 文件说明：真实C++设备短MQTT断网恢复验收；浏览器只发固定本机操作，凭据和SIG始终由原native宿主持有。
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {spawn, execFile} = require('node:child_process');
const {randomUUID, createHash} = require('node:crypto');
const {parseJSON, waitClosed} = require('./browser_webrtc_text_live.cjs');
const {createDeviceFailureObserver, readStartupFacts} = require('./mcp_file_media_live.cjs');

// validateEnvironment 限已部署的真实AI安全子集；不注册身份、不写模型绑定、不接受脚本或桥凭据。
function validateEnvironment(env) {
  assert.equal(env.UR_AI_CONTROL_LIVE, '1');
  assert.equal(env.UR_DEVICE_CONTROL_SYNC, '1');
  assert.ok(['udp', 'webrtc'].includes(env.UR_MEDIA_TRANSPORT));
  assert.ok(['cascaded_agentruntime', 'realtime_native'].includes(env.UR_AI_CONTROL_CHAIN));
  assert.ok(['active', 'stopped', 'expired'].includes(env.UR_AI_CONTROL_VARIANT));
  assert.ok(!env.UR_OUTPUT_FOCUS && !env.UR_FILE_CATALOG && !env.UR_AI_VISION_CAMERA);
  const port = Number(env.UR_BROWSER_PORT);
  assert.ok(Number.isInteger(port) && port >= 1024 && port <= 65535);
  for (const key of ['UR_DEVICE_PRODUCT', 'UR_DEVICE_NAME', 'UR_DEVICE_SECRET', 'UR_MQTT_HOST', 'UR_MQTT_PORT'])
    assert.ok(typeof env[key] === 'string' && env[key].length && !/[\r\n\x00]/.test(env[key]));
  for (const key of ['UR_DEVICE_BROWSER_BIN', 'UR_BROWSER_INPUT_WAV'])
    assert.ok(path.isAbsolute(env[key] || '') && fs.statSync(env[key]).isFile());
  const bytes = fs.readFileSync(env.UR_BROWSER_INPUT_WAV);
  // 16k/mono/16bit十秒PCM的有界预算与实际decode时长一致；不将已批准7.96秒语料截短换句。
  assert.ok(bytes.length > 44 && bytes.length <= 320044 && bytes.toString('ascii', 0, 4) === 'RIFF');
  const keywords = (env.UR_BROWSER_EXPECT_ASR_KEYWORDS || '').split(',').filter(Boolean);
  assert.ok(keywords.length > 0 && keywords.length <= 10 && keywords.every(value => value.length <= 64 && !/[\r\n\x00]/.test(value)));
  return {port, transport: env.UR_MEDIA_TRANSPORT, chain: env.UR_AI_CONTROL_CHAIN,
    variant: env.UR_AI_CONTROL_VARIANT, bytes, keywords};
}

// installObserver 观察原incoming实际接纳的唯一控制/采集命令；ID只在页面内比对，不输出身份。
function installObserver() {
  if (globalThis.__urAIControl) throw Error('observer_already_installed');
  globalThis.__urAIControl = {starts: 0, stops: 0, configurations: 0, lastInput: '',
    lossInput: '', lossPeer: null, lossPeerRevision: '', sequence: [], fixtureEnded: 0,
    recognized: 0, recognizedID: '', recognizedText: '', keywords: []};
  const original = incoming;
  incoming = function(value) {
    const result = original(value);
    const fact = globalThis.__urAIControl;
    if (value.kind === 'captureStart') { ++fact.starts; fact.lastInput = value.id; }
    if (value.kind === 'captureStop') ++fact.stops;
    if (value.kind === 'webrtcConfigure' || value.kind === 'webrtcRecover') ++fact.configurations;
    if (value.kind === 'controlRecovery') {
      if (fact.sequence.length >= 32) throw Error('control_fact_overflow');
      fact.sequence.push({...value});
    }
    if (value.kind === 'text' && typeof value.responseId === 'string' &&
        value.text.startsWith('\n用户：')) {
      if (value.text.length > 4096) throw Error('recognition_overflow');
      ++fact.recognized; fact.recognizedID = value.responseId; fact.recognizedText = value.text;
    }
    return result;
  };
}

// installFixture 使用真实WebAudio单次有界语料；静音床防止循环假麦在控制窗口内制造新的用户输入。
async function installFixture() {
  const bytes = Uint8Array.from(atob(globalThis.__urFixtureChunks.join('')), ch => ch.charCodeAt(0));
  delete globalThis.__urFixtureChunks;
  const decoded = await context.decodeAudioData(bytes.buffer);
  if (decoded.numberOfChannels !== 1 || decoded.duration <= 0 || decoded.duration > 10)
    throw Error('invalid_fixture');
  let target = worklet;
  if (useWebRTC) {
    const destination = context.createMediaStreamDestination();
    const old = stream, track = destination.stream.getAudioTracks()[0];
    track.enabled = false;
    const sender = rtcPeer.getSenders().find(item => item.track?.kind === 'audio');
    if (!sender || !old?.getAudioTracks().includes(sender.track)) throw Error('missing_sender');
    try { await sender.replaceTrack(track); stream = destination.stream; }
    catch (error) { track.stop(); throw error; }
    old.getTracks().forEach(item => item.stop());
    const bed = context.createConstantSource(); bed.offset.value = 0;
    bed.connect(destination); bed.start();
    globalThis.__urAIControl.silentBed = bed;
    target = destination;
  } else {
    source.disconnect();
    // UDP逐轮语料之外仍保留真实零值输入块；否则采集与完整组false票据没有处理线程事实。
    const bed = context.createConstantSource(); bed.offset.value = 0;
    bed.connect(target); bed.start();
    globalThis.__urAIControl.silentBed = bed;
  }
  globalThis.__urControlPlay = function() {
    const node = context.createBufferSource(); node.buffer = decoded; node.connect(target);
    node.onended = () => { node.disconnect(); ++globalThis.__urAIControl.fixtureEnded; };
    node.start(context.currentTime + 0.05);
  };
  return true;
}

// readFacts 区分实际Worklet/Sender开关和控制ready；原reply身份仅在进程内核完成与有声消费关联。
async function readFacts() {
  const fact = globalThis.__urAIControl;
  let sent = 0, received = 0;
  if (useWebRTC && rtcPeer) {
    (await rtcPeer.getStats()).forEach(item => {
      if (item.type === 'outbound-rtp' && item.kind === 'audio') sent += item.packetsSent || 0;
      if (item.type === 'inbound-rtp' && item.kind === 'audio') received += item.packetsReceived || 0;
    });
  }
  const captureOpen = useWebRTC ? Boolean(rtcPeer?.getSenders().some(item => item.track?.kind === 'audio' && item.track.enabled)) : metrics.captureOpen;
  return {control: aiControlState, starts: fact.starts, stops: fact.stops,
    configurations: fact.configurations, fixtureEnded: fact.fixtureEnded,
    captureOpen, outputStopped: responseId === '',
    peerSame: !useWebRTC || (rtcPeer === fact.lossPeer && rtcPeerRevision === fact.lossPeerRevision),
    freshInput: Boolean(fact.lossInput && fact.lastInput && fact.lossInput !== fact.lastInput),
    recognized: fact.recognized, recognizedSameReply: Boolean(fact.recognizedID && fact.recognizedID === completedResponseId),
    keywordMatched: fact.keywords.length > 0 && fact.keywords.every(value => fact.recognizedText.includes(value)),
    completed: metrics.completedResponses, completedID: completedResponseId,
    playableID: metrics.lastPlayableConsumedResponseId, voiced: metrics.consumedNonSilent,
    captured: metrics.captured, network: metrics.lastNetworkPackets, sent, received,
    // 只读原媒体与Worklet账本，网络包、解码帧、实际消费及连续有声分别保留，绝不补造可播放。
    decoded: metrics.lastDecodedFrames, decodedNonSilent: metrics.lastNonSilentFrames,
    consumed: metrics.consumed, consumedNonSilent: metrics.consumedNonSilent,
    currentReplyConsumed, consecutiveConsumedNonSilent: currentReplyConsecutiveConsumedNonSilent,
    pendingPlaybackFrames: pendingPlayback.size, missingPackets: metrics.lastMissingPackets,
    observedComplete: metrics.lastObservedComplete, lastAudioState: metrics.lastAudioState,
    completedReplyPlayable: completedResponseId !== '' && metrics.lastPlayableConsumedResponseId === completedResponseId,
    completedReplyMediaFinal: completedResponseId !== '' && metrics.lastMediaFinalResponseId === completedResponseId,
    replyTiming: metrics.lastReplyTiming && metrics.lastReplyTiming.responseId === completedResponseId
      ? {lastVoiceSentMs: metrics.lastReplyTiming.lastVoiceSentMs,
        firstTextMs: metrics.lastReplyTiming.firstTextMs,
        firstNetworkMs: metrics.lastReplyTiming.firstNetworkMs,
        firstPlayableMs: metrics.lastReplyTiming.firstPlayableMs} : null,
    peer: useWebRTC ? rtcPeer?.connectionState : null, contextRunning: context?.state === 'running',
    terminated, fixtureReady: typeof globalThis.__urControlPlay === 'function'};
}

// verifyReplyTiming只相减原同一C++时钟；缺失/合法重叠明确不可测，不用WAV结束或网页时钟换起点。
function verifyReplyTiming(facts, transport) {
  const names = {firstTextMilliseconds: 'firstTextMs', firstNetworkAudioMilliseconds: 'firstNetworkMs',
    firstPlayableAudioMilliseconds: 'firstPlayableMs'};
  const result = {clock: 'cpp_monotonic_ms', firstTextMilliseconds: null,
    firstNetworkAudioMilliseconds: null, firstPlayableAudioMilliseconds: null,
    primaryMeasured: false, unavailable: {}, deviceConsumptionVerifiedNotTimed: true,
    physicalSpeakerOnsetMeasured: false};
  const unavailable = reason => { for (const key of Object.keys(names)) result.unavailable[key] = reason; return result; };
  if (transport === 'webrtc') return unavailable('webrtc_native_timestamps_unavailable');
  const timing = facts.replyTiming;
  if (!timing) return unavailable('trusted_native_timestamps_not_projected');
  if (typeof timing !== 'object' || Array.isArray(timing) ||
      Object.keys(timing).length !== 4 ||
      ['lastVoiceSentMs', ...Object.values(names)].some(key => !Object.hasOwn(timing, key) ||
        (timing[key] !== null && (!Number.isSafeInteger(timing[key]) || timing[key] < 0))))
    throw Error('invalid_reply_timing');
  if (timing.firstPlayableMs !== null && (timing.firstNetworkMs === null ||
      timing.firstPlayableMs < timing.firstNetworkMs)) throw Error('invalid_reply_timing');
  if (timing.lastVoiceSentMs === null) return unavailable('last_valid_voice_sent_unavailable');
  for (const [key, timestamp] of Object.entries(names)) {
    if (timing[timestamp] === null) result.unavailable[key] = 'native_timestamp_unavailable';
    else if (timing[timestamp] < timing.lastVoiceSentMs) result.unavailable[key] = 'overlapped_response';
    else result[key] = timing[timestamp] - timing.lastVoiceSentMs;
  }
  result.primaryMeasured = result.firstPlayableAudioMilliseconds !== null;
  return result;
}

// failureFacts只投影固定驱动事实；回复/输入/会话标识与任何自由正文不能进入失败报告。
function failureFacts(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return null;
  const result = {};
  for (const key of ['starts', 'stops', 'configurations', 'fixtureEnded', 'recognized', 'completed',
    'voiced', 'captured', 'network', 'sent', 'received', 'decoded', 'decodedNonSilent',
    'consumed', 'consumedNonSilent', 'currentReplyConsumed', 'consecutiveConsumedNonSilent',
    'pendingPlaybackFrames', 'missingPackets'])
    if (Number.isSafeInteger(value[key]) && value[key] >= 0) result[key] = value[key];
  for (const key of ['captureOpen', 'outputStopped', 'peerSame', 'freshInput', 'recognizedSameReply',
    'keywordMatched', 'contextRunning', 'terminated', 'fixtureReady', 'observedComplete',
    'completedReplyPlayable', 'completedReplyMediaFinal'])
    if (typeof value[key] === 'boolean') result[key] = value[key];
  if (['new', 'connecting', 'connected', 'disconnected', 'failed', 'closed'].includes(value.peer))
    result.peer = value.peer;
  if (['none', 'sent', 'cancelled', 'failed'].includes(value.lastAudioState))
    result.lastAudioState = value.lastAudioState;
  if (value.control && typeof value.control === 'object' && !Array.isArray(value.control)) {
    const control = {};
    if (['ready', 'disconnected', 'connecting', 'synchronizing', 'failed'].includes(value.control.state))
      control.state = value.control.state;
    for (const key of ['endpointEpoch', 'messageWatermark'])
      if (Number.isSafeInteger(value.control[key]) && value.control[key] >= 0) control[key] = value.control[key];
    if (typeof value.control.originalSessionPreserved === 'boolean')
      control.originalSessionPreserved = value.control.originalSessionPreserved;
    if (Object.keys(control).length) result.control = control;
  }
  return result;
}

// failureReason将首因归入固定白名单；包括断言与浏览器等待，绝不透传供应商/CLI原文。
function failureReason(error) {
  if (error?.code === 'ERR_ASSERTION') return 'criterion_failed';
  return ['device_start_timeout', 'device_spawn_failed', 'device_start_failed', 'browser_command_failed',
    'invalid_reply_timing'].includes(error?.message) ? error.message : 'scenario_failed';
}

// controlWindow 只通过正式本机桥断开/重新鉴权；不伪造MQTT、SUBACK、SIG或恢复快照。
async function controlWindow(variant) {
  const fact = globalThis.__urAIControl;
  fact.lossInput = fact.lastInput;
  fact.lossPeer = useWebRTC ? rtcPeer : null;
  fact.lossPeerRevision = useWebRTC ? rtcPeerRevision : '';
  const epoch = aiControlState.endpointEpoch;
  const started = performance.now();
  const wait = async (condition, limit) => {
    while (!condition()) {
      if (performance.now() - started >= limit) throw Error('control_window_timeout');
      await new Promise(resolve => setTimeout(resolve, 20));
    }
  };
  const closed = () => useWebRTC ? rtcPeer?.getSenders().every(item => item.track?.kind !== 'audio' || !item.track.enabled) : metrics.captureOpen === false;
  if (!send({kind: 'controlDisconnect'})) throw Error('disconnect_send_failed');
  await wait(() => aiControlState?.state === 'disconnected' && closed() && responseId === '', 5000);
  const stopped = {actualCaptureClosed: true, actualOutputStopped: true,
    originalSessionPreserved: aiControlState.originalSessionPreserved, endpointEpoch: aiControlState.endpointEpoch};
  if (variant === 'expired') {
    await wait(() => terminated, 20000);
    return {...stopped, expired: true, reconnectAfterExpiryRejected: send({kind: 'controlReconnect'}) === false,
      elapsed: performance.now() - started};
  }
  if (variant === 'stopped') {
    if (element('stop').disabled) throw Error('offline_stop_disabled');
    element('stop').click(); // 真实用户停止，不能直接改HAL或核心采集意图。
  }
  if (!send({kind: 'controlReconnect'})) throw Error('reconnect_send_failed');
  await wait(() => aiControlState?.state === 'ready' && aiControlState.endpointEpoch === epoch + 1, 14000);
  await wait(() => variant === 'active' ? !closed() : closed(), 14000);
  return {...stopped, expired: false, elapsed: performance.now() - started};
}

// verifyRecovery 核原业务scope、公开新代次、真实驱动和旧输入隔离，握手成功不能代替媒体实态。
function verifyRecovery(before, stopped, after, transport, variant) {
  assert.equal(before.control.state, 'ready');
  assert.equal(before.control.originalSessionPreserved, true);
  assert.equal(before.captureOpen, true);
  assert.equal(stopped.actualCaptureClosed, true);
  assert.equal(stopped.actualOutputStopped, true);
  assert.equal(stopped.originalSessionPreserved, true);
  assert.equal(stopped.endpointEpoch, before.control.endpointEpoch);
  assert.ok(Number.isFinite(stopped.elapsed) && stopped.elapsed >= 0 && stopped.elapsed < 15000);
  assert.equal(after.control.state, 'ready');
  assert.equal(after.control.endpointEpoch, before.control.endpointEpoch + 1);
  assert.ok(Number.isSafeInteger(after.control.messageWatermark) && after.control.messageWatermark >= before.control.messageWatermark);
  assert.equal(after.control.originalSessionPreserved, true);
  assert.equal(after.terminated, false);
  assert.equal(after.contextRunning, true);
  assert.equal(after.outputStopped, true);
  assert.equal(after.configurations, before.configurations);
  assert.equal(after.recognized, before.recognized);
  assert.equal(after.completed, before.completed);
  if (transport === 'webrtc') { assert.equal(after.peerSame, true); assert.equal(after.peer, 'connected'); }
  assert.equal(after.captureOpen, variant === 'active');
  assert.equal(after.starts, before.starts + (variant === 'active' ? 1 : 0));
  assert.equal(after.freshInput, variant === 'active');
  return {originalSessionPreserved: true, endpointAdvancedOnce: true, originalPeerPreserved: transport === 'webrtc',
    actualCaptureStopped: true, oldInputNotReplayed: true, userStopPreserved: variant === 'stopped',
    recoveryMilliseconds: stopped.elapsed};
}

// verifyAudio 保留原回复识别、关联和有声消费门；UDP另要求同回复媒体终态、零丢包和全量解码，尾部有声不能冒充完整音频。
function verifyAudio(before, after, transport) {
  for (const key of ['completed', 'voiced', 'network', 'received', 'recognized'])
    assert.ok(Number.isSafeInteger(after[key]) && after[key] >= 0);
  assert.equal(after.completed, before.completed + 1);
  assert.equal(after.recognized, before.recognized + 1);
  assert.equal(after.recognizedSameReply, true);
  assert.equal(after.keywordMatched, true);
  assert.ok(typeof after.completedID === 'string' && after.completedID && after.completedID !== before.completedID);
  assert.equal(after.playableID, after.completedID);
  assert.ok(after.voiced >= before.voiced + 2);
  assert.equal(after.outputStopped, true);
  if (transport === 'webrtc') assert.ok(after.received > before.received);
  else {
    assert.ok(after.network > 0);
    assert.equal(after.completedReplyMediaFinal, true, 'original_media_final_required');
    assert.ok(Number.isSafeInteger(after.missingPackets) && after.missingPackets === 0,
      'original_reply_loss_forbidden');
    assert.equal(after.observedComplete, true, 'original_reply_incomplete');
    assert.ok(Number.isSafeInteger(after.decoded) && after.decoded >= 0 && after.decoded === after.network,
      'original_reply_decode_incomplete');
  }
  return {newReplyCompleted: true, originalReplyPlayable: true,
    actualVoicedFrames: transport === 'udp' ? after.voiced - before.voiced : null,
    actualVoicedWindows: transport === 'webrtc' ? after.voiced - before.voiced : null,
    latency: verifyReplyTiming(after, transport)};
}

// run 生命周期最终PASS必须等实际进程close、stderr排空和浏览器关闭；临时注入只用于离线失败回归。
async function run(env = process.env, deps = {}) {
  const settings = validateEnvironment(env), port = String(settings.port);
  const browserSession = 'device-ai-control-' + randomUUID();
  const child = (deps.spawn || spawn)(env.UR_DEVICE_BROWSER_BIN, [port], {env, stdio: ['ignore', 'pipe', 'pipe']});
  const observer = createDeviceFailureObserver();
  let exited = false, exitCode = null, stderrEnded = false, phase = 'device_start', success = false, diagnostic = null;
  let subphase = 'process_start', browserOperation = null, primary = null, observedFacts = null;
  const rounds = []; // 各轮独立保留，后续失败不能抹掉前轮或用前轮冒充恢复后成功。
  let recovery = null, audio = null;
  const completion = new Promise(resolve => child.once('close', code => { exited = true; exitCode = code; resolve(); }));
  child.stderr.on('data', bytes => observer.push(bytes));
  child.stderr.once('end', () => { stderrEnded = true; observer.finish(); });
  const invoke = (...args) => new Promise((resolve, reject) => {
    browserOperation = ['open', 'eval', 'wait', 'click', 'close'].includes(args[0]) ? args[0] : null;
    (deps.execFile || execFile)('agent-browser', ['--session', browserSession, ...args],
    {env: {...env, AGENT_BROWSER_ARGS: [env.AGENT_BROWSER_ARGS || '--no-sandbox', '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream'].join(',')},
      timeout: args[0] === 'wait' ? 65000 : 25000, maxBuffer: 65536},
    (error, stdout) => error ? reject(Error('browser_command_failed')) : resolve(stdout.trim()));
  });
  const evaluate = async expression => parseJSON(await invoke('eval', expression));
  const facts = () => evaluate('(' + readFacts.toString() + ')()');
  const deadline = setTimeout(() => { try { child.kill('SIGTERM'); } catch {} }, 240000);
  try {
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(Error('device_start_timeout')), 10000);
      child.once('error', () => { clearTimeout(timer); reject(Error('device_spawn_failed')); });
      child.once('close', () => { clearTimeout(timer); reject(Error('device_start_failed')); });
      child.stdout.on('data', bytes => { if (bytes.toString().includes('http://127.0.0.1:' + port + '/')) { clearTimeout(timer); resolve(); } });
    });
    phase = 'session_ready';
    subphase = 'browser_open';
    await invoke('open', 'http://127.0.0.1:' + port + '/' + (settings.transport === 'webrtc' ? '?media=webrtc' : ''));
    await invoke('eval', '(' + installObserver.toString() + ')()');
    await invoke('eval', 'globalThis.__urAIControl.keywords=' + JSON.stringify(settings.keywords));
    await invoke('click', '#activate');
    subphase = 'session_established';
    await invoke('wait', '--fn', 'aiControlState?.state === "ready" && !element("start").disabled' + (settings.transport === 'webrtc' ? ' && rtcPeer?.connectionState === "connected"' : ''));
    phase = 'bounded_fixture';
    subphase = 'fixture_install';
    await invoke('eval', 'globalThis.__urFixtureChunks=[]');
    const encoded = settings.bytes.toString('base64');
    for (let offset = 0; offset < encoded.length; offset += 24000)
      await invoke('eval', 'globalThis.__urFixtureChunks.push(' + JSON.stringify(encoded.slice(offset, offset + 24000)) + ')');
    await invoke('eval', '(' + installFixture.toString() + ')()');
    // audioRound 每轮只提交一个原新输入，固定语料通过真实Worklet/Sender，不直接送Provider。
    const audioRound = async (alreadyCapturing, round) => {
      subphase = 'round_baseline';
      const before = await facts();
      subphase = 'capture_start';
      if (!alreadyCapturing) await invoke('click', '#start');
      subphase = 'capture_ready';
      await invoke('wait', '--fn', settings.transport === 'webrtc' ? 'rtcPeer.getSenders().some(item=>item.track?.kind==="audio" && item.track.enabled)' : 'metrics.captureOpen === true');
      subphase = 'fixture_start';
      await invoke('eval', 'globalThis.__urControlPlay()');
      subphase = 'fixture_end';
      await invoke('wait', '--fn', 'globalThis.__urAIControl.fixtureEnded === ' + (before.fixtureEnded + 1));
      subphase = 'input_stop';
      await invoke('click', '#stop');
      subphase = 'reply_complete';
      await invoke('wait', '--fn', 'metrics.completedResponses === ' + (before.completed + 1) + ' && responseId === "" && completedResponseId !== "" && metrics.lastPlayableConsumedResponseId === completedResponseId');
      subphase = 'reply_verify';
      const verified = verifyAudio(before, await facts(), settings.transport);
      rounds.push({round, ...verified});
      return verified;
    };
    phase = 'baseline_audio'; await audioRound(false, 'baseline');
    subphase = 'capture_before_loss';
    phase = 'capture_before_loss'; await invoke('click', '#start');
    await invoke('wait', '--fn', settings.transport === 'webrtc' ? 'rtcPeer.getSenders().some(item=>item.track?.kind==="audio" && item.track.enabled)' : 'metrics.captureOpen === true');
    const before = await facts();
    phase = 'physical_control_window';
    subphase = 'control_window';
    const stopped = await evaluate('(' + controlWindow.toString() + ')(' + JSON.stringify(settings.variant) + ')');
    if (settings.variant === 'expired') {
      assert.equal(stopped.expired, true); assert.equal(stopped.originalSessionPreserved, true);
      assert.equal(stopped.reconnectAfterExpiryRejected, true); assert.ok(stopped.elapsed >= 14900 && stopped.elapsed <= 20000);
      assert.equal(await waitClosed(completion, deps.closeTimeoutMs || 10000), true);
      assert.equal(exitCode, 1);
      recovery = {expiredFailClosed: true, lateReconnectRejected: true, actualCaptureStopped: true};
    } else {
      phase = 'authoritative_restoration';
      subphase = 'restoration_verify';
      recovery = verifyRecovery(before, stopped, await facts(), settings.transport, settings.variant);
      phase = 'restored_audio'; audio = await audioRound(settings.variant === 'active', 'restored');
      subphase = 'release';
      phase = 'release'; await invoke('click', '#end');
      assert.equal(await waitClosed(completion, deps.closeTimeoutMs || 10000), true); assert.equal(exitCode, 0);
    }
    success = true;
  } catch (error) {
    primary = {phase, subphase, reason: failureReason(error), browserOperation};
    diagnostic = await evaluate('(' + readStartupFacts.toString() + ')()').catch(() => null);
    observedFacts = await facts().then(failureFacts).catch(() => null);
  } finally {
    clearTimeout(deadline);
    const browserClosed = await invoke('close').then(() => true, () => false);
    if (!exited) {
      try { child.kill('SIGTERM'); } catch {}
      if (!await waitClosed(completion, deps.closeTimeoutMs || 8000) && !exited) {
        try { child.kill('SIGKILL'); } catch {}
        await waitClosed(completion, deps.closeTimeoutMs || 8000);
      }
    }
    const cleanup = {browserClosed, childClosed: exited, stderrEnded};
    const passed = success && browserClosed && exited && stderrEnded;
    const report = {result: passed ? 'pass' : 'fail', phase: passed ? 'completed' : phase,
      configuredExecutionChain: settings.chain, transport: settings.transport, variant: settings.variant,
      binarySHA: createHash('sha256').update(fs.readFileSync(env.UR_DEVICE_BROWSER_BIN)).digest('hex'),
      corpusSHA: createHash('sha256').update(settings.bytes).digest('hex'), recovery, audio, rounds, cleanup,
      ...(!passed ? {diagnostic: {primary: primary || {phase, subphase: 'cleanup', reason: 'cleanup_failed', browserOperation: 'close'},
        facts: observedFacts, ui: diagnostic, deviceFailure: stderrEnded ? observer.finish() : null, deviceExitCode: exitCode}} : {})};
    (deps.report || console.log)(JSON.stringify(report));
    if (!passed) throw Error('ai_control_live_failed');
    return report;
  }
}
module.exports = {validateEnvironment, installObserver, installFixture, readFacts, controlWindow, verifyRecovery,
  verifyReplyTiming, failureFacts, failureReason, verifyAudio, run};
if (require.main === module) run().catch(() => { console.error('ai_control_live_failed'); process.exitCode = 1; });

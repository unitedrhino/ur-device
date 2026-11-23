// 文件说明：真实控制恢复驱动的判据、脱敏和生命周期单测；合成端口仅验证测试器，不代表模型或设备通过。
'use strict';
const test = require('node:test'), assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const {EventEmitter} = require('node:events');
const live = require('./browser_ai_control_live.cjs');
// environment 每例在仓库.temp建立唯一有界WAV替身，调用者finally回收，不触碰真实语料或凭据。
function environment(variant = 'active', transport = 'udp') {
  const parent = path.join(process.cwd(), '.temp'); fs.mkdirSync(parent, {recursive: true});
  const directory = fs.mkdtempSync(path.join(parent, 'ai-control-unit-'));
  const wave = path.join(directory, 'fixture.wav'), bytes = Buffer.alloc(48); bytes.write('RIFF'); fs.writeFileSync(wave, bytes);
  return {directory, env: {UR_AI_CONTROL_LIVE: '1', UR_DEVICE_CONTROL_SYNC: '1', UR_AI_CONTROL_CHAIN: 'cascaded_agentruntime',
    UR_AI_CONTROL_VARIANT: variant, UR_MEDIA_TRANSPORT: transport, UR_BROWSER_PORT: '39881',
    UR_DEVICE_BROWSER_BIN: __filename, UR_BROWSER_INPUT_WAV: wave,
    UR_BROWSER_EXPECT_ASR_KEYWORDS: '合成语料',
    UR_DEVICE_PRODUCT: 'synthetic-product', UR_DEVICE_NAME: 'synthetic-device',
    UR_DEVICE_SECRET: 'synthetic-private-secret', UR_MQTT_HOST: '127.0.0.1', UR_MQTT_PORT: '12345'}};
}
// before 描述真实驱动判据所需全部事实形状，身份仅合成且不进入最终报告。
function before() { return {control: {state: 'ready', endpointEpoch: 1, messageWatermark: 4, originalSessionPreserved: true},
  captureOpen: true, configurations: 1, starts: 2, stops: 1, fixtureEnded: 1,
  contextRunning: true, terminated: false, outputStopped: true, peerSame: true, peer: 'connected',
  freshInput: false, completed: 1, completedID: 'synthetic-original-reply', playableID: 'synthetic-original-reply',
  voiced: 4, captured: 20, network: 10, sent: 20, received: 20,
  recognized: 1, recognizedSameReply: true, keywordMatched: true}; }
// restored 只有公开代次推进及实际采集策略符合时才能通过，Peer沿原对象而非只检查connected。
function restored(variant = 'active') { const value = before(); return {...value,
  control: {...value.control, endpointEpoch: 2, messageWatermark: 9},
  captureOpen: variant === 'active', starts: variant === 'active' ? 3 : 2, freshInput: variant === 'active'}; }
// stopped 是断网窗口内已经观察到的原驱动停止事实，不取恢复后状态反推。
function stopped() { return {actualCaptureClosed: true, actualOutputStopped: true,
  originalSessionPreserved: true, endpointEpoch: 1, elapsed: 800}; }
test('仅显式真实安全子集，UDP与WebRTC、串联与原生均可；外围能力不暗降级', () => {
  const fixture = environment();
  try {
    for (const transport of ['udp', 'webrtc']) for (const chain of ['cascaded_agentruntime', 'realtime_native'])
      assert.equal(live.validateEnvironment({...fixture.env, UR_MEDIA_TRANSPORT: transport, UR_AI_CONTROL_CHAIN: chain}).transport, transport);
    for (const change of [{UR_AI_CONTROL_LIVE: '0'}, {UR_DEVICE_CONTROL_SYNC: ''}, {UR_MEDIA_TRANSPORT: 'none'},
      {UR_AI_CONTROL_CHAIN: 'unknown'}, {UR_AI_CONTROL_VARIANT: 'retry'}, {UR_OUTPUT_FOCUS: 'voice'},
      {UR_AI_VISION_CAMERA: 'camera'}, {UR_DEVICE_SECRET: ''}, {UR_BROWSER_PORT: '0'}])
      assert.throws(() => live.validateEnvironment({...fixture.env, ...change}));
  } finally { fs.rmSync(fixture.directory, {recursive: true}); }
});
test('标准16k单声道16bit的7.96秒语料与10秒上界可验收，超字节预算仍拒绝', () => {
  const fixture = environment();
  try {
    for (const size of [254764, 320044]) {
      // 合成与已批准实际语料相同长度的合法PCM WAV，不提交真实语音、身份或供应商凭据。
      const bytes = Buffer.alloc(size); bytes.write('RIFF'); bytes.writeUInt32LE(size - 8, 4);
      bytes.write('WAVE', 8); bytes.write('fmt ', 12); bytes.writeUInt32LE(16, 16);
      bytes.writeUInt16LE(1, 20); bytes.writeUInt16LE(1, 22); bytes.writeUInt32LE(16000, 24);
      bytes.writeUInt32LE(32000, 28); bytes.writeUInt16LE(2, 32); bytes.writeUInt16LE(16, 34);
      bytes.write('data', 36); bytes.writeUInt32LE(size - 44, 40);
      fs.writeFileSync(fixture.env.UR_BROWSER_INPUT_WAV, bytes);
      assert.equal(live.validateEnvironment(fixture.env).bytes.length, size);
    }
    fs.appendFileSync(fixture.env.UR_BROWSER_INPUT_WAV, Buffer.alloc(1));
    assert.throws(() => live.validateEnvironment(fixture.env));
  } finally { fs.rmSync(fixture.directory, {recursive: true}); }
});
test('原会话、epoch+1、Peer、旧输入隔离与用户停采集分别有硬判据', () => {
  for (const variant of ['active', 'stopped']) {
    assert.equal(live.verifyRecovery(before(), stopped(), restored(variant), 'webrtc', variant).oldInputNotReplayed, true);
    for (const change of [value => value.control.endpointEpoch = 1, value => value.control.endpointEpoch = 3,
      value => value.control.messageWatermark = 3, value => value.control.originalSessionPreserved = false,
      value => value.contextRunning = false, value => value.terminated = true, value => value.outputStopped = false,
      value => value.configurations = 2, value => value.peerSame = false, value => value.peer = 'disconnected',
      value => value.captureOpen = !value.captureOpen, value => value.starts++, value => value.freshInput = !value.freshInput,
      value => value.recognized++, value => value.completed++]) {
      const value = restored(variant); change(value);
      assert.throws(() => live.verifyRecovery(before(), stopped(), value, 'webrtc', variant));
    }
  }
  for (const change of [{actualCaptureClosed: false}, {actualOutputStopped: false},
    {originalSessionPreserved: false}, {elapsed: 15000}, {elapsed: Infinity}, {endpointEpoch: 2}])
    assert.throws(() => live.verifyRecovery(before(), {...stopped(), ...change}, restored(), 'udp', 'active'));
});
test('原新回复关联、连续两个有声消费帧和真实网络媒体不能互相替代', () => {
  const b = before(), a = {...b, completed: 2, completedID: 'synthetic-new-reply', playableID: 'synthetic-new-reply', voiced: 6, received: 30, recognized: 2};
  assert.equal(live.verifyAudio(b, a, 'webrtc').newReplyCompleted, true);
  for (const change of [{completed: 1}, {completedID: b.completedID}, {playableID: b.playableID},
    {voiced: 5}, {voiced: Infinity}, {received: b.received}, {received: '30'}, {outputStopped: false},
    {recognized: 1}, {recognizedSameReply: false}, {keywordMatched: false}])
    assert.throws(() => live.verifyAudio(b, {...a, ...change}, 'webrtc'));
  assert.throws(() => live.verifyAudio(b, {...a, network: 0}, 'udp'));
});
// completedAudioFacts 用原只读投影关联当前回复和媒体终态；合成计数不代表真实设备或模型验收。
async function completedAudioFacts({finalID = 'synthetic-new-reply', missing = 0,
  complete = true, network = 180, decoded = 180} = {}) {
  const context = vm.createContext({useWebRTC: false, rtcPeer: null, rtcPeerRevision: '1',
    aiControlState: {}, responseId: '', completedResponseId: 'synthetic-new-reply',
    currentReplyConsumed: 0, currentReplyConsecutiveConsumedNonSilent: 0,
    pendingPlayback: new Map(), context: {state: 'running'}, terminated: false,
    __urControlPlay() {}, __urAIControl: {starts: 1, stops: 1, configurations: 1,
      fixtureEnded: 1, recognized: 2, recognizedID: 'synthetic-new-reply',
      recognizedText: '合成语料', keywords: ['合成语料']},
    metrics: {captureOpen: false, completedResponses: 2,
      lastPlayableConsumedResponseId: 'synthetic-new-reply', consumedNonSilent: 9,
      captured: 518, lastNetworkPackets: network, lastDecodedFrames: decoded,
      lastNonSilentFrames: 5, consumed: 36, lastMissingPackets: missing,
      lastObservedComplete: complete, lastAudioState: 'sent',
      lastMediaFinalResponseId: finalID, lastReplyTiming: null}});
  return vm.runInContext('(' + live.readFacts.toString() + ')()', context);
}
test('UDP完整回复沿原媒体终态、零丢包与全量解码通过，不改变逐轮结果形状', async () => {
  const result = live.verifyAudio(before(), await completedAudioFacts(), 'udp');
  assert.equal(result.newReplyCompleted, true);
  assert.equal(result.actualVoicedFrames, 5);
  assert.equal(result.originalReplyPlayable, true);
  assert.equal(result.latency.primaryMeasured, false);
});
for (const [name, change] of [
  ['尾部有声但前部丢包', {missing: 164, complete: false, network: 16, decoded: 16}],
  ['缺少媒体终态', {finalID: ''}],
  ['旧回复媒体终态', {finalID: 'synthetic-original-reply'}],
  ['零丢包但未观察完整', {complete: false}],
  ['部分网络包未解码', {decoded: 179}],
  ['负丢包数', {missing: -1}],
  ['丢包数超安全整数', {missing: Number.MAX_SAFE_INTEGER + 1}],
  ['非整数解码数', {decoded: 179.5}],
  ['空丢包数', {missing: null}],
  ['文本丢包数', {missing: '0'}],
  ['非布尔完整事实', {complete: 1}],
  ['解码数超安全整数', {decoded: Number.MAX_SAFE_INTEGER + 1}],
  ['负解码数', {decoded: -1}],
  ['解码数超过网络包数', {decoded: 181}],
]) test('UDP完整性拒绝：' + name, async () => {
  const facts = await completedAudioFacts(change);
  assert.throws(() => live.verifyAudio(before(), facts, 'udp'));
});
test('严格UDP完整性仍不能替代原回复身份、识别关键词或连续有声门', async () => {
  const facts = await completedAudioFacts();
  for (const change of [{completedID: before().completedID}, {recognizedSameReply: false},
    {keywordMatched: false}, {voiced: before().voiced + 1}])
    assert.throws(() => live.verifyAudio(before(), {...facts, ...change}, 'udp'));
  for (const key of ['completedReplyMediaFinal', 'missingPackets', 'observedComplete', 'decoded']) {
    const omitted = {...facts}; delete omitted[key];
    assert.throws(() => live.verifyAudio(before(), omitted, 'udp'));
  }
});
test('WebRTC不冒认UDP终态和丢包账本，仍须原实际媒体增量', async () => {
  const facts = await completedAudioFacts({finalID: '', missing: 164, complete: false, decoded: 0});
  assert.equal(live.verifyAudio(before(), {...facts, received: 30}, 'webrtc').newReplyCompleted, true);
  assert.throws(() => live.verifyAudio(before(), {...facts, received: before().received}, 'webrtc'));
});
test('逐轮延迟只相减原C++时刻，缺失与实时重叠保留为不可测', () => {
  const facts = {replyTiming: {lastVoiceSentMs: 100, firstTextMs: 110,
    firstNetworkMs: 120, firstPlayableMs: 140}};
  const measured = live.verifyReplyTiming(facts, 'udp');
  assert.equal(measured.firstTextMilliseconds, 10);
  assert.equal(measured.firstNetworkAudioMilliseconds, 20);
  assert.equal(measured.firstPlayableAudioMilliseconds, 40);
  assert.equal(measured.primaryMeasured, true);
  assert.equal(measured.physicalSpeakerOnsetMeasured, false);
  assert.deepEqual(measured.unavailable, {});
  const overlap = live.verifyReplyTiming({replyTiming: {...facts.replyTiming,
    firstTextMs: 90, firstNetworkMs: 95, firstPlayableMs: 100}}, 'udp');
  assert.equal(overlap.firstTextMilliseconds, null);
  assert.equal(overlap.firstNetworkAudioMilliseconds, null);
  assert.equal(overlap.firstPlayableAudioMilliseconds, 0);
  assert.equal(overlap.unavailable.firstTextMilliseconds, 'overlapped_response');
  assert.equal(overlap.unavailable.firstNetworkAudioMilliseconds, 'overlapped_response');
  assert.equal(live.verifyReplyTiming({}, 'udp').primaryMeasured, false);
  assert.equal(live.verifyReplyTiming(facts, 'webrtc').unavailable.firstPlayableAudioMilliseconds,
    'webrtc_native_timestamps_unavailable');
  assert.equal(live.verifyReplyTiming({replyTiming: {...facts.replyTiming, lastVoiceSentMs: null}},
    'udp').unavailable.firstPlayableAudioMilliseconds, 'last_valid_voice_sent_unavailable');
  assert.equal(live.verifyReplyTiming({replyTiming: {lastVoiceSentMs: 0, firstTextMs: 0,
    firstNetworkMs: 0, firstPlayableMs: 0}}, 'udp').firstPlayableAudioMilliseconds, 0);
});
test('时间精度、缺键与不可能先解码后收包不能通过测量，不能做负uint差值', () => {
  const timing = {lastVoiceSentMs: 100, firstTextMs: 110, firstNetworkMs: 120, firstPlayableMs: 140};
  for (const change of [{firstPlayableMs: 119}, {firstNetworkMs: null}, {firstTextMs: -1},
    {firstTextMs: '110'}, {firstTextMs: Infinity}, {firstTextMs: 9007199254740992}, {extra: 1}])
    assert.throws(() => live.verifyReplyTiming({replyTiming: {...timing, ...change}}, 'udp'));
  const missing = {...timing}; delete missing.firstTextMs;
  assert.throws(() => live.verifyReplyTiming({replyTiming: missing}, 'udp'));
});
test('失败事实只留固定计数和状态，任意身份/正文/无效数值均不进入报告', () => {
  const projected = live.failureFacts({...before(), completedID: 'private-reply',
    recognizedText: 'private-body', captured: -1, voiced: Infinity,
    control: {...before().control, sessionId: 'private-session', reason: 'private-error'},
    replyTiming: {responseId: 'private-reply'}});
  assert.equal(projected.recognized, 1);
  assert.equal(projected.network, 10);
  assert.equal(projected.control.endpointEpoch, 1);
  assert.equal(Object.hasOwn(projected, 'captured'), false);
  assert.equal(Object.hasOwn(projected, 'voiced'), false);
  assert.ok(!JSON.stringify(projected).includes('private'));
  assert.equal(live.failureReason(Error('private-provider-error')), 'scenario_failed');
  assert.equal(live.failureReason({code: 'ERR_ASSERTION', message: 'private'}), 'criterion_failed');
});
test('失败音频分层只保留已有解码消费计数与完成关联，不把网络到达当有声', () => {
  const audio = {decoded: 16, decodedNonSilent: 0, consumed: 16, consumedNonSilent: 0,
    currentReplyConsumed: 16, consecutiveConsumedNonSilent: 0, pendingPlaybackFrames: 0,
    missingPackets: 0, observedComplete: true, lastAudioState: 'sent',
    completedReplyPlayable: false, completedReplyMediaFinal: true};
  assert.deepEqual(live.failureFacts(audio), audio);
  for (const key of ['decoded', 'decodedNonSilent', 'consumed', 'consumedNonSilent',
    'currentReplyConsumed', 'consecutiveConsumedNonSilent', 'pendingPlaybackFrames', 'missingPackets']) {
    for (const value of [-1, Infinity, '16', Number.MAX_SAFE_INTEGER + 1])
      assert.equal(Object.hasOwn(live.failureFacts({...audio, [key]: value}), key), false);
  }
  for (const key of ['observedComplete', 'completedReplyPlayable', 'completedReplyMediaFinal'])
    assert.equal(Object.hasOwn(live.failureFacts({...audio, [key]: 'true'}), key), false);
  assert.equal(Object.hasOwn(live.failureFacts({...audio, lastAudioState: 'private-error'}), 'lastAudioState'), false);
  for (const value of ['none', 'sent', 'cancelled', 'failed'])
    assert.equal(live.failureFacts({lastAudioState: value}).lastAudioState, value);
});
test('readFacts读取原UDP媒体和消费账本一次，不推进解码或伪造连续有声', async () => {
  const metrics = {completedResponses: 1, captured: 424, lastNetworkPackets: 16,
    lastDecodedFrames: 16, lastNonSilentFrames: 0, consumed: 16, consumedNonSilent: 0,
    lastMissingPackets: 0, lastObservedComplete: true, lastAudioState: 'sent',
    lastMediaFinalResponseId: 'private-reply', lastPlayableConsumedResponseId: '', captureOpen: false};
  const pending = new Map([[1, false]]);
  const context = vm.createContext({metrics, useWebRTC: false, aiControlState: null,
    responseId: '', completedResponseId: 'private-reply', currentReplyConsumed: 16,
    currentReplyConsecutiveConsumedNonSilent: 0, pendingPlayback: pending,
    terminated: false, context: {state: 'running'}});
  context.__urAIControl = {starts: 1, stops: 1, configurations: 0, fixtureEnded: 1,
    recognized: 1, recognizedID: 'private-reply', recognizedText: '', keywords: []};
  const original = JSON.stringify(metrics);
  const facts = await vm.runInContext('(' + live.readFacts.toString() + ')()', context);
  assert.equal(facts.decoded, 16); assert.equal(facts.decodedNonSilent, 0);
  assert.equal(facts.pendingPlaybackFrames, 1); assert.equal(facts.completedReplyPlayable, false);
  assert.equal(facts.completedReplyMediaFinal, true); assert.equal(facts.lastAudioState, 'sent');
  assert.equal(JSON.stringify(metrics), original); assert.equal(pending.size, 1);
  assert.ok(!JSON.stringify(live.failureFacts(facts)).includes('private'));
});
test('observer调用原incoming一次，只观察已接纳命令，不伪造SIG或开麦', () => {
  const values = [], context = vm.createContext({incoming: value => {values.push(value); return 'original';}});
  vm.runInContext('(' + live.installObserver.toString() + ')()', context);
  assert.equal(vm.runInContext('incoming({kind:"captureStart",id:"private-input"})', context), 'original');
  vm.runInContext('incoming({kind:"captureStop",id:"private-input"});incoming({kind:"webrtcConfigure"})', context);
  assert.equal(values.length, 3);
  const facts = JSON.parse(vm.runInContext('JSON.stringify(__urAIControl)', context));
  assert.equal(facts.starts, 1); assert.equal(facts.stops, 1); assert.equal(facts.configurations, 1);
  assert.throws(() => vm.runInContext('(' + live.installObserver.toString() + ')()', context));
});
// fixtureSandbox仅核测试夹具连接合同，不伪造原Worklet线程事实；真实处理块另留独立浏览器证据。
function fixtureSandbox(decoded = {numberOfChannels: 1, duration: 0.25}) {
  const calls = [], buffers = [], target = {};
  const bed = {offset: {value: 1}, connect(value) { assert.equal(value, target); calls.push('bed-connect'); },
    start() { calls.push('bed-start'); }, stop() { calls.push('bed-stop'); }};
  const context = vm.createContext({atob, Uint8Array, useWebRTC: false, worklet: target,
    source: {disconnect() { calls.push('source-disconnect'); }},
    __urFixtureChunks: [Buffer.from([1, 2, 3]).toString('base64')], __urAIControl: {fixtureEnded: 0},
    context: {currentTime: 1, async decodeAudioData() { return decoded; },
      createConstantSource() { calls.push('bed-create'); return bed; },
      createBufferSource() { const node = {connect(value) { assert.equal(value, target); calls.push('buffer-connect'); },
        disconnect() { calls.push('buffer-disconnect'); }, start(time) { assert.equal(time, 1.05); calls.push('buffer-start'); }};
        buffers.push(node); return node; }}});
  return {context, calls, bed, buffers};
}
test('UDP语料夹具断开假麦后维持唯一零值输入床，不借空输入确认所有权', async () => {
  const probe = fixtureSandbox();
  await vm.runInContext('(' + live.installFixture.toString() + ')()', probe.context);
  assert.deepEqual(probe.calls, ['source-disconnect', 'bed-create', 'bed-connect', 'bed-start']);
  assert.equal(probe.bed.offset.value, 0);
  assert.equal(probe.context.__urAIControl.silentBed, probe.bed);
});
test('UDP两轮语料分别停止BufferSource，零值床不重复创建或被单轮结束断开', async () => {
  const probe = fixtureSandbox();
  await vm.runInContext('(' + live.installFixture.toString() + ')()', probe.context);
  for (let index = 0; index < 2; ++index) {
    vm.runInContext('__urControlPlay()', probe.context); probe.buffers[index].onended();
  }
  assert.equal(probe.context.__urAIControl.fixtureEnded, 2);
  for (const name of ['bed-create', 'bed-connect', 'bed-start'])
    assert.equal(probe.calls.filter(value => value === name).length, 1);
  assert.equal(probe.calls.includes('bed-stop'), false);
  assert.equal(probe.calls.filter(value => value === 'buffer-disconnect').length, 2);
});
test('非法语料在任何源断开或零值床副作用前拒绝', async () => {
  for (const decoded of [{numberOfChannels: 2, duration: 1}, {numberOfChannels: 1, duration: 0},
    {numberOfChannels: 1, duration: 10.01}]) {
    const probe = fixtureSandbox(decoded);
    await assert.rejects(vm.runInContext('(' + live.installFixture.toString() + ')()', probe.context));
    assert.deepEqual(probe.calls, []);
  }
});
// harness 驱动原run流程及精确清理；只在离线tests注入，环境不能选择端口实现。
function harness(variant, options = {}) {
  const child = new EventEmitter(); child.stdout = new EventEmitter(); child.stderr = new EventEmitter();
  const commands = [], reports = [], signals = [];
  let starts = 0, stops = 0, completed = 0, fixtureEnded = 0, captureOpen = false, resumed = false;
  let replyWaitFailed = false; // 只为测试器首因反例注入失败，不进入真实环境开关。
  const terminate = code => setImmediate(() => { if (!options.noStderr) child.stderr.emit('end'); child.emit('close', code); });
  child.kill = signal => { signals.push(signal); if (!options.neverClose) terminate(1); return true; };
  return {commands, reports, signals, closeTimeoutMs: 3,
    spawn(binary, args, configuration) {
      assert.equal(configuration.env.UR_DEVICE_CONTROL_SYNC, '1'); assert.deepEqual(args, ['39881']);
      setImmediate(() => child.stdout.emit('data', Buffer.from('http://127.0.0.1:39881/\n'))); return child;
    },
    execFile(binary, args, configuration, callback) {
      commands.push(args); const operation = args[2], expression = operation === 'wait' ? args[4] : args[3]; let output = 'true';
      if (operation === 'wait' && expression.includes('metrics.completedResponses') && options.replyWaitError) {
        replyWaitFailed = true; setImmediate(() => callback(Error('synthetic-private-secret'))); return;
      }
      if (operation === 'eval' && replyWaitFailed &&
          ((options.diagnosticFactsError && expression.includes('async function readFacts')) ||
           (options.diagnosticStartupError && expression.includes('function readStartupFacts')))) {
        setImmediate(() => callback(Error('synthetic-private-secret'))); return;
      }
      if (operation === 'click' && expression === '#start') { ++starts; captureOpen = true; }
      if (operation === 'eval' && expression === 'globalThis.__urControlPlay()') ++fixtureEnded;
      if (operation === 'click' && expression === '#stop') { ++stops; captureOpen = false; if (fixtureEnded > completed) completed = fixtureEnded; }
      if (operation === 'eval' && expression.includes('async function controlWindow')) {
        captureOpen = variant === 'active'; resumed = variant !== 'expired'; if (captureOpen) ++starts;
        output = JSON.stringify({...stopped(), expired: variant === 'expired', reconnectAfterExpiryRejected: true, elapsed: variant === 'expired' ? 15001 : 800});
        if (variant === 'expired') terminate(1);
      }
      if (operation === 'eval' && expression.includes('async function readFacts')) {
        output = JSON.stringify({...before(), control: {state: 'ready', endpointEpoch: resumed ? 2 : 1,
          messageWatermark: resumed ? 9 : 4, originalSessionPreserved: true}, starts, stops, configurations: 1,
          captureOpen, freshInput: resumed && (variant === 'active' || starts >= 3), fixtureEnded,
          completed, completedID: completed ? 'synthetic-reply-' + completed : '', playableID: completed ? 'synthetic-reply-' + completed : '',
          voiced: completed * 4, received: completed * 10, network: completed * 10, recognized: completed,
          // 合成成功轮显式提供完整媒体账本；缺失事实不能被验收器默认补成通过。
          decoded: completed * 10, missingPackets: 0,
          observedComplete: completed > 0, completedReplyMediaFinal: completed > 0,
          replyTiming: completed ? {lastVoiceSentMs: completed * 100,
            firstTextMs: completed * 100 + 10, firstNetworkMs: completed * 100 + 20,
            firstPlayableMs: completed * 100 + 40} : null,
          ...(options.badScope && resumed ? {control: {state: 'ready', endpointEpoch: 2, messageWatermark: 9, originalSessionPreserved: false}} : {})});
      }
      if (operation === 'eval' && expression.includes('function readStartupFacts')) output = JSON.stringify({contextState: 'closed', closureReason: 'device_link_error'});
      if (operation === 'click' && expression === '#end' && !options.neverClose) terminate(0);
      setImmediate(() => callback(options.closeError && operation === 'close' ? Error('synthetic-private-secret') : null, output));
    }, report(value) { reports.push(JSON.parse(value)); }};
}
test('active/stopped完整正常生命周期只在原进程close和stderr排空后PASS，报告不含身份', async () => {
  for (const variant of ['active', 'stopped']) {
    const fixture = environment(variant), h = harness(variant);
    try { const result = await live.run(fixture.env, h);
      assert.equal(result.result, 'pass'); assert.equal(result.recovery.userStopPreserved, variant === 'stopped');
      assert.equal(result.audio.actualVoicedFrames, 4);
      assert.deepEqual(result.rounds.map(round => round.round), ['baseline', 'restored']);
      assert.equal(result.rounds.every(round => round.latency.firstPlayableAudioMilliseconds === 40), true);
      assert.equal(result.rounds.every(round => round.latency.primaryMeasured), true);
      assert.deepEqual(result.cleanup, {browserClosed: true, childClosed: true, stderrEnded: true});
      assert.equal(h.reports.length, 1);
      assert.ok(!JSON.stringify(result).includes('synthetic-private-secret'));
      assert.ok(!JSON.stringify(result).includes('synthetic-reply'));
    } finally { fs.rmSync(fixture.directory, {recursive: true}); }
  }
});
test('原恢复scope失败保留明确阶段并停止，不用后续新会话音频冒充通过', async () => {
  const fixture = environment(), h = harness('active', {badScope: true});
  try { await assert.rejects(live.run(fixture.env, h)); assert.equal(h.reports[0].phase, 'authoritative_restoration');
    assert.equal(h.reports[0].result, 'fail'); assert.equal(h.reports[0].audio, null);
  } finally { fs.rmSync(fixture.directory, {recursive: true}); }
});
test('回复等待的首因不被诊断或finally失败覆盖，失败事实保留且不泄露ID', async () => {
  for (const options of [{replyWaitError: true}, {replyWaitError: true, closeError: true,
    diagnosticFactsError: true, diagnosticStartupError: true}]) {
    const fixture = environment(), h = harness('active', options);
    try {
      await assert.rejects(live.run(fixture.env, h));
      const report = h.reports[0];
      assert.equal(report.phase, 'baseline_audio');
      assert.deepEqual(report.diagnostic.primary, {phase: 'baseline_audio', subphase: 'reply_complete',
        reason: 'browser_command_failed', browserOperation: 'wait'});
      assert.deepEqual(report.rounds, []);
      if (options.diagnosticFactsError) assert.equal(report.diagnostic.facts, null);
      else { assert.equal(report.diagnostic.facts.recognized, 1); assert.equal(report.diagnostic.facts.captureOpen, false); }
      assert.ok(!JSON.stringify(report).includes('synthetic-private-secret'));
      assert.ok(!JSON.stringify(report).includes('synthetic-reply'));
    } finally { fs.rmSync(fixture.directory, {recursive: true}); }
  }
});
test('逾期只认原运行时失败退出和晚操作被拒，不创建第二模型会话', async () => {
  const fixture = environment('expired'), h = harness('expired');
  try { const result = await live.run(fixture.env, h); assert.equal(result.result, 'pass');
    assert.equal(result.recovery.expiredFailClosed, true); assert.equal(result.audio, null);
    assert.equal(h.commands.filter(args => args[2] === 'click' && args[3] === '#end').length, 0);
  } finally { fs.rmSync(fixture.directory, {recursive: true}); }
});
test('浏览器关闭失败或stderr未排空不能PASS；TERM/KILL未知退出保持cleanup失败', async () => {
  for (const options of [{closeError: true}, {noStderr: true}, {neverClose: true}]) {
    const fixture = environment(), h = harness('active', options);
    try { await assert.rejects(live.run(fixture.env, h)); assert.equal(h.reports[0].result, 'fail');
      if (options.neverClose) { assert.equal(h.reports[0].cleanup.childClosed, false); assert.deepEqual(h.signals, ['SIGTERM', 'SIGKILL']); }
      assert.ok(!JSON.stringify(h.reports).includes('synthetic-private-secret'));
    } finally { fs.rmSync(fixture.directory, {recursive: true}); }
  }
});

// 文件说明：正式客户端工具验收器的离线合同/失败收尾测试；合成端口不构成设备或模型验收。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const {EventEmitter} = require('node:events');
const {createHash} = require('node:crypto');
const live = require('./browser_client_tool_live.cjs');

// environment合成身份只验证准入边界，测试文件作为已存在路径，不创建真实资产/许可/显示器。
function environment() {
  return {UR_CLIENT_TOOL_LIVE: '1', UR_CLIENT_TOOL_CHAIN: 'cascaded_agentruntime', UR_MEDIA_TRANSPORT: 'udp',
    UR_BROWSER_PORT: '39871', UR_DEVICE_BROWSER_BIN: __filename, AGENT_BROWSER_HEADED: '1', DISPLAY: ':synthetic',
    XAUTHORITY: '/synthetic/private-authority', UR_DEVICE_PRODUCT: 'synthetic-product', UR_DEVICE_NAME: 'synthetic-device',
    UR_DEVICE_SECRET: 'synthetic-private-secret', UR_MQTT_HOST: '127.0.0.1', UR_MQTT_PORT: '12345'};
}
test('取消迟到探针仅显式开启且不借无媒体模式冒充音频链验收', () => {
  assert.equal(live.validateEnvironment({...environment(),UR_CLIENT_TOOL_CANCEL_LATE:'1'}).UR_MEDIA_TRANSPORT,'udp');
  assert.throws(() => live.validateEnvironment({...environment(),UR_CLIENT_TOOL_CANCEL_LATE:'0'}));
  assert.throws(() => live.validateEnvironment({...environment(),UR_CLIENT_TOOL_CANCEL_LATE:'1',UR_MEDIA_TRANSPORT:'none'}));
});

test('串联工具音频回复显式开启，None及原生链不能混用此开关', () => {
  assert.equal(live.validateEnvironment({...environment(), UR_CLIENT_TOOL_AUDIO_REPLY:'1'}).UR_MEDIA_TRANSPORT,'udp');
  for (const change of [{UR_CLIENT_TOOL_AUDIO_REPLY:'0'}, {UR_MEDIA_TRANSPORT:'none'}, {UR_CLIENT_TOOL_CHAIN:'realtime_native'}])
    assert.throws(() => live.validateEnvironment({...environment(), UR_CLIENT_TOOL_AUDIO_REPLY:'1', ...change}));
});

test('真实Execute延迟只准本机UDP和原接收诊断，不混入本地事实延迟器', () => {
  const env = {...environment(),UR_CLIENT_TOOL_DELAY_EXECUTE:'1',UR_DEVICE_MESSAGE_DIAGNOSTICS:'1'};
  assert.equal(live.validateEnvironment(env),env);
  for(const change of [{UR_CLIENT_TOOL_DELAY_EXECUTE:'0'},{UR_MEDIA_TRANSPORT:'none'},
    {UR_MQTT_HOST:'remote.invalid'},{UR_DEVICE_MESSAGE_DIAGNOSTICS:undefined},{UR_CLIENT_TOOL_CANCEL_LATE:'1'}])
    assert.throws(() => live.validateEnvironment({...env,...change}));
});
// facts完整原前后关系，所有ID仅在离线测试内，安全报告不返回这些ID。
function facts(index = 0) {
  const receipts = [], tools = [];
  for (let round = 1; round <= index; ++round) {
    receipts.push({sessionId: 'synthetic-session', responseId: 'synthetic-reply-' + round,
      toolCallId: 'synthetic-call-' + round, requestMethod: 'toolCallSubmit', code: 200, continuation: 'accepted', receiptIndex: round});
    tools.push({id: 'synthetic-call-' + round, response: 'synthetic-reply-' + round, state: 'started', approved: true},
      {id: 'synthetic-call-' + round, response: 'synthetic-reply-' + round, state: 'succeeded'});
  }
  const theme = index === 1 ? 'dark' : index === 2 ? 'light' : '';
  return {accepted: index, submitted: index, rejected: 0, localTextAttempts: index, localTextSent: index,
    overflow: false, tools,
    completed: index, completedID: index ? 'synthetic-reply-' + index : '', active: false, audioState: index ? 'none' : null,
    captureClosed: true, contextRunning: true, driverReady: true, visible: true, receiptIndex: index, receipts,
    actualRecords: index, actualStatus: index ? 'succeeded' : 'missing', actualTheme: theme, theme,
    background: theme === 'dark' ? 'rgb(15, 23, 42)' : 'rgb(248, 250, 252)',
    foreground: theme === 'dark' ? 'rgb(248, 250, 252)' : 'rgb(15, 23, 42)'};
}

// voiceFacts合成原生语音的真实事实形状，不把文字提交计数或暂停采集当持续上行。
function voiceFacts(index = 0) {
  return {...facts(index), accepted: 0, submitted: 0, localTextAttempts: 0, localTextSent: 0,
    audioState: null, captureClosed: false, captureOpen: true, captured: 100 + index * 200,
    interactionMode: 'fullDuplex', recognized: index, playableResponses: index,
    consumedNonSilent: index * 20, pendingPlayback: 0,
    playableResponse: index ? 'synthetic-reply-' + index : '',
    consumedResponse: index ? 'synthetic-reply-' + index : '', consumedResponseFrames: index ? 20 : 0};
}

test('串联音频工具要求同一原回复实际解码消费，文字完成和旧轮音频不能代替', () => {
  const audioFacts = index => ({...facts(index), playableResponses:index, consumedNonSilent:index * 20,
    pendingPlayback:0, playableResponse:index ? 'synthetic-reply-' + index : '',
    consumedResponse:index ? 'synthetic-reply-' + index : '', consumedResponseFrames:index ? 20 : 0});
  const verify = value => live.verifyRound(live.rounds[0], audioFacts(0), value, 'udp', 'cascaded_agentruntime', true);
  assert.equal(verify(audioFacts(1)).originalReplyDecodedAndConsumed, true);
  for (const change of [{playableResponses:0}, {consumedNonSilent:0}, {pendingPlayback:1},
    {playableResponse:'old'}, {consumedResponse:'old'}, {consumedResponseFrames:1}])
    assert.throws(() => verify({...audioFacts(1), ...change}));
  assert.throws(() => live.verifyRound(live.rounds[0], audioFacts(0), audioFacts(1), 'none', 'cascaded_agentruntime', true));
});

test('Realtime语音工具必须持续采集、识别新语句且实际解码消费同原回复，不借文字计数通过', () => {
  assert.equal(live.verifyRound(live.rounds[0], voiceFacts(), voiceFacts(1), 'udp', 'realtime_native').passed, true);
  for (const change of [{captureOpen: false}, {captureClosed: true}, {captured: 100},
    {interactionMode: 'halfDuplex'}, {recognized: 0}, {playableResponses: 0},
    {consumedNonSilent: 0}, {playableResponse: 'old'}, {consumedResponse: 'old'},
    {consumedResponseFrames: 1}, {pendingPlayback: 1}, {localTextAttempts: 1}, {submitted: 1}])
    assert.throws(() => live.verifyRound(live.rounds[0], voiceFacts(), {...voiceFacts(1), ...change}, 'udp', 'realtime_native'));
  assert.throws(() => live.verifyRound(live.rounds[0], voiceFacts(), voiceFacts(1), 'none', 'realtime_native'));
});

// syntheticVoice仅验证WAV准入，离线方波不是可供模型识别的收费语料。
function syntheticVoice() {
  const bytes = Buffer.alloc(44 + 1920);
  bytes.write('RIFF'); bytes.writeUInt32LE(bytes.length - 8, 4); bytes.write('WAVEfmt ', 8);
  bytes.writeUInt32LE(16, 16); bytes.writeUInt16LE(1, 20); bytes.writeUInt16LE(1, 22);
  bytes.writeUInt32LE(16000, 24); bytes.writeUInt32LE(32000, 28); bytes.writeUInt16LE(2, 32);
  bytes.writeUInt16LE(16, 34); bytes.write('data', 36); bytes.writeUInt32LE(1920, 40);
  for (let offset = 44; offset < bytes.length; offset += 2) bytes.writeInt16LE(offset % 4 ? 400 : -400, offset);
  return bytes;
}

test('Realtime固定语料要求原摘要、严格PCM格式和连续两有声帧，损坏和静音在spawn前失败', () => {
  const sha = bytes => createHash('sha256').update(bytes).digest('hex');
  const bytes = syntheticVoice();
  assert.equal(live.voiceWave(bytes, sha(bytes)), bytes);
  assert.throws(() => live.voiceWave(bytes, '0'.repeat(64)));
  for (const mutate of [
    value => value.write('RIFX'), value => value.writeUInt32LE(0, 4),
    value => value.writeUInt16LE(2, 20), value => value.writeUInt16LE(2, 22),
    value => value.writeUInt32LE(24000, 24), value => value.writeUInt32LE(0, 28),
    value => value.writeUInt16LE(0, 32), value => value.writeUInt16LE(8, 34),
    value => value.writeUInt32LE(1921, 40), value => value.fill(0, 44),
    value => value.fill(0, 44 + 640),
  ]) {
    const changed = Buffer.from(bytes); mutate(changed);
    assert.throws(() => live.voiceWave(changed, sha(changed)));
  }
  assert.throws(() => live.voiceWave(bytes.subarray(0, bytes.length - 1), sha(bytes.subarray(0, bytes.length - 1))));
});

test('Realtime仅接受UDP和两条显式语料摘要，不沿None或文字路径降级', () => {
  const env = {...environment(), UR_CLIENT_TOOL_CHAIN: 'realtime_native',
    UR_CLIENT_TOOL_DARK_WAV: __filename, UR_CLIENT_TOOL_LIGHT_WAV: __filename,
    UR_CLIENT_TOOL_DARK_SHA256: 'a'.repeat(64), UR_CLIENT_TOOL_LIGHT_SHA256: 'b'.repeat(64)};
  assert.equal(live.validateEnvironment(env), env);
  for (const change of [{UR_MEDIA_TRANSPORT: 'none'}, {UR_CLIENT_TOOL_DARK_WAV: 'relative'},
    {UR_CLIENT_TOOL_LIGHT_WAV: ''}, {UR_CLIENT_TOOL_DARK_SHA256: 'unknown'}, {UR_CLIENT_TOOL_LIGHT_SHA256: ''}])
    assert.throws(() => live.validateEnvironment({...env, ...change}));
});

test('固定语料先隔离Chromium模拟麦克风的周期音，保留原Worklet和持续静音采集', () => {
  const calls = [];
  const worklet = {};
  const silence = {offset: {value: 9}, connect: value => calls.push(['connect', value]),
    start: () => calls.push(['start'])};
  const context = {context: {createConstantSource: () => silence}, worklet,
    source: {disconnect: () => calls.push(['disconnect'])}};
  vm.runInNewContext('(' + live.prepareVoiceInput.toString() + ')()', context);
  assert.deepEqual(calls, [['disconnect'], ['connect', worklet], ['start']]);
  assert.equal(silence.offset.value, 0);
  assert.equal(context.__urClientToolSilence, silence);
  assert.throws(() => vm.runInNewContext('(' + live.prepareVoiceInput.toString() + ')()', context));
  assert.equal(calls.length, 3, '重复准备不得再次断开原输入或创建新源');
});

test('语音识别观察仅统计本轮原用户转写一次，助手同词和旧转写不代替新语句', () => {
  let called = 0;
  const context = {incoming: () => { ++called; return 7; }, send: () => true};
  vm.runInNewContext('(' + live.installObserver.toString() + ')()', context);
  const observer = context.__urClientToolLive;
  observer.expectedKeyword = '深色';
  for (const text of ['\n助手：深色', '\n用户：浅色', '\n用户：请切换深色', '\n用户：请切换深色。'])
    assert.equal(context.incoming({kind: 'text', text}), 7);
  assert.equal(called, 4); assert.equal(observer.recognized, 1);
  observer.expectedKeyword = '浅色'; observer.recognizedThisRound = false;
  context.incoming({kind: 'text', text: '\n用户：深色'});
  assert.equal(observer.recognized, 1);
  context.incoming({kind: 'text', text: '\n用户：请切换浅色'});
  assert.equal(observer.recognized, 2);
});

test('默认关闭/未知链/错误产物或非可见浏览器在spawn前拒绝', () => {
  assert.equal(live.validateEnvironment(environment()).UR_CLIENT_TOOL_LIVE, '1');
  assert.equal(live.validateEnvironment({...environment(), UR_MEDIA_TRANSPORT: 'none'}).UR_MEDIA_TRANSPORT, 'none');
  for (const change of [{UR_CLIENT_TOOL_LIVE: undefined}, {UR_CLIENT_TOOL_LIVE: '0'},
    {UR_CLIENT_TOOL_CHAIN: 'realtime_native'}, {UR_MEDIA_TRANSPORT: 'webrtc'}, {AGENT_BROWSER_HEADED: '0'},
    {DISPLAY: ''}, {XAUTHORITY: 'relative'}, {UR_OUTPUT_FOCUS: 'on'}, {UR_FILE_CATALOG: 'file'},
    {UR_LIVE_CATALOG: 'live'}, {UR_BROWSER_PORT: '08000'}, {UR_BROWSER_PORT: '1000'}, {UR_BROWSER_PORT: '65536'},
    {UR_DEVICE_BROWSER_BIN: 'relative'}, {UR_DEVICE_SECRET: ''}])
    assert.throws(() => live.validateEnvironment({...environment(), ...change}));
});
test('None主题验收检查双时点真实媒体缺席，不伪造音频线程已停止', () => {
  const none = value => ({...value, textOnly: true, mediaAbsent: true, contextRunning: false,
    captureClosed: false, audioChoiceEnabled: false});
  const before = none(facts()), after = none(facts(1));
  assert.equal(live.verifyRound(live.rounds[0], before, after, 'none').passed, true);
  for (const change of [{textOnly: false}, {mediaAbsent: false}, {contextRunning: true},
                       {audioChoiceEnabled: true}]) {
    assert.throws(() => live.verifyRound(live.rounds[0], {...before, ...change}, after, 'none'));
    assert.throws(() => live.verifyRound(live.rounds[0], before, {...after, ...change}, 'none'));
  }
  assert.throws(() => live.verifyRound(live.rounds[0], before, after, 'udp'));
});

test('失败投影仅固定计数/布尔/枚举，不泄漏原会话/调用/票据/自由正文', () => {
  const projected = live.projectDiagnostic({...facts(1), freeText: 'synthetic-private-text', themeTicket: 'synthetic-ticket'});
  assert.equal(projected.theme, 'dark');
  assert.equal(projected.receiptCount, 1);
  assert.equal(projected.lastReceiptMatchesCompletedReply, true);
  assert.ok(!JSON.stringify(projected).includes('synthetic-'));
  assert.equal(live.projectDiagnostic({...facts(), actualRecords: Infinity}).actualRecords, null);
  assert.equal(live.projectDiagnostic(null), null);
});

// 端接收诊断必须来自原分支的固定快照，不能反射自由标识或把不完整记录当已知。
test('端接收诊断保留固定原水位和分支，非法或超预算投影显式未知', () => {
  const original = {received: 2, taken: 1, hostFiltered: 0, hostSent: 1,
    hostSendFailed: 0, overflow: false, records: [{method: 'respCreated',
      outcome: 'content_queued', sequence: 1, watermark: 0, parsed: true,
      sessionMatches: true, taken: true, hostObserved: true, projected: true,
      sent: true, sessionId: 'synthetic-private-session', body: 'synthetic-private-body'}],
    payload: 'synthetic-private-payload'};
  const projected = live.projectMessageDiagnostics(original);
  assert.equal(projected.received, 2);
  assert.equal(projected.records[0].sequence, 1);
  assert.equal(projected.records[0].outcome, 'content_queued');
  assert.ok(!JSON.stringify(projected).includes('synthetic-'));
  assert.deepEqual(live.projectDiagnostic({...facts(), messageDiagnostics: original}).messageReception, projected);
  for (const change of [{received: Infinity}, {overflow: 'false'}, {records: null},
    {records: Array.from({length: 33}, () => original.records[0])},
    {records: [{...original.records[0], method: 'synthetic-private-method'}]},
    {records: [{...original.records[0], outcome: 'synthetic-private-error'}]},
    {records: [{...original.records[0], sequence: 9007199254740992}]},
    {records: [{...original.records[0], sessionMatches: null}]}])
    assert.equal(live.projectMessageDiagnostics({...original, ...change}), null);
  assert.equal(live.projectMessageDiagnostics(undefined), null);
});

// 原页面处理仅调用一次；观察器持有脱敏副本，不保留可变报文引用或生成执行结果。
test('端接收观察不改变原函数返回和调用次数，不保存自由正文或后续变异', () => {
  const calls = [];
  const context = {incoming: message => { calls.push(message); return 17; }, send: () => true};
  vm.runInNewContext('(' + live.installObserver.toString() + ')(' +
    live.projectMessageDiagnostics.toString() + ')', context);
  const message = {kind: 'messageDiagnostic', received: 1, taken: 0, hostFiltered: 0,
    hostSent: 0, hostSendFailed: 0, overflow: false, records: [{method: 'respCreated',
      outcome: 'session_mismatch', sequence: 1, watermark: 0, parsed: true,
      sessionMatches: false, taken: false, hostObserved: false, projected: false,
      sent: false, secret: 'synthetic-private-secret'}], body: 'synthetic-private-body'};
  assert.equal(context.incoming(message), 17);
  assert.equal(calls.length, 1);
  assert.equal(calls[0], message);
  message.records[0].sequence = 99;
  const observed = context.__urClientToolLive;
  assert.equal(observed.messageDiagnostics.records[0].sequence, 1);
  assert.equal(observed.tools.length, 0);
  assert.equal(observed.accepted, 0);
  assert.ok(!JSON.stringify(observed).includes('synthetic-'));
});

test('同原回复唯一实际副作用及ACK明确续答都齐备才通过，不返回身份或自由正文', () => {
  const report = live.verifyRound(live.rounds[0], facts(), facts(1));
  assert.equal(report.passed, true);
  assert.equal(report.theme, 'dark');
  assert.equal(live.verifyRound(live.rounds[1], facts(1), facts(2)).theme, 'light');
  assert.ok(!JSON.stringify(report).includes('synthetic-'));
  for (const change of [{accepted: 0}, {submitted: 0}, {rejected: 1}, {overflow: true}, {completed: 0},
    {completedID: 'wrong'}, {active: true}, {audioState: 'sent'}, {captureClosed: false}, {contextRunning: false},
    {driverReady: false}, {visible: false}, {actualRecords: 0}, {actualRecords: 2}, {actualStatus: 'unknown'},
    {actualTheme: 'light'}, {theme: 'light'}, {background: 'wrong'}, {receiptIndex: 0}, {receipts: []}, {tools: []}])
    assert.throws(() => live.verifyRound(live.rounds[0], facts(), {...facts(1), ...change}));
});

test('仅结果收件accepted但continuation unknown、错原会话/调用或其他工具均不能误PASS', () => {
  for (const change of [{continuation: 'unknown'}, {code: 403}, {responseId: 'old-reply'},
    {toolCallId: 'wrong-call'}, {requestMethod: 'audioStart'}]) {
    const after = facts(1);
    after.receipts[0] = {...after.receipts[0], ...change};
    assert.throws(() => live.verifyRound(live.rounds[0], facts(), after));
  }
  const next = facts(2);
  next.receipts[1].sessionId = 'other-session';
  assert.throws(() => live.verifyRound(live.rounds[1], facts(1), next));
  for (const change of [{approved: false}, {id: 'wrong'}, {response: 'wrong'}]) {
    const after = facts(1);
    after.tools[0] = {...after.tools[0], ...change};
    assert.throws(() => live.verifyRound(live.rounds[0], facts(), after));
  }
  const after = facts(1);
  after.tools.push({...after.tools[0]});
  assert.throws(() => live.verifyRound(live.rounds[0], facts(), after));
});

test('只读展示observer原incoming只执行一次，不替换返回/参数或保存正文/工具名称', () => {
  const calls = [], context = vm.createContext({send: () => true,
    incoming: message => { calls.push(message); return 'unchanged'; }});
  vm.runInContext('(' + live.installObserver.toString() + ')()', context);
  assert.equal(vm.runInContext('incoming({kind:"textStatus",result:"accepted"})', context), 'unchanged');
  vm.runInContext('incoming({kind:"text",text:"synthetic-private-free-text"}); incoming({kind:"toolStatus",id:"call",responseId:"reply",state:"started",name:"synthetic-private-tool-name"})', context);
  assert.equal(calls.length, 3);
  const projected = JSON.parse(vm.runInContext('JSON.stringify(__urClientToolLive)', context));
  assert.equal(projected.accepted, 1);
  assert.equal(projected.tools[0].approved, false);
  assert.ok(!JSON.stringify(projected).includes('synthetic-private'));
  assert.throws(() => vm.runInContext('(' + live.installObserver.toString() + ')()', context));
});

test('发送观察保留原send唯一调用及返回，只计实际文字尝试，不记录文字或伪造受理', () => {
  const calls = [], context = vm.createContext({incoming: () => undefined,
    send: message => { calls.push(message); return message.audio === false; }});
  vm.runInContext('(' + live.installObserver.toString() + ')()', context);
  assert.equal(vm.runInContext('send({kind:"sendText",text:"synthetic-private-body",audio:false})', context), true);
  assert.equal(vm.runInContext('send({kind:"sendText",text:"synthetic-private-body",audio:true})', context), false);
  vm.runInContext('send({kind:"stop"})', context);
  assert.equal(calls.length, 3);
  const observed = JSON.parse(vm.runInContext('JSON.stringify(__urClientToolLive)', context));
  assert.equal(observed.localTextAttempts, 2);
  assert.equal(observed.localTextSent, 1);
  assert.equal(observed.accepted, 0);
  assert.equal(observed.submitted, 0);
  assert.ok(!JSON.stringify(observed).includes('synthetic-private-body'));
});

// harness仅测试控制面，合成close/end分别发送以核实真实双流结束不能由close伪造。
function harness(options = {}) {
  const child = new EventEmitter(), reports = [], commands = [], signals = [];
  child.stdout = new EventEmitter(); child.stderr = new EventEmitter();
  let round = 0, inputPaused = !options.initialCaptureOpen;
  const finish = () => setImmediate(() => {
    if (!options.noStdoutEnd) child.stdout.emit('end');
    if (!options.noStderrEnd) child.stderr.emit('end');
    if (options.emitExit) child.emit('exit', options.exitCode ?? 0, null);
    child.emit('close', options.exitCode ?? 0);
  });
  child.kill = signal => { signals.push(signal); if (!options.neverClose) finish(); return true; };
  return {reports, commands, signals, closeTimeoutMs: 3,
    spawn(binary, args, config) {
      assert.equal(binary, __filename); assert.deepEqual(args, ['39871']);
      assert.equal(config.env.UR_CLIENT_TOOL_LIVE, '1');
      setImmediate(() => {
        child.stdout.emit('data', Buffer.from('http://127.0.0.'));
        child.stdout.emit('data', Buffer.from('1:39871/\n'));
        if (options.lifecycle) {
          child.stdout.emit('data',Buffer.from('voice_state=1\nvoice_sta'));
          child.stdout.emit('data',Buffer.from('te=4\nhost_exit_branch='+options.lifecycle+'\n'));
        }
        if (options.startFailure) finish();
      });
      return child;
    },
    execFile(binary, args, config, callback) {
      commands.push(args);
      assert.equal(binary, 'agent-browser'); assert.equal(args[2], '--headed');
      const action = args[3], value = args[4];
      let output = 'ok';
      if (action === 'eval' && value.includes('function readFacts')) {
        const projected = facts(round);
        projected.captureClosed = inputPaused;
        if (options.badAck && round === 2) projected.receipts[1].continuation = 'unknown';
        output = JSON.stringify(projected);
      }
      if (action === 'eval' && value.includes('function readStartupFacts'))
        output = JSON.stringify({captureState: 'unknown', contextRunning: true, driverReady: true,
          textEnabled: true, activeReply: false, terminated: false});
      if (action === 'click' && value === '#sendText') ++round;
      if (action === 'click' && value === '#stop') inputPaused = true;
      if (action === 'click' && value === '#end' && !options.neverClose) finish();
      if (options.closeOnReplyWait && action === 'wait' && args[5]?.includes('metrics.completedResponses')) {
        finish();
        setImmediate(() => callback(Error('synthetic-private-wait-failure')));
        return new EventEmitter();
      }
      setImmediate(() => callback((options.browserCloseError && action === 'close') ||
        (options.replyWaitFailure && action === 'wait' && args[5]?.includes('metrics.completedResponses')) ||
        (options.capturePauseFailure && action === 'wait' && args[5]?.includes('captureOpen'))
        || (options.failedRoundAction === action && ['fill', 'uncheck', 'click'].includes(action) && value !== '#activate' && value !== '#stop' && value !== '#end')
        ? Error('synthetic-private-secret') : null, output));
      return new EventEmitter();
    },
    report(value) { reports.push(JSON.parse(value)); },
  };
}

test('采集准备失败只记录真实固定启动事实，不提交模型语料或泄漏浏览器异常', async () => {
  const h = harness({capturePauseFailure: true});
  await assert.rejects(live.run(environment(), h));
  assert.equal(h.reports[0].phase, 'capturePause');
  assert.deepEqual(h.reports[0].diagnostic.startupFacts,
    {captureState: 'unknown', contextRunning: true, driverReady: true,
      textEnabled: true, activeReply: false, terminated: false});
  assert.equal(h.reports[0].results.length, 0);
  assert.equal(h.commands.some(args => args[3] === 'fill'), false);
  assert.ok(!JSON.stringify(h.reports).includes('synthetic-private'));
});

test('真实两轮驱动在原END点击前先滚回原按钮，原正常退出判据不变', async () => {
  const h=harness();await live.run(environment(),h);
  const close=h.commands.findIndex(args=>args[3]==='click'&&args[4]==='#end');
  assert.ok(close>0);assert.equal(h.commands[close-1][3],'scrollintoview');assert.equal(h.commands[close-1][4],'#end');
  assert.equal(h.reports[0].passed,true);assert.equal(h.reports[0].results.length,2);
});

test('原轮次UI失败记录确切准备步骤，不吞错误、不补模型输入或暴露自由正文', async () => {
  for (const [action, expected] of [['fill', 'fillInput'], ['uncheck', 'audioChoice'], ['click', 'submitInput']]) {
    const h = harness({failedRoundAction: action});
    await assert.rejects(live.run(environment(), h));
    assert.equal(h.reports[0].phase, 'dark');
    assert.equal(h.reports[0].diagnostic.roundStep, expected);
    assert.equal(h.reports[0].results.length, 0);
    assert.equal(h.commands.filter(args => args[3] === action && !['#activate', '#stop', '#end'].includes(args[4])).length, 1);
    assert.ok(!JSON.stringify(h.reports).includes('synthetic-private'));
  }
});

test('失败后的页面采样未知不能拿提交前零计数冒充失败时实态', async () => {
  const h = harness(), original = h.execFile;
  let snapshots = 0;
  h.execFile = (binary, args, config, callback) => {
    if (args[3] === 'eval' && args[4].includes('function readFacts') && ++snapshots > 1) {
      setImmediate(() => callback(Error('synthetic-private-snapshot-failure')));
      return new EventEmitter();
    }
    return original(binary, args, config, callback);
  };
  await assert.rejects(live.run(environment(), h));
  assert.equal(h.reports[0].diagnostic.uiFacts, null);
  assert.equal(h.reports[0].timing.freshAfterFailure,false);
  assert.ok(!JSON.stringify(h.reports).includes('synthetic-private'));
});

test('等待回复期间child原exit/close后不能把提交快照冒充失败当前UI事实', async () => {
  const h=harness({closeOnReplyWait:true,emitExit:true});
  const original=h.execFile;let snapshots=0;
  h.execFile=(binary,args,config,callback)=>{
    if(args[3]==='eval'&&args[4].includes('function readFacts')&&++snapshots===3) {
      setImmediate(()=>callback(Error('synthetic-private-closed-snapshot')));return new EventEmitter();
    }
    return original(binary,args,config,callback);
  };
  await assert.rejects(live.run(environment(),h));
  const report=h.reports[0];assert.equal(report.phase,'dark');
  assert.equal(report.diagnostic.roundStep,'awaitReply');assert.equal(report.diagnostic.uiFacts,null);
  assert.equal(report.diagnostic.deviceExitCode,0);assert.equal(report.cleanup.childClosed,true);
  assert.equal(snapshots,3);
  assert.equal(h.commands.filter(args=>args[3]==='wait'&&args[5]?.includes('metrics.completedResponses')).length,1);
  assert.equal(h.commands.filter(args=>args[3]==='close').length,1);assert.deepEqual(h.signals,[]);
  const time=report.timing;
  for(const name of ['childExitMs','childCloseMs','waitFailedMs','finallyMs','browserCloseRequestedMs','latestFactsObservedMs'])
    assert.ok(Number.isSafeInteger(time[name])&&time[name]>=0,name);
  assert.ok(time.latestFactsObservedMs<=time.childExitMs);assert.ok(time.childExitMs<=time.childCloseMs);
  assert.ok(time.childCloseMs<=time.waitFailedMs);assert.ok(time.waitFailedMs<=time.finallyMs);
  assert.ok(time.finallyMs<=time.browserCloseRequestedMs);assert.equal(time.freshAfterFailure,false);
  assert.ok(!JSON.stringify(report).includes('synthetic-private'));
});

test('closed后原页面仍可读时只报告新快照，fresh不是CPP在线事实',async()=>{
  const h=harness({closeOnReplyWait:true,emitExit:true}),original=h.execFile;let snapshots=0;
  h.execFile=(binary,args,config,callback)=>{
    if(args[3]==='eval'&&args[4].includes('function readFacts')&&++snapshots===3) {
      setImmediate(()=>callback(null,JSON.stringify({...facts(),terminated:true,localSocketOpen:false,
        contextRunning:false,releaseCategory:'invalid_local_message',failedIncomingKind:'text'})));return new EventEmitter();
    }
    return original(binary,args,config,callback);
  };
  await assert.rejects(live.run(environment(),h));const report=h.reports[0];
  assert.equal(snapshots,3);assert.equal(report.cleanup.childClosed,true);
  assert.equal(report.diagnostic.uiFacts.terminated,true);assert.equal(report.diagnostic.uiFacts.contextRunning,false);
  assert.equal(report.diagnostic.uiFacts.releaseCategory,'invalid_local_message');
  assert.equal(report.diagnostic.uiFacts.failedIncomingKind,'text');
  assert.equal(report.timing.freshAfterFailure,true);
  assert.ok(report.timing.latestFactsObservedMs>=report.timing.childCloseMs);
  assert.equal(report.results.length,0);assert.deepEqual(h.signals,[]);
});

test('END未按原截止关闭时重新观察当前页面，清理exit0和终态快照都不能将整轮失败改成通过',async()=>{
  for(const readFails of [false,true]){
    const h=harness({neverClose:true}),originalSpawn=h.spawn,original=h.execFile;
    let child,endClicked=false,readsAfterEnd=0;
    h.spawn=(...args)=>child=originalSpawn(...args);
    h.execFile=(binary,args,config,callback)=>{
      if(args[3]==='click'&&args[4]==='#end')endClicked=true;
      if(endClicked&&args[3]==='eval'&&args[4].includes('function readFacts')){
        ++readsAfterEnd;
        setImmediate(()=>callback(readFails?Error('synthetic-private-read-failed'):null,
          JSON.stringify({...facts(2),terminated:true,localSocketOpen:false,contextRunning:false,releaseCategory:'session_ended'})));
        return new EventEmitter();
      }
      if(args[3]==='close')setImmediate(()=>{
        child.stdout.emit('end');child.stderr.emit('end');child.emit('exit',0);child.emit('close',0);
      });
      return original(binary,args,config,callback);
    };
    await assert.rejects(live.run(environment(),h));const report=h.reports[0];
    assert.equal(readsAfterEnd,1);assert.equal(report.phase,'sessionClose');assert.equal(report.passed,false);
    assert.equal(report.results.length,2);assert.equal(report.cleanup.childClosed,true);assert.equal(report.diagnostic.deviceExitCode,0);
    assert.equal(report.timing.freshAfterFailure,!readFails);
    if(readFails)assert.equal(report.diagnostic.uiFacts,null);
    else {assert.equal(report.diagnostic.uiFacts.terminated,true);assert.equal(report.diagnostic.uiFacts.releaseCategory,'session_ended');}
    assert.ok(!JSON.stringify(report).includes('synthetic-private'));
  }
});

test('正常退出边界仅复用真实stdout整行观察，旧产物和未end保持未知',async()=>{
  for(const lifecycle of ['browser_closed','runtime_idle']) {
    const h=harness({lifecycle});const report=await live.run(environment(),h);
    assert.deepEqual(report.deviceLifecycle.voiceStates,[1,4]);
    assert.equal(report.deviceLifecycle.normalHostExitBranch,lifecycle);
    assert.equal(report.deviceLifecycle.stdoutEnded,true);
  }
  const plain=await live.run(environment(),harness());assert.equal(plain.deviceLifecycle.normalHostExitBranch,null);
  const h=harness({lifecycle:'forged_private_reason',noStdoutEnd:true});await assert.rejects(live.run(environment(),h));
  assert.equal(h.reports[0].deviceLifecycle.normalHostExitBranch,null);assert.equal(h.reports[0].deviceLifecycle.stdoutEnded,false);
  assert.ok(!JSON.stringify(h.reports).includes('forged_private_reason'));
});

test('正常退出输出只在原两个正常分支及精确诊断开关内，不更改业务退出',()=>{
  const source=require('node:fs').readFileSync(require('node:path').join(__dirname,'../adapters/desktop/browser_main.cpp'),'utf8');
  for(const name of ['browser_closed','runtime_idle'])assert.equal((source.match(new RegExp('host_exit_branch='+name,'g'))||[]).length,1);
  const closed=source.slice(source.indexOf('if (event->kind == BrowserEventKind::Closed ||'),source.indexOf('if (event->kind != BrowserEventKind::Message)'));
  assert.match(closed,/event->kind == BrowserEventKind::Closed\)\s*\{/);
  assert.match(closed,/diagnostic_switch && std::string_view\(diagnostic_switch\) == "1"/);
  assert.match(closed,/failed = event->kind == BrowserEventKind::Failed;/);
  const idle=source.slice(source.indexOf('if (runtime->state() == RuntimeState::Idle)'),source.indexOf('if (progress) progress->host(HostStage::MediaCommands);'));
  assert.match(idle,/diagnostic_switch && std::string_view\(diagnostic_switch\) == "1"/);
  assert.match(idle,/host_exit_branch=runtime_idle/);assert.match(idle,/done = true;/);
});

test('只读release分类保留原this参数返回异常及一次调用，不输出自由reason',()=>{
  const calls=[],incomingFailure=Error('synthetic-private-error'),releaseFailure=Error('synthetic-private-release-error');
  const context=vm.createContext({incoming:message=>{if(message.kind==='text')throw incomingFailure;return 7;},
    send:()=>true,release:function(...args){calls.push({self:this,args});if(args[0]==='synthetic-private-error')throw releaseFailure;return 19;}});
  vm.runInContext('('+live.installObserver.toString()+')()',context);
  const message={kind:'text',text:'synthetic-private-body'};
  assert.throws(()=>context.incoming(message),error=>error===incomingFailure);
  assert.equal(context.release('本地媒体报文非法，已停止采集与播放。'),19);
  assert.equal(calls.length,1);assert.equal(calls[0].self,context);assert.equal(calls[0].args[0],'本地媒体报文非法，已停止采集与播放。');
  assert.equal(context.__urClientToolLive.releaseCategory,'invalid_local_message');
  assert.equal(context.__urClientToolLive.failedIncomingKind,'text');
  assert.throws(()=>context.release('synthetic-private-error'),error=>error===releaseFailure);assert.equal(calls.length,2);
  assert.equal(context.__urClientToolLive.releaseCategory,'invalid_local_message');
  assert.ok(!JSON.stringify(context.__urClientToolLive).includes('synthetic-private'));
  const projected=live.projectDiagnostic({...facts(),releaseCategory:'synthetic-private-reason',failedIncomingKind:'synthetic-private-kind'});
  assert.equal(projected.releaseCategory,'unknown');assert.equal(projected.failedIncomingKind,'unknown');
});

test('等待回复失败仍存活时只复用原一次失败快照，缺exit事件不补造exit时点',async()=>{
  const h=harness({replyWaitFailure:true});await assert.rejects(live.run(environment(),h));
  const report=h.reports[0],time=report.timing;
  assert.equal(report.diagnostic.roundStep,'awaitReply');assert.notEqual(report.diagnostic.uiFacts,null);
  assert.equal(time.freshAfterFailure,true);assert.equal(time.childExitMs,null);
  assert.ok(time.waitFailedMs<=time.latestFactsObservedMs);assert.ok(time.latestFactsObservedMs<=time.finallyMs);
  assert.ok(time.browserCloseRequestedMs<=time.childCloseMs);
  assert.equal(h.commands.filter(args=>args[3]==='eval'&&args[4].includes('function readFacts')).length,3);
  assert.deepEqual(h.signals,['SIGTERM']);assert.equal(h.commands.filter(args=>args[3]==='close').length,1);
});

test('两轮只通过原UI输入/点击，双流end与原child close之后唯一安全PASS', async () => {
  const h = harness();
  const report = await live.run(environment(), h);
  assert.equal(report.passed, true);
  assert.equal(report.evidence, 'offline-control-fixture');
  assert.equal(report.timing.childExitMs,null);assert.equal(report.timing.waitFailedMs,null);
  assert.equal(report.timing.freshAfterFailure,null);
  assert.deepEqual(report.cleanup, {browserClosed: true, childClosed: true, stdoutEnded: true, stderrEnded: true});
  assert.equal(h.commands.filter(args => args[3] === 'click' && args[4] === '#sendText').length, 2);
  assert.ok(h.commands.some(args => args[3] === 'uncheck' && args[4] === '#textAudio'));
  assert.ok(!h.commands.some(args => args[3] === 'click' && args[4] === '#start'));
  assert.ok(!h.commands.some(args => args[3] === 'eval' && /\.style\s*=|\.handle\(|\.send\(|new WebSocket/.test(args[4])));
  assert.ok(!JSON.stringify(h.reports).includes('synthetic-'));
});

// 正式设备建立后可合法自动开始输入；验收器须经原暂停按钮及媒体实际事实后再发送文字，不能赋指标造绿。
test('建立后采集已开时先真实UI暂停并等待原驱动事实，不直接断言默认静音', async () => {
  const h = harness({initialCaptureOpen: true});
  const report = await live.run(environment(), h);
  assert.equal(report.passed, true);
  const pause = h.commands.findIndex(args => args[3] === 'click' && args[4] === '#stop');
  const firstInput = h.commands.findIndex(args => args[3] === 'fill' && args[4] === '#textInput');
  assert.ok(pause >= 0 && firstInput > pause);
  assert.ok(h.commands.slice(pause + 1, firstInput).some(args => args[3] === 'wait' &&
    args[4] === '--fn' && args[5].includes('metrics.captureOpen === false')));
  assert.ok(!h.commands.some(args => args[3] === 'eval' && /captureOpen\s*=(?!=)/.test(args[4])));
});

test('原第二轮ACK未知保留真实失败，不补样、不重发、不把首轮成功当全通过', async () => {
  const h = harness({badAck: true});
  await assert.rejects(live.run(environment(), h));
  assert.equal(h.reports[0].passed, false);
  assert.equal(h.reports[0].phase, 'light');
  assert.equal(h.reports[0].results.length, 1);
  assert.equal(h.commands.filter(args => args[3] === 'click' && args[4] === '#sendText').length, 2);
});

test('双流缺end/close非零或独立浏览器关闭失败，效果齐全仍不得PASS', async () => {
  for (const options of [{noStdoutEnd: true}, {noStderrEnd: true}, {exitCode: 1}, {browserCloseError: true}]) {
    const h = harness(options);
    await assert.rejects(live.run(environment(), h));
    assert.equal(h.reports[0].passed, false);
    assert.ok(!JSON.stringify(h.reports).includes('synthetic-private-secret'));
  }
});

test('TERM/KILL受理不是原close，未知退出有界保留清理失败', async () => {
  const h = harness({neverClose: true});
  await assert.rejects(live.run(environment(), h));
  assert.equal(h.reports[0].cleanup.childClosed, false);
  assert.equal(h.reports[0].timing.childExitMs,null);assert.equal(h.reports[0].timing.childCloseMs,null);
  assert.deepEqual(h.signals, ['SIGTERM', 'SIGKILL']);
  assert.equal(await live.waitClosed(Promise.resolve(), 3), true);
  assert.equal(await live.waitClosed(new Promise(() => {}), 3), false);
  assert.throws(() => live.parseJSON('null'));
});

// 文件说明：AI三端接续真实驱动的判据与执行顺序回归；合成端口不构成设备、模型、焦点HAL或中央E2E证据。
'use strict';
const test = require('node:test'), assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const {EventEmitter} = require('node:events');
const {PassThrough} = require('node:stream');
const live = require('./browser_ai_endpoint_live.cjs');

// timeout期间有效原源语音必须沿同一输入；仅验证原编排，不把端口替身称为模型或UDP验证。
test('timeout原同输入有效音频在prepare之后且不延自然30秒', async () => {
  const ports = scenarioPorts('timeout'), beforeWait = ports.devices[1].wait;
  let deadlineWait = false; // 保留原自然截止等待，不以语音活动重建候选时钟。
  ports.devices[1].wait = async expression => {
    if (expression.includes('>= 30000')) deadlineWait = true;
    return beforeWait(expression);
  };
  const result = await live.executeScenario({variant: 'timeout'}, ports.devices, undefined, undefined, undefined, ports.timeoutCandidateWitness);
  const prepared = ports.calls.findIndex(item => item[0] === 'B' && item[2] === '#endpointSourcePrepare');
  const sameInput = ports.calls.map((item, index) => ({item, index}))
    .filter(({item}) => item[0] === 'B' && item[1] === 'round' && item[3] === true);
  assert.equal(sameInput.length, 1, 'timeout必须有唯一同输入活动而非纯静音');
  assert.ok(sameInput[0].index > prepared);
  assert.equal(sameInput[0].item[2], true);
  assert.equal(deadlineWait, true);
  assert.equal(result.transfers[1].timeoutThirtySecondsVerified, true);
  assert.equal(result.transfers[1].originalInputPreserved, true);
  assert.equal(result.transfers[1].originalFullGroupRetained, true);
  assert.equal(ports.calls.some(item => item[0] === 'C' && item[2] === '#endpointAccept'), false);
});

// 换输入必须触发原保持断言，不能借保活测试制造新的采集所有权。
test('timeout活动不能借换输入跳过原保持断言', async () => {
  const ports = scenarioPorts('timeout'), b = ports.devices[1], round = b.audioRound;
  b.audioRound = async (active, preserve) => {
    const value = await round(active, preserve);
    if (preserve) b.value.inputID = 'different-input';
    return value;
  };
  await assert.rejects(live.executeScenario({variant: 'timeout'}, ports.devices));
});

// 不足30秒即使出现终态也必须失败，原候选期限不能由新增音频改变。
test('timeout活动不使不足30秒的终态通过', async () => {
  const ports = scenarioPorts('timeout'), b = ports.devices[1], wait = b.wait;
  b.wait = async expression => {
    await wait(expression);
    if (expression.includes('>= 30000')) b.value.prepareElapsedMs = 29999;
  };
  await assert.rejects(live.executeScenario({variant: 'timeout'}, ports.devices));
});

// 显式中止仍走原四轮，不执行timeout专用的保持输入音频。
// 完整原轮后仍须真实wire窗口，缺失、错误或额外字段不能由页面停采事实代替。
test('timeout拒绝缺失或不完整的独立wire旁证', async () => {
  for (const witness of [undefined, async () => ({boundaryComplete:false,neverAccepted:true,neverPublished:true,sameOriginalHandoff:true}),
    async () => ({boundaryComplete:true,neverAccepted:true,neverPublished:true}),
    async () => ({boundaryComplete:true,neverAccepted:true,neverPublished:true,sameOriginalHandoff:true,unknown:true})]) {
    const ports = scenarioPorts('timeout');
    await assert.rejects(live.executeScenario({variant:'timeout'}, ports.devices, undefined, undefined, undefined, witness));
  }
});

// 退休采证使用完整轮之后当前facts；曾经关闭的快照不授予当前尚未关闭资源的资格。
test('timeout退休证据重新采当前候选而非复用旧已关闭快照', async () => {
  const ports = scenarioPorts('timeout'), witness = ports.timeoutCandidateWitness;
  const checkpoint = async () => {
    const wire = await witness();
    Object.assign(ports.devices[2].value, {terminated:false,tracksEnded:false,contextClosed:false,captureOpen:true});
    return wire;
  };
  const result = await live.executeScenario({variant:'timeout'}, ports.devices, undefined, undefined, undefined, checkpoint);
  assert.equal(result.timeoutRetirementBeforeCleanup.terminated, false);
  assert.equal(result.timeoutRetirementBeforeCleanup.tracksEnded, false);
  assert.equal(result.timeoutRetirementBeforeCleanup.contextClosed, false);
  const valid = fixture();
  assert.equal(live.expectedCandidateRetirement(valid.proof, valid.cleanup, valid.diagnostics), true);
  assert.equal(live.expectedCandidateRetirement({...valid.proof, ...result.timeoutRetirementBeforeCleanup}, valid.cleanup, valid.diagnostics), false);
});

test('abort不借timeout活动改变原四轮', async () => {
  const ports = scenarioPorts('abort'), result = await live.executeScenario({variant: 'abort'}, ports.devices);
  assert.equal(result.rounds.length, 4);
  assert.equal(ports.calls.some(item => item[1] === 'round' && item[3] === true), false);
});

// environment 在仓库.temp创建唯一有界语料替身，调用方finally精确回收，不触碰真实测试配置。
function environment() {
  const parent = path.join(process.cwd(), '.temp'); fs.mkdirSync(parent, {recursive: true});
  const directory = fs.mkdtempSync(path.join(parent, 'ai-endpoint-unit-'));
  const wave = path.join(directory, 'fixture.wav'), bytes = Buffer.alloc(48); bytes.write('RIFF'); fs.writeFileSync(wave, bytes);
  return {directory, env: {UR_AI_ENDPOINT_LIVE: '1', UR_AI_ENDPOINT_VARIANT: 'commit',
    UR_DEVICE_CONTROL_SYNC: '1', UR_MEDIA_TRANSPORT: 'udp', UR_AI_CONTROL_CHAIN: 'realtime_native',
    UR_BROWSER_PORT: '39881', UR_DEVICE_BROWSER_BIN: __filename, UR_BROWSER_INPUT_WAV: wave,
    UR_BROWSER_EXPECT_ASR_KEYWORDS: '合成语料', UR_DEVICE_PRODUCT: 'synthetic-product',
    UR_DEVICE_NAME: 'synthetic-A', UR_DEVICE_SECRET: 'synthetic-private-A', UR_MQTT_HOST: '127.0.0.1', UR_MQTT_PORT: '12345',
    UR_AI_ENDPOINT_B_PRODUCT: 'synthetic-product', UR_AI_ENDPOINT_B_NAME: 'synthetic-B',
    UR_AI_ENDPOINT_B_SECRET: 'synthetic-private-B', UR_AI_ENDPOINT_B_PORT: '39882',
    UR_AI_ENDPOINT_C_PRODUCT: 'synthetic-product', UR_AI_ENDPOINT_C_NAME: 'synthetic-C',
    UR_AI_ENDPOINT_C_SECRET: 'synthetic-private-C', UR_AI_ENDPOINT_C_PORT: '39883'}};
}
// focus 固定原成功事实，receiptIndex与revision独立，不能把任意UI阶段当作实际完成。
function focus(operation, index = 1) {
  return {state: operation === 'acquire' ? 'acquired' : 'released', operation,
    code: 200, revision: index, receiptIndex: index};
}
// fact 覆盖采集、播放、原输入和实际资源；标识只在断言内部使用。
function fact() { return {captureOpen: true, terminated: false, contextRunning: true,
  outputStopped: true, inputID: 'synthetic-input', starts: 1, stops: 0, captured: 3,
  completed: 0, voiced: 0, focusReceipts: [], sourceState: 'idle', targetPhase: null,
  tracksEnded: false, contextClosed: false}; }
test('三份已授权身份与端口各不重复，拒绝无显式许可、越界、None和不安全WebRTC替轨', () => {
  const f = environment();
  try {
    const settings = live.validateEnvironment(f.env);
    assert.equal(settings.devices.length, 3); assert.equal(settings.devices[0].env.UR_DEVICE_AI_ENDPOINT_LISTEN, undefined);
    assert.equal(settings.devices[1].env.UR_DEVICE_AI_ENDPOINT_LISTEN, '1');
    assert.equal(settings.devices[2].env.UR_DEVICE_CONTROL_SYNC, '1');
    for (const change of [{UR_AI_ENDPOINT_LIVE: '0'}, {UR_AI_ENDPOINT_VARIANT: 'unknown'},
      {UR_MEDIA_TRANSPORT: 'webrtc'}, {UR_DEVICE_CONTROL_SYNC: '0'}, {UR_DEVICE_AI_ENDPOINT_LISTEN: '1'},
      {UR_AI_ENDPOINT_B_PORT: '39881'}, {UR_AI_ENDPOINT_C_NAME: 'synthetic-B'},
      {UR_AI_ENDPOINT_B_SECRET: ''}, {UR_AI_ENDPOINT_B_NAME: 'bad:name'}, {UR_AI_ENDPOINT_C_PRODUCT: 'bad\nproduct'}])
      assert.throws(() => live.validateEnvironment({...f.env, ...change}));
  } finally { fs.rmSync(f.directory, {recursive: true}); }
});
test('新恢复variant须显式实际支持开关，只有B/C目标声明，旧variant环境不被重新解释',()=>{
  const f=environment();try{
    for(const variant of ['targetRecoveryActive','targetRecoveryStopped','targetRecoveryExpired']){
      assert.throws(()=>live.validateEnvironment({...f.env,UR_AI_ENDPOINT_VARIANT:variant}));
      const settings=live.validateEnvironment({...f.env,UR_AI_ENDPOINT_VARIANT:variant,UR_DEVICE_AI_ENDPOINT_CONTROL_SYNC:'1'});
      assert.equal(settings.devices[0].env.UR_DEVICE_AI_ENDPOINT_CONTROL_SYNC,undefined);assert.equal(settings.devices[1].env.UR_DEVICE_AI_ENDPOINT_CONTROL_SYNC,'1');assert.equal(settings.devices[2].env.UR_DEVICE_AI_ENDPOINT_CONTROL_SYNC,'1');
    }
    for(const variant of ['commit','abort','timeout'])assert.equal(live.validateEnvironment({...f.env,UR_AI_ENDPOINT_VARIANT:variant}).variant,variant);
  }finally{fs.rmSync(f.directory,{recursive:true});}
});
test('ready前原输入、真实采集增量与候选未播未采必须分别成立', () => {
  const b = fact(), a = {...b, captured: 4}, c = {...fact(), captureOpen: false};
  assert.equal(live.verifyPreserved(b, a, c).originalInputPreserved, true);
  assert.equal(live.verifyPreserved(b, a, {...c, captureOpen: null, terminated: true,
    tracksEnded: true, contextClosed: true}).candidateNotPublishing, true);
  for (const change of [{captureOpen: false}, {terminated: true}, {contextRunning: false},
    {inputID: 'replacement'}, {starts: 2}, {stops: 1}, {captured: 3}, {captured: Infinity}, {outputStopped: false}])
    assert.throws(() => live.verifyPreserved(b, {...a, ...change}, c));
  for (const change of [{captureOpen: true}, {captureOpen: null, terminated: true, tracksEnded: false},
    {captureOpen: null, terminated: true, tracksEnded: true, contextClosed: false},
    {outputStopped: false}, {completed: 1}, {voiced: 2}, {focusReceipts: [focus('acquire')]}])
    assert.throws(() => live.verifyPreserved(b, a, {...c, ...change}));
});
// 正式源ready才证明目标实际探测与上报，不能只认候选已拿接听ACK或过时UI阶段。
test('候选正式Preparing与原源ready共同证明held，不存在的phase5不是接续条件', () => {
  const source = {...fact(), sourceState: 'ready'};
  const candidate = {...fact(), captureOpen: false, targetPhase: 4};
  assert.deepEqual(live.verifyCandidateReady(source, candidate),
    {sourceReadyConfirmed: true, candidateHeldBeforeCommit: true});
  for (const sourceState of ['idle', 'pending', 'preparing', 'committed', 'aborted', 'failed', 'unknown'])
    assert.throws(() => live.verifyCandidateReady({...source, sourceState}, candidate));
  for (const targetPhase of [1, 2, 3, 5, 6, 7, 8, 9])
    assert.throws(() => live.verifyCandidateReady(source, {...candidate, targetPhase}));
});
test('late ready、终态和错误采播事实不能授权提交，接听ACK不代替真实media ready', () => {
  const source = {...fact(), sourceState: 'ready'};
  const candidate = {...fact(), captureOpen: false, targetPhase: 4};
  for (const change of [{terminated: true}, {contextRunning: false}])
    assert.throws(() => live.verifyCandidateReady({...source, ...change}, candidate));
  for (const change of [{terminated: true}, {contextRunning: false}, {captureOpen: true},
    {captureOpen: null}, {outputStopped: false}, {completed: 1}, {voiced: 1},
    {focusReceipts: [focus('acquire')]}])
    assert.throws(() => live.verifyCandidateReady(source, {...candidate, ...change}));
  assert.throws(() => live.verifyCandidateReady({...source, sourceState: 'preparing'}, candidate),
    '目标ACK已完成但平台未确认真实probe/媒体报告时仍不得继续');
});
test('完整组退场必须原actual release，sessionEnded、空麦克风和关闭意图不能冒充成功', () => {
  const before = {...fact(), focusReceipts: [focus('acquire')]};
  const source = {...before, terminated: true, contextClosed: true, tracksEnded: true, focusReceipts: [focus('acquire'), focus('release', 2)]};
  const target = {...fact(), captureOpen: false, targetPhase: 7, focusReceipts: [focus('acquire')]};
  assert.equal(live.verifyCommitted(before, source, target, true).sourceOriginalFullGroupReleaseConfirmed, true);
  assert.equal(live.verifyCommitted(fact(), {...source, focusReceipts: []}, target, false).sourceHadFullGroup, false);
  for (const change of [{tracksEnded: false}, {contextClosed: false}, {terminated: false},
    {focusReceipts: [focus('acquire')]}, {focusReceipts: [focus('release')]},
    {focusReceipts: [focus('acquire'), {...focus('release', 2), code: 409}]},
    {focusReceipts: [focus('acquire'), {...focus('release', 2), revision: 1}]}])
    assert.throws(() => live.verifyCommitted(before, {...source, ...change}, target, true));
  for (const change of [{targetPhase: 6}, {captureOpen: true}, {contextRunning: false}, {focusReceipts: []}])
    assert.throws(() => live.verifyCommitted(before, source, {...target, ...change}, true));
});
test('原回执按水位严格递增，错误取得或重放不能补造第二份实际释放', () => {
  assert.throws(() => live.successfulFocus({focusReceipts: [focus('acquire'), focus('release')]}, 'release'));
  assert.equal(live.successfulFocus({focusReceipts: [{...focus('acquire'), code: 409}]}, 'acquire').length, 0);
  assert.equal(live.successfulFocus({focusReceipts: [focus('release', 2)]}, 'release', 2).length, 0);
});
test('只读页面快照沿原轨ended与context.closed，不返回任何自由文本或会话身份', () => {
  const context = vm.createContext({aiEndpointSourceState: {state: 'committed', canPrepare: false},
    aiEndpointState: {phase: 7}, metrics: {focusReceipts: [focus('release', 2)]},
    stream: {getAudioTracks: () => [{readyState: 'ended'}]}, context: {state: 'closed'},
    performance: {now: () => 31000}, __urEndpointPrepareAt: 10,
    __urAIControl: {lastInput: 'synthetic-input', recognizedText: 'never-export-free-text'}});
  const result = vm.runInContext('(' + live.readEndpointFacts.toString() + ')()', context);
  assert.equal(result.tracksEnded, true); assert.equal(result.contextClosed, true);
  assert.equal(result.prepareElapsedMs, 30990); assert.equal(JSON.stringify(result).includes('never-export'), false);
});
test('失败投影保留实际采播和原资源回执数量，但禁止所有身份、自由状态和识别正文', () => {
  const value = {...fact(), sourceState: 'committed', targetPhase: 7,
    focusReceipts: [focus('acquire'), focus('release', 2)], device: 'private-device',
    recognizedText: 'private-text', sessionId: 'private-session', target: 'private-target'};
  const facts = live.failureFacts(value);
  assert.equal(facts.sourceState, 'committed'); assert.equal(facts.targetPhase, 7);
  assert.equal(facts.successfulAcquires, 1); assert.equal(facts.successfulReleases, 1);
  assert.equal(JSON.stringify(facts).includes('private'), false);
  assert.equal(JSON.stringify(facts).includes('synthetic-input'), false);
  assert.equal(live.failureFacts({...value, sourceState: 'private-state'}).sourceState, undefined);
});
test('失败投影保留原页面有限非负准备耗时，不补造未知或非法值', () => {
  for (const prepareElapsedMs of [0, 30990, 123.5])
    assert.equal(live.failureFacts({...fact(), prepareElapsedMs}).prepareElapsedMs, prepareElapsedMs);
  for (const prepareElapsedMs of [undefined, null, -1, NaN, Infinity, -Infinity, '12', true])
    assert.equal(Object.hasOwn(live.failureFacts({...fact(), prepareElapsedMs}), 'prepareElapsedMs'), false);
  assert.equal(Object.hasOwn(live.failureFacts(fact()), 'prepareElapsedMs'), false);
});

// scenarioPorts 模拟用户入口和观察端口；协议、媒体和实际设备从未由此模型实现。
function scenarioPorts(variant, corrupt = () => {}) {
  const calls = [];
  const devices = ['A', 'B', 'C'].map(role => {
    const value = {...fact(), captureOpen: false, targetPhase: role === 'A' ? null : 1};
    const port = {role, target: role, value,
      async facts() { value.captured++; return structuredClone(value); },
      async evaluate() {},
      async invoke(kind, selector, text) {
        calls.push([role, kind, selector]);
        if (kind === 'fill') port.destination = devices.find(item => item.target === text);
        if (selector === '#start') { value.captureOpen = true; value.starts++; }
        if (selector === '#endpointSourcePrepare') { value.sourceState = 'preparing'; port.destination.value.targetPhase = 2; }
        if (selector === '#endpointAccept') {
          value.targetPhase = 4; // 冻结C++正式接听及媒体ACK后仍为Preparing，不能伪造不存在的phase5。
          devices.find(item => item.destination === port).value.sourceState = 'ready';
        }
        if (selector === '#endpointSourceAbort') value.sourceState = 'aborted';
        if (selector === '#endpointSourceCommit') {
          value.terminated = value.tracksEnded = value.contextClosed = true; value.captureOpen = false;
          if (role === 'B') value.focusReceipts.push(focus('release', 2));
          Object.assign(port.destination.value, {targetPhase: 7, focusReceipts: [focus('acquire')]});
          corrupt(role, value, port.destination.value);
        }
      },
      async wait(expression) {
        if (expression.includes('>= 30000')) { value.sourceState = 'aborted'; value.prepareElapsedMs = 31000;
          Object.assign(devices[2].value, {terminated:true,tracksEnded:true,contextClosed:true,captureOpen:false,targetPhase:9}); }
        // 执行真实driver条件而非空wait：候选本地阶段不能被合成端口无条件等待掩盖。
        const observation = {metrics: value, aiEndpointSourceState: {state: value.sourceState},
          aiEndpointState: {phase: value.targetPhase}, terminated: value.terminated,
          element: () => ({disabled: false}), context: {state: value.contextClosed ? 'closed' : 'running'},
          stream: {getAudioTracks: () => [{readyState: value.tracksEnded ? 'ended' : 'live'}]},
          performance: {now: () => 31000}, __urEndpointPrepareAt: 0};
        assert.equal(vm.runInNewContext(expression, observation), true, '原driver等待条件应与冻结C++实际阶段一致');
      },
      async audioRound(active, preserveInput = false) { calls.push([role, 'round', active, preserveInput]);
        if (!preserveInput) { value.captureOpen = false; value.stops++; } value.completed++; value.voiced += 2;
        return {newReplyCompleted: true, actualVoicedFrames: 2}; }
    }; return port;
  });
  // timeoutCandidateWitness 仅纯夹具的完整窗口声明，真实入口必须由原wire checkpoint核定，不授予正式页面证明能力。
  const timeoutCandidateWitness = async () => {
    const source=devices[1].value,candidate=devices[2].value;
    assert.equal(source.sourceState,'aborted');assert.ok(source.prepareElapsedMs>=30000&&source.prepareElapsedMs<45000);
    assert.equal(candidate.terminated,true);assert.equal(candidate.tracksEnded,true);assert.equal(candidate.contextClosed,true);
    assert.equal(candidate.completed,0);assert.equal(candidate.voiced,0);assert.equal(candidate.focusReceipts.length,0);
    assert.equal(calls.some(item=>item[0]==='C'&&item[2]==='#endpointAccept'),false);
    return {boundaryComplete:true,neverAccepted:true,neverPublished:true,sameOriginalHandoff:true};
  };
  return {devices, calls, timeoutCandidateWitness};
}
for (const variant of ['commit', 'abort', 'timeout']) test('三C++编排' + variant + '必须先引导完整源，再验证原源与新候选', async () => {
  const ports = scenarioPorts(variant);
  const result = await live.executeScenario({variant}, ports.devices, undefined, undefined, undefined, ports.timeoutCandidateWitness);
  assert.equal(result.transfers.length, 2);
  assert.equal(result.transfers[0].sourceHadFullGroup, false);
  assert.equal(result.transfers[1].sourceHadFullGroup, true);
  assert.equal(result.rounds.length, variant === 'abort' ? 4 : 5);
  const secondPrepare = ports.calls.findIndex(item => item[0] === 'B' && item[2] === '#endpointSourcePrepare');
  const bRound = ports.calls.findIndex(item => item[0] === 'B' && item[1] === 'round');
  assert.ok(secondPrepare > bRound);
  if (variant === 'commit') assert.equal(result.transfers[1].sourceOriginalFullGroupReleaseConfirmed, true);
  else {
    assert.equal(result.transfers[1].originalFullGroupRetained, true);
    assert.equal(ports.calls.some(item => item[0] === 'B' && item[2] === '#endpointSourceCommit'), false);
    assert.equal(ports.calls.some(item => item[0] === 'C' && item[2] === '#endpointAccept'), false);
  }
});
for(const variant of ['targetRecoveryActive','targetRecoveryStopped','targetRecoveryExpired'])test('新'+variant+'仅在真实引导路径验完后移交专属hook，不运行旧第二跳',async()=>{
  const ports=scenarioPorts('commit'),journal={rounds:[],transfers:[]};let calls=0;
  await live.executeScenario({variant},ports.devices,()=>{},journal,async(settings,devices,_progress,result)=>{calls++;assert.equal(settings.variant,variant);assert.equal(devices,ports.devices);assert.equal(result,journal);assert.equal(result.rounds.length,3);assert.equal(result.transfers.length,1);assert.equal(devices[1].value.targetPhase,7);});
  assert.equal(calls,1);assert.equal(ports.calls.some(item=>item[0]==='B'&&item[2]==='#endpointSourcePrepare'),false);
});
test('原完整源缺释放即场景失败，已成功引导不能代替第二跳', async () => {
  const ports = scenarioPorts('commit', (role, source) => { if (role === 'B') source.focusReceipts.pop(); });
  const journal = {rounds: [], transfers: []};
  await assert.rejects(live.executeScenario({variant: 'commit'}, ports.devices, () => {}, journal));
  assert.equal(journal.rounds.length, 4); assert.equal(journal.transfers.length, 1);
});
// lifecyclePorts 只模拟native/浏览器进程接口，验证真实驱动的关闭与报告，绝不成为模型或HAL验收样本。
function lifecyclePorts(change = {}) {
  const devices = new Map(), reports = [], closures = [];
  return {devices, reports, closures, deps: {closeTimeoutMs: 1, report: value => reports.push(JSON.parse(value)),
    spawn(_binary, _args, options) {
      const role = options.env.UR_DEVICE_NAME.slice(-1), child = new EventEmitter();
      child.stdout = new EventEmitter(); child.stderr = new EventEmitter();
      const state = {...fact(), starts: 0, stops: 0, captured: 0, captureOpen: false,
        targetPhase: role === 'A' ? null : 1, fixtureEnded: 0, recognized: 0,
        network: 0, received: 0, completedID: '', playableID: '', recognizedSameReply: true, keywordMatched: true};
      const close = (code = 0) => {
        if (devices.get(role).closed) return;
        devices.get(role).closed = true;
        if (change.noStderr !== role) child.stderr.emit('end');
        child.emit('close', change.exitCodeRole === role ? 1 : code);
      };
      devices.set(role, {state, close, env: options.env}); child.kill = () => close(1);
      setImmediate(() => child.stdout.emit('data', Buffer.from('http://127.0.0.1:' + options.env.UR_BROWSER_PORT + '/')));
      return child;
    },
    execFile(_binary, args, _options, callback) {
      const role = args[1].split('-')[2], device = devices.get(role), value = device.state;
      const [kind, expression, text] = args.slice(2);
      if (kind === 'close') { closures.push(role); return callback(change.closeFailure === role ? Error('private-secret') : null, '{}'); }
      if (kind === 'fill') device.target = [...devices.values()].find(item =>
        'device:' + item.env.UR_DEVICE_PRODUCT + ':' + item.env.UR_DEVICE_NAME === text);
      if (kind === 'eval') {
        if (expression.startsWith('(async function readFacts') || expression.startsWith('(function readEndpointFacts')) {
          if (value.captureOpen) value.captured++;
          return callback(null, JSON.stringify(value));
        }
        if (expression === 'globalThis.__urControlPlay()') value.fixtureEnded++;
        if (expression.includes('element("end").click()')) device.close();
      }
      if (kind === 'click') {
        if (expression === '#start') { value.captureOpen = true; value.starts++; value.inputID = 'synthetic-' + role + '-' + value.starts; }
        if (expression === '#stop') {
          value.captureOpen = false; value.stops++; value.completed++; value.recognized++;
          value.completedID = value.playableID = 'synthetic-' + role + '-reply-' + value.completed;
          value.voiced += 2; value.network += 2;
          // 模拟完成的本轮必须关联原回复终态；此账本只验证驱动，不代表真实媒体已解码。
          const mediaFinalID = change.wrongFinalRole === role ? 'synthetic-old-reply' : value.completedID;
          value.completedReplyMediaFinal = mediaFinalID === value.completedID;
          value.decoded = value.network;
          value.missingPackets = change.truncatedRole === role ? 164 : 0;
          value.observedComplete = change.truncatedRole !== role;
        }
        if (expression === '#endpointSourcePrepare') { value.sourceState = 'preparing'; device.target.state.targetPhase = 2; }
        if (expression === '#endpointAccept') {
          value.targetPhase = 4; // 原实际候选Prepared，不用不存在的AwaitingCommit投影补造验证。
          [...devices.values()].find(item => item.target === device).state.sourceState = 'ready';
        }
        if (expression === '#endpointSourceCommit') {
          if (change.commitFailure === role) return callback(Error('private-secret'));
          value.terminated = value.tracksEnded = value.contextClosed = true; value.captureOpen = false;
          if (role === 'B') value.focusReceipts.push(focus('release', 2));
          Object.assign(device.target.state, {targetPhase: 7, focusReceipts: [focus('acquire')]}); device.close();
        }
      }
      callback(null, '{}');
    }
  }};
}
test('完整驱动五轮、两次接续和三进程正常释放缺一不可，身份与输入不进入报告', async () => {
  const f = environment(), ports = lifecyclePorts();
  try {
    const report = await live.run(f.env, ports.deps);
    assert.equal(report.result, 'pass'); assert.equal(report.rounds.length, 5); assert.equal(report.transfers.length, 2);
    assert.deepEqual(ports.closures, ['C', 'B', 'A']);
    assert.ok(report.cleanup.every(value => value.normalExit && value.stderrEnded));
    assert.equal(JSON.stringify(report).includes('synthetic-private'), false);
    assert.equal(JSON.stringify(report).includes('synthetic-product'), false);
    assert.equal(JSON.stringify(report).includes('synthetic-input'), false);
  } finally { fs.rmSync(f.directory, {recursive: true}); }
});
for (const change of [{truncatedRole: 'B'}, {wrongFinalRole: 'B'}])
  test('接续后的尾部有声或旧终态不构成完整回复 ' + JSON.stringify(change), async () => {
    const f = environment(), ports = lifecyclePorts(change);
    try {
      await assert.rejects(live.run(f.env, ports.deps));
      assert.equal(ports.reports[0].result, 'fail');
      assert.equal(ports.reports[0].phase, 'bootstrap_target_audio');
      assert.equal(ports.reports[0].rounds.length, 2);
      assert.equal(ports.reports[0].transfers.length, 1);
      assert.deepEqual(ports.closures, ['C', 'B', 'A']);
    } finally { fs.rmSync(f.directory, {recursive: true}); }
  });
for (const change of [{closeFailure: 'C'}, {noStderr: 'B'}, {exitCodeRole: 'A'}, {commitFailure: 'B'}])
  test('浏览器关闭、原生退出、stderr排空与第二跳失败不能被前轮成功掩盖 ' + JSON.stringify(change), async () => {
    const f = environment(), ports = lifecyclePorts(change);
    try {
      await assert.rejects(live.run(f.env, ports.deps), /ai_endpoint_live_failed/);
      const report = ports.reports[0]; assert.equal(report.result, 'fail');
      assert.equal(report.rounds.length, change.commitFailure ? 4 : 5);
      assert.equal(report.transfers.length, change.commitFailure ? 1 : 2);
      assert.equal(report.diagnostic.primary.reason, change.commitFailure ? 'browser_command_failed' : 'cleanup_failed');
      assert.equal(JSON.stringify(report).includes('private-secret'), false);
      assert.deepEqual(ports.closures, ['C', 'B', 'A']);
    } finally { fs.rmSync(f.directory, {recursive: true}); }
  });
test('原生启动失败仍精确退出并脱敏，不启动第二端或抹掉首因', async () => {
  const f = environment(), reports = [], children = [];
  try {
    await assert.rejects(live.run(f.env, {closeTimeoutMs: 1, report: value => reports.push(JSON.parse(value)),
      spawn() {
        const child = new EventEmitter(); child.stdout = new EventEmitter(); child.stderr = new EventEmitter();
        child.kill = () => { child.stderr.emit('end'); child.emit('close', 1); };
        children.push(child); setImmediate(() => child.emit('error', Error('synthetic-private-A'))); return child;
      },
      execFile(_bin, _args, _options, callback) { callback(null, '{}'); }
    }), /ai_endpoint_live_failed/);
    assert.equal(children.length, 1); assert.equal(reports.length, 1);
    assert.equal(reports[0].diagnostic.primary.phase, 'initialize_A');
    assert.equal(reports[0].diagnostic.primary.reason, 'device_spawn_failed');
    assert.equal(reports[0].cleanup[0].childClosed, true);
    assert.equal(JSON.stringify(reports).includes('synthetic-private'), false);
    assert.equal(JSON.stringify(reports).includes('synthetic-product'), false);
  } finally { fs.rmSync(f.directory, {recursive: true}); }
});

// captureAckFixture 仅构造闭集投影输入，不代表真实线程或模型现场。
function captureAckFixture() {
  const names = ['received', 'filterAccepted', 'filterRejected', 'sendAccepted', 'sendRejected',
    'sendException', 'ackPosted', 'ackRejected', 'ackException', 'socketUnavailable', 'socketBackpressure'];
  return {main: {...Object.fromEntries(names.map(k => [k, 0])), overflow: false},
    thread: {kind: 'captureAckDiagnostic', generated: 16, applied: 0, oldInputRejected: 0,
      ownerRejected: 0, highWater: 16, pending: 16, overflow: false, clock: 'valid',
      maximumAgeMilliseconds: null, oldestPendingAgeMilliseconds: 320}, threadRejected: false};
}

test('采集ACK驱动投影只接闭字段和同钟有界age，身份与未知字段拒绝', () => {
  const fact = captureAckFixture();
  assert.deepEqual(live.projectCaptureAckDiagnostic('null'), {available: false});
  assert.equal(live.projectCaptureAckDiagnostic(JSON.stringify(fact)).thread.pending, 16);
  assert.equal(live.projectCaptureAckDiagnostic(JSON.stringify(JSON.stringify(fact))).thread.oldestPendingAgeMilliseconds, 320);
  for (const bad of [{...fact, identity: 'private'}, {...fact, main: {...fact.main, token: 'private'}},
    {...fact, thread: {...fact.thread, sessionId: 'private'}},
    {...fact, thread: {...fact.thread, pending: 17}},
    {...fact, thread: {...fact.thread, clock: 'invalid'}},
    {...fact, thread: {...fact.thread, pending: 0}},
    {...fact, thread: {...fact.thread, oldestPendingAgeMilliseconds: 900001}}])
    assert.equal(live.projectCaptureAckDiagnostic(JSON.stringify(bad)), null);
  assert.equal(live.projectCaptureAckDiagnostic('x'.repeat(8193)), null);
});

test('采集ACK默认关闭不执行页面读取，也不改变原进程收尾', async () => {
  const f = environment(), ports = lifecyclePorts(); let reads = 0;
  const execute = ports.deps.execFile;
  ports.deps.execFile = (file, args, options, callback) => {
    if (args[3]?.includes('UrReadCaptureAckDiagnostic')) reads++;
    return execute(file, args, options, callback);
  };
  try {
    const report = await live.run(f.env, ports.deps);
    assert.equal(reads, 0); assert.equal(Object.hasOwn(report, 'captureAckDiagnostics'), false);
    assert.equal(report.result, 'pass'); assert.ok(report.cleanup.every(v => v.normalExit));
  } finally { fs.rmSync(f.directory, {recursive: true}); }
});

test('原三端cleanup前各只读一次，采证拒绝不改原业务结果或cleanup', async () => {
  for (const failure of ['none', 'malformed', 'commandFailed']) {
    const f = environment(), ports = lifecyclePorts(), calls = [];
    const execute = ports.deps.execFile;
    ports.deps.execFile = (file, args, options, callback) => {
      if (args[3]?.includes('UrReadCaptureAckDiagnostic')) {
        calls.push('read'); assert.equal(options.timeout, 3000); assert.equal(options.maxBuffer, 8192);
        if (failure === 'commandFailed') return callback(Error('controlled'));
        return callback(null, failure === 'malformed' ? '{"private":"unknown"}' : JSON.stringify(captureAckFixture()));
      }
      if (args[2] === 'close') calls.push('close');
      return execute(file, args, options, callback);
    };
    try {
      const report = await live.run({...f.env, UR_DEVICE_AUDIO_FAILURE_DIAGNOSTICS: '1'}, ports.deps);
      assert.deepEqual(calls.slice(0, 3), ['read', 'read', 'read']);
      assert.equal(calls.filter(v => v === 'read').length, 3);
      assert.equal(report.captureAckDiagnostics.length, 3);
      assert.ok(report.captureAckDiagnostics.every(v => v.probe ===
        (failure === 'none' ? 'observed' : failure === 'malformed' ? 'invalid' : 'commandFailed')));
      assert.equal(report.result, 'pass'); assert.ok(report.cleanup.every(v => v.normalExit));
      assert.equal(JSON.stringify(report.captureAckDiagnostics).includes('private'), false);
    } finally { fs.rmSync(f.directory, {recursive: true}); }
  }
});

// 以下10组只验证严格退休分类和原系统边界，不代替真实同scope wire/live证据。
const {createDevice,expectedCandidateRetirement}=live;
// 文件说明：纯分类正反边界；合成事实不代替下一真实业务的同scope与资源释放采证。

// fixture 只提供白名单合成事实，普通normalExit仍保false。
function fixture() {
  return {proof: {variant:'timeout', role:'C', scenarioComplete:true, sourceState:'aborted', prepareElapsedMs:30000,
    boundaryComplete:true, sameOriginalHandoff:true, neverAccepted:true, neverPublished:true, originalInputPreserved:true,
    originalFullGroupRetained:true, terminated:true, tracksEnded:true, contextClosed:true, outputStopped:true,
    completed:0, voiced:0, successfulAcquires:0}, cleanup:{browserClosed:true, childClosed:true, stderrEnded:true,
    forced:false, signal:null, exitCode:1, normalExit:false}, diagnostics:{overflow:false, malformed:false, stdoutEnded:true,
    scopeFailureObserved:false,stderrMalformed:false,stderrEnded:true,stderrRecords:[{kind:'runtime_failed',code:null}],
    runtimeRecords:[{kind:'runtimeFailureDiagnostic',runtime:'clientTick',udp:'none',pcm:'none',observation:1}],records:[{kind:'clientFailureDiagnostic',origin:'clientFailed',cause:'candidateEstablishDeadline',method:'none',observation:1}]}};
}
// realCleanup 用原createDevice实际close回调及原cleanup，不启动真实进程/浏览器。
async function realCleanup() {
  const child = new EventEmitter(); child.stdout = new PassThrough(); child.stderr = new PassThrough();
  const device = live.createDevice({role:'C',target:'device:synthetic:c',port:1,env:{}},{},
    {spawn:() => child, execFile:(bin,args,options,callback) => callback(null, '')});
  child.stdout.emit('data', Buffer.from('http://127.0.0.1:1/'));
  child.stderr.emit('end'); child.emit('close', 1, null);
  return device.cleanup();
}
test('原cleanup自然code1仍normalExitfalse，不被候选分类覆写', async () => {
  const original = await realCleanup(); assert.equal(original.normalExit,false);
  const value = fixture(); Object.assign(value.cleanup,original);
  assert.equal(expectedCandidateRetirement(value.proof,value.cleanup,value.diagnostics),true);
  assert.equal(value.cleanup.normalExit,false);
});
test('删除每个cleanup和diagnostics必需字段均拒绝', () => {
  for (const section of ['cleanup','diagnostics']) for (const key of Object.keys(fixture()[section])) {
    const value=fixture();delete value[section][key];
    assert.equal(expectedCandidateRetirement(value.proof,value.cleanup,value.diagnostics),false,section+'.'+key);
  }
});
test('缺任一必要真实布尔或计数仍失败', () => {
  for (const key of Object.keys(fixture().proof)) {
    const value = fixture(); delete value.proof[key];
    assert.equal(expectedCandidateRetirement(value.proof,value.cleanup,value.diagnostics),false,key);
  }
});
test('非本场景/设备及预算外不能借同类退出通过', () => {
  for (const [key,change] of [['variant','abort'],['variant','targetRecoveryExpired'],['role','B'],
    ['prepareElapsedMs',29999],['prepareElapsedMs',45000],['prepareElapsedMs',NaN],['prepareElapsedMs',Infinity],['sourceState','failed'],
    ['neverAccepted',false],['successfulAcquires',1],['completed',1],['voiced',1]]) {
    const value = fixture();value.proof[key]=change;
    assert.equal(expectedCandidateRetirement(value.proof,value.cleanup,value.diagnostics),false,key);
  }
});
test('信号/强制清理/缺自然退出事实不冒预期退休', () => {
  for (const [key,change] of [['signal','SIGTERM'],['signal',undefined],['forced',true],['exitCode',0],
    ['exitCode',null],['normalExit',true],['browserClosed',false],['childClosed',false],['stderrEnded',false]]) {
    const value=fixture();value.cleanup[key]=change;
    assert.equal(expectedCandidateRetirement(value.proof,value.cleanup,value.diagnostics),false,key);
  }
});
test('其它失败/多失败/截断/正文与缺诊断不得吞掉', () => {
  for (const cause of ['candidateClock','candidateRecoveryDeadline','requestRejected','unknown']) {
    const value=fixture();value.diagnostics.records[0].cause=cause;
    assert.equal(expectedCandidateRetirement(value.proof,value.cleanup,value.diagnostics),false);
  }
  for (const mutate of [value=>value.records.push({...value.records[0]}),value=>value.records=[],
    value=>value.records[0].payload='private-body',value=>value.overflow=true,value=>value.malformed=true,
    value=>value.stdoutEnded=false,value=>value.records=null,value=>value.records={},value=>value.records='private-body',value=>value.records=[null],value=>value.records=[undefined]]) {
    const value=fixture();mutate(value.diagnostics);
    assert.equal(expectedCandidateRetirement(value.proof,value.cleanup,value.diagnostics),false);
  }
});

test('其它runtime/UDP/PCM首因、scope失败与stderr未知均拒绝',()=>{
  for(const mutate of [d=>d.runtimeRecords[0].runtime='mediaPoll',d=>d.runtimeRecords[0].udp='replyReceive',d=>d.runtimeRecords[0].pcm='bufferFull',d=>d.runtimeRecords.push({...d.runtimeRecords[0]}),d=>d.runtimeRecords=[],d=>d.scopeFailureObserved=true,d=>d.stderrMalformed=true,d=>d.stderrEnded=false,d=>d.stderrRecords[0].kind='request_rejected',d=>d.stderrRecords.push({...d.stderrRecords[0]})]){const v=fixture();mutate(v.diagnostics);assert.equal(expectedCandidateRetirement(v.proof,v.cleanup,v.diagnostics),false);}
});
// 文件说明：使用候选原createDevice真实回调的纯生命周期正反用例；不启动设备、浏览器或后端。
const client={kind:'clientFailureDiagnostic',origin:'clientFailed',cause:'candidateEstablishDeadline',method:'none',observation:1}; // 原唯一客户端截止首因。
const runtime={kind:'runtimeFailureDiagnostic',runtime:'clientTick',udp:'none',pcm:'none',observation:1}; // 与原clientFailed一致的宿主包装首因，不许可额外UDP/PCM失败。
// proof 仅合成分类输入，真实同scope布尔必须由原wire严格验证器给出，不声称已实际实网。
function proof(){return {variant:'timeout',role:'C',scenarioComplete:true,sourceState:'aborted',prepareElapsedMs:30000,boundaryComplete:true,sameOriginalHandoff:true,neverAccepted:true,neverPublished:true,originalInputPreserved:true,originalFullGroupRetained:true,terminated:true,tracksEnded:true,contextClosed:true,outputStopped:true,completed:0,voiced:0,successfulAcquires:0};}
// natural 调原唯一stdout/stderr/close监听与实际cleanup分支；只替换execFile和spawn系统边界。
async function natural({code=1,signal=null,extraOut='',stderr='runtime_failed state=6 voice=6\n',endStdout=true}={}){
  const child=new EventEmitter();child.stdout=new PassThrough();child.stderr=new PassThrough();
  const device=createDevice({role:'C',target:'device:test:c',port:1,env:{}},{},{spawn:()=>child,execFile:(a,b,c,callback)=>callback(null,'')});
  child.stdout.emit('data',Buffer.from('http://127.0.0.1:1/\n'+ 'host_client_failure='+JSON.stringify(client)+'\n'+'host_failure='+JSON.stringify(runtime)+'\n'+extraOut));
  child.stderr.emit('data',Buffer.from(stderr));if(endStdout)child.stdout.emit('end');child.stderr.emit('end');child.emit('close',code,signal);
  return device.cleanup();
}
test('原自然code1仍normalExitfalse，仅完整原截止证据额外分类',async()=>{const clean=await natural();assert.equal(clean.normalExit,false);assert.equal(expectedCandidateRetirement(proof(),clean,clean.clientFailureDiagnostics),true);assert.equal(clean.normalExit,false);});
test('未接收完stdout或其它stderr/host/scope首因全部拒绝',async()=>{
  for(const args of [{endStdout:false},{stderr:'request_rejected method=audioStart code=500\n'},{stderr:'unknown private error\n'},
    {extraOut:'host_scope_failure={}\n'},{extraOut:'host_failure='+JSON.stringify({...runtime,runtime:'mediaPoll',udp:'replyReceive'})+'\n'},
    {extraOut:'host_failure=malformed private body\n'},{extraOut:'host_client_failure='+JSON.stringify(client)+'\n'}]){
    const clean=await natural(args);assert.equal(expectedCandidateRetirement(proof(),clean,clean.clientFailureDiagnostics),false);assert.equal(JSON.stringify(clean.clientFailureDiagnostics).includes('private'),false);
  }
});
test('信号、code0与非timeout或未完整实际释放不可冒候选退休',async()=>{
  for(const args of [{signal:'SIGTERM'},{code:0},{code:2}]){const clean=await natural(args);assert.equal(expectedCandidateRetirement(proof(),clean,clean.clientFailureDiagnostics),false);}
  const clean=await natural();for(const [key,value]of [['tracksEnded',false],['contextClosed',false],['boundaryComplete',false],['sameOriginalHandoff',false],['prepareElapsedMs',29999],['variant','abort']]){const p=proof();p[key]=value;assert.equal(expectedCandidateRetirement(p,clean,clean.clientFailureDiagnostics),false);}
});

// expiredRetirementFixture仅合成已由原wire核验的同scope事实；普通退出状态不被分类改写。
function expiredRetirementFixture() {
  return {proof:{variant:'targetRecoveryExpired',role:'B',scenarioComplete:true,expiredMilliseconds:16000,
    sameOriginalRecoveryScopeVerified:true,originalDeadlinePreserved:true,uniqueNetworkTimeout:true,
    noLateInputOrAcquire:true,noAutomaticCapture:true,reconnectAfterExpiryRejected:true,originalInputPreserved:true,
    originalEndpointPreserved:true,actualCaptureStopped:true,actualOutputStopped:true,terminated:true,
    tracksEnded:true,contextClosed:true,successfulAcquires:1},
    cleanup:{role:'B',browserClosed:true,childClosed:true,stderrEnded:true,forced:false,signal:null,exitCode:1,normalExit:false},
    diagnostics:{overflow:false,malformed:false,stdoutEnded:true,scopeFailureObserved:false,stderrMalformed:false,
      stderrEnded:true,records:[{kind:'clientFailureDiagnostic',origin:'clientFailed',cause:'candidateRecoveryDeadline',method:'none',observation:1}],
      runtimeRecords:[{kind:'runtimeFailureDiagnostic',runtime:'clientTick',udp:'none',pcm:'none',observation:1}],
      stderrRecords:[{kind:'runtime_failed',code:null}]}};
}
test('Expired只有原scope自然恢复截止可分类，normalExitfalse保持',()=>{
  const x=expiredRetirementFixture();assert.equal(live.expectedExpiredTargetRetirement(x.proof,x.cleanup,x.diagnostics),true);
  assert.equal(x.cleanup.normalExit,false);
  x.cleanup.exitCode=0;x.cleanup.normalExit=true;assert.equal(live.expectedExpiredTargetRetirement(x.proof,x.cleanup,x.diagnostics),false);
  x.proof.scenarioComplete=false;assert.equal(live.expectedExpiredTargetRetirement(x.proof,x.cleanup,x.diagnostics),false);
});
test('Expired全部必需旁证字段缺失均拒绝',()=>{
  for(const group of ['proof','cleanup','diagnostics'])for(const key of Object.keys(expiredRetirementFixture()[group])){
    const x=expiredRetirementFixture();delete x[group][key];assert.equal(live.expectedExpiredTargetRetirement(x.proof,x.cleanup,x.diagnostics),false,group+'.'+key);
  }
});
test('Expired错scope旧代次其它variant额外取得及早迟期限均拒绝',()=>{
  for(const [key,value] of [['variant','timeout'],['role','C'],['sameOriginalRecoveryScopeVerified',false],
    ['originalInputPreserved',false],['originalEndpointPreserved',false],['successfulAcquires',2],
    ['expiredMilliseconds',14999],['expiredMilliseconds',20000],['expiredMilliseconds',Infinity],
    ['uniqueNetworkTimeout',false],['noLateInputOrAcquire',false]]){
    const x=expiredRetirementFixture();x.proof[key]=value;assert.equal(live.expectedExpiredTargetRetirement(x.proof,x.cleanup,x.diagnostics),false,key);
  }
});
test('Expired信号强杀非code1及当前资源未关闭不能冒自然退休',()=>{
  for(const [group,key,value] of [['cleanup','signal','SIGTERM'],['cleanup','forced',true],['cleanup','exitCode',2],
    ['cleanup','childClosed',false],['cleanup','browserClosed',false],['proof','terminated',false],
    ['proof','tracksEnded',false],['proof','contextClosed',false],['proof','actualCaptureStopped',false],['proof','actualOutputStopped',false]]){
    const x=expiredRetirementFixture();x[group][key]=value;assert.equal(live.expectedExpiredTargetRetirement(x.proof,x.cleanup,x.diagnostics),false,key);
  }
});
test('Expired未知或额外首因UDP PCM及不完整诊断均拒绝',()=>{
  for(const change of [x=>x.diagnostics.records[0].cause='candidateEstablishDeadline',x=>x.diagnostics.records.push({...x.diagnostics.records[0]}),
    x=>x.diagnostics.stderrRecords[0].kind='unknown',x=>x.diagnostics.runtimeRecords[0].udp='replyReceive',
    x=>x.diagnostics.runtimeRecords[0].pcm='captureBacklogFull',x=>x.diagnostics.scopeFailureObserved=true,
    x=>x.diagnostics.stdoutEnded=false,x=>x.diagnostics.malformed=true,x=>x.diagnostics.records=null]){
    const x=expiredRetirementFixture();change(x);assert.equal(live.expectedExpiredTargetRetirement(x.proof,x.cleanup,x.diagnostics),false);
  }
});

// connectionDeadlineRetirementFixture复现独占Runtime的单钟恢复到期；完整scope proof仍复用原严格字段。
function connectionDeadlineRetirementFixture() {
  const x=expiredRetirementFixture();x.proof.expiredMilliseconds=15023.59999999404;
  x.diagnostics.records=[];x.diagnostics.runtimeRecords=[{kind:'runtimeFailureDiagnostic',runtime:'connectionDeadline',udp:'none',pcm:'none',observation:1}];
  x.cleanup.clientFailureDiagnostics=x.diagnostics;
  // other只构造真实正常退出且无首因的另外一端，不能以退出码替代诊断完整。
  const other=role=>({role,browserClosed:true,childClosed:true,stderrEnded:true,normalExit:true,exitCode:0,signal:null,forced:false,
    clientFailureDiagnostics:{records:[],runtimeRecords:[],stderrRecords:[],scopeFailureObserved:false,overflow:false,malformed:false,stderrMalformed:false,stdoutEnded:true,stderrEnded:true}});
  x.allCleanup=[other('A'),x.cleanup,other('C')];return x;
}
test('Expired同scope完整核证的离线15秒connectionDeadline允许自然退休而不改normalExit',()=>{
  const x=connectionDeadlineRetirementFixture();assert.equal(live.expectedExpiredTargetRetirement(x.proof,x.cleanup,x.diagnostics,x.allCleanup),true);assert.equal(x.cleanup.normalExit,false);
  assert.equal(live.expectedExpiredTargetRetirement(x.proof,x.cleanup,x.diagnostics),false);
});
test('离线截止不吞额外错误未知首因或UDP PCM故障',()=>{
  for(const change of [x=>x.diagnostics.records.push({kind:'clientFailureDiagnostic',origin:'clientFailed',cause:'unknown',method:'none',observation:1}),
    x=>x.diagnostics.runtimeRecords[0].runtime='clientTick',x=>x.diagnostics.runtimeRecords[0].udp='replyReceive',x=>x.diagnostics.runtimeRecords[0].pcm='captureBacklogFull',
    x=>x.diagnostics.runtimeRecords[0].observation=2,x=>x.diagnostics.runtimeRecords[0].extra=true,x=>x.diagnostics.runtimeRecords.push({...x.diagnostics.runtimeRecords[0]}),
    x=>x.diagnostics.stderrRecords.push({...x.diagnostics.stderrRecords[0]}),x=>x.diagnostics.stderrRecords[0].kind='unknown',x=>x.diagnostics.extra=true,
    x=>x.diagnostics.overflow=true,x=>x.diagnostics.malformed=true,x=>x.diagnostics.stderrMalformed=true,x=>x.diagnostics.scopeFailureObserved=true,
    x=>x.diagnostics.stdoutEnded=false,x=>x.diagnostics.stderrEnded=false]){
    const x=connectionDeadlineRetirementFixture();change(x);assert.equal(live.expectedExpiredTargetRetirement(x.proof,x.cleanup,x.diagnostics,x.allCleanup),false);
  }
});
test('离线截止仍要求全部原scope证明及15至20秒窗口，不接受任意runtime_failed',()=>{
  for(const key of Object.keys(connectionDeadlineRetirementFixture().proof)){
    const x=connectionDeadlineRetirementFixture();delete x.proof[key];assert.equal(live.expectedExpiredTargetRetirement(x.proof,x.cleanup,x.diagnostics,x.allCleanup),false,key);
  }
  for(const [key,value]of [['variant','timeout'],['role','A'],['expiredMilliseconds',14999],['expiredMilliseconds',20000],['expiredMilliseconds',Infinity],
    ['sameOriginalRecoveryScopeVerified',false],['originalDeadlinePreserved',false],['uniqueNetworkTimeout',false],['noLateInputOrAcquire',false],
    ['originalInputPreserved',false],['originalEndpointPreserved',false],['successfulAcquires',2],['tracksEnded',false],['contextClosed',false],['terminated',false]]){
    const x=connectionDeadlineRetirementFixture();x.proof[key]=value;assert.equal(live.expectedExpiredTargetRetirement(x.proof,x.cleanup,x.diagnostics,x.allCleanup),false,key);
  }
});
test('离线截止其他角色必须实际正常退出且完整无错误，缺失换组或混入额外首因拒绝',()=>{
  for(const change of [x=>x.allCleanup.pop(),x=>x.allCleanup.push({...x.allCleanup[0]}),x=>x.allCleanup[2].role='A',
    x=>x.allCleanup[1]={...x.cleanup},x=>x.allCleanup[0].exitCode=1,x=>x.allCleanup[0].signal='SIGTERM',x=>x.allCleanup[0].forced=true,
    x=>x.allCleanup[0].normalExit=false,x=>x.allCleanup[0].browserClosed=false,x=>x.allCleanup[0].childClosed=false,x=>x.allCleanup[0].stderrEnded=false,
    x=>x.allCleanup[0].clientFailureDiagnostics=null,x=>x.allCleanup[0].clientFailureDiagnostics.stderrRecords.push({kind:'runtime_failed',code:null}),
    x=>x.allCleanup[0].clientFailureDiagnostics.runtimeRecords.push({kind:'runtimeFailureDiagnostic',runtime:'unknown',udp:'none',pcm:'none',observation:1}),
    x=>x.allCleanup[0].clientFailureDiagnostics.records.push({kind:'clientFailureDiagnostic',origin:'clientFailed',cause:'unknown',method:'none',observation:1}),
    x=>x.allCleanup[0].clientFailureDiagnostics.overflow=true,x=>x.allCleanup[0].clientFailureDiagnostics.malformed=true,
    x=>x.allCleanup[0].clientFailureDiagnostics.stderrMalformed=true,x=>x.allCleanup[0].clientFailureDiagnostics.scopeFailureObserved=true,
    x=>x.allCleanup[0].clientFailureDiagnostics.stdoutEnded=false,x=>x.allCleanup[0].clientFailureDiagnostics.stderrEnded=false,
    x=>x.allCleanup[0].clientFailureDiagnostics.extra=true]){
    const x=connectionDeadlineRetirementFixture();change(x);assert.equal(live.expectedExpiredTargetRetirement(x.proof,x.cleanup,x.diagnostics,x.allCleanup),false);
  }
});

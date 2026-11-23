// Worklet纯逻辑回归：真实源码在隔离VM内执行，不把此模拟上下文称为浏览器媒体验收。
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/audio-worklet.js'), 'utf8');
const ownershipSource = fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/input-ownership.js'), 'utf8');

// ownershipDevice以显式伪原生类型测装配合同；不把VM类型或手动handleActual当实际浏览器证据。
function ownershipDevice() {
  const events = [], posted = [], context = {state: 'running'}, track = {readyState: 'live', enabled: true};
  class Stream { getAudioTracks() { return [track]; } }
  class Source { constructor(stream) { this.mediaStream = stream; this.context = context; } }
  class Worklet { constructor() { this.context = context; this.port = {postMessage: v => posted.push({...v})}; } }
  class Sender {
    constructor() { this.track = null; this.calls = []; this.waits = []; }
    replaceTrack(track) {
      this.calls.push(track);
      return new Promise((resolve, reject) => this.waits.push({resolve: () => { this.track = track; resolve(); }, reject}));
    }
  }
  class Peer { constructor(senders) { this.connectionState = 'connected'; this.senders = senders; }
    getSenders() { return this.senders; } }
  const stream = new Stream(), source = new Source(stream), worklet = new Worklet(), senders = [new Sender()];
  const peer = new Peer(senders), timers = new Map(); let time = 0, timerId = 0;
  const sandbox = {TextEncoder, MediaStream: Stream, MediaStreamAudioSourceNode: Source, AudioWorkletNode: Worklet,
    RTCRtpSender: Sender, RTCPeerConnection: Peer, performance: {now: () => time},
    setTimeout: (fn) => { timers.set(++timerId, fn); return timerId; }, clearTimeout: id => timers.delete(id)};
  vm.runInNewContext(ownershipSource, sandbox);
  const gate = new sandbox.UrInputOwnershipGate({stream, source, worklet, bootGeneration: '7',
    senders: () => senders, emit: v => events.push(JSON.parse(JSON.stringify(v)))});
  return {gate, track, source, context, worklet, senders, peer, events, posted, timers,
    setTime: value => { time = value; }};
}

test('Human取得必须原Worklet暂停实态后原Sender承诺完成，专用事实不能伪装Worklet', async () => {
  const r = ownershipDevice(), scope = {sessionId: 'human', routeGeneration: '11'};
  assert.equal(r.gate.bindRoute(scope, 'humanSender', {peer: r.peer, sender: r.senders[0]}), true);
  const request = {kind: 'inputOwnershipGate', ...scope, ticket: '1', enabled: true};
  assert.equal(r.gate.request(request), true);
  assert.equal(r.posted[0].enabled, false, 'Human先取得同scope/ticket的Worklet固定false中间事实');
  assert.equal(r.senders[0].calls.length, 0);
  assert.equal(r.gate.handleActual({...request, kind: 'inputOwnershipGated', enabled: false}), true);
  assert.equal(r.events.length, 0);
  assert.equal(r.senders[0].calls[0], r.track);
  r.senders[0].waits[0].resolve(); await new Promise(resolve => setImmediate(resolve));
  assert.deepEqual(r.events, [{kind: 'inputOwnershipSenderGated', ...scope, ticket: '1', enabled: true}]);
  assert.equal(r.gate.live(), true);
  assert.equal(r.gate.forwardControl({kind:'captureStart',...scope,id:'no-udp'}), false);
});

// settle只排空当前承诺回调，不靠长等待推进模拟时钟或伪造线程事实。
const settle = () => new Promise(resolve => setImmediate(resolve));

test('同一采集源UDP到Human再回UDP，必须两边原实态且不授第二路采集', async () => {
  const r = ownershipDevice(), ai = {sessionId:'ai',routeGeneration:'10'}, human = {sessionId:'human',routeGeneration:'11'};
  assert.equal(r.gate.bindRoute(ai, 'udp'), true);
  assert.equal(r.gate.bindRoute(human, 'humanSender', {peer:r.peer,sender:r.senders[0]}), true);
  const command = (scope, ticket, enabled) => ({kind:'inputOwnershipGate',...scope,ticket,enabled});
  const actual = (scope, ticket, enabled) => ({kind:'inputOwnershipGated',...scope,ticket,enabled});
  assert.equal(r.gate.request(command(ai,'1',true)), true); assert.equal(r.gate.handleActual(actual(ai,'1',true)), true);
  assert.equal(r.gate.request(command(human,'2',true)), false, '原UDP实际拥有输入时不能接管另一腿');
  assert.equal(r.gate.request(command(ai,'2',false)), true); assert.equal(r.gate.handleActual(actual(ai,'2',false)), true);
  assert.equal(r.gate.request(command(human,'3',true)), true); assert.equal(r.gate.handleActual(actual(human,'3',false)), true);
  assert.equal(r.gate.handleActual(actual(human,'3',false)), true); assert.equal(r.senders[0].calls.length, 1);
  r.senders[0].waits[0].resolve(); await settle();
  assert.equal(r.senders[0].track, r.track); assert.equal(r.gate.request(command(ai,'4',true)), false);
  assert.equal(r.gate.request(command(human,'4',false)), true); assert.equal(r.gate.handleActual(actual(human,'4',false)), true);
  assert.equal(r.events.length, 3, 'null承诺之前不得报告Human已释放');
  r.senders[0].waits[1].resolve(); await settle();
  assert.equal(r.senders[0].track, null); assert.equal(r.track.readyState, 'live');
  assert.equal(r.events.at(-1).kind, 'inputOwnershipSenderGated'); assert.equal(r.events.at(-1).enabled, false);
  assert.equal(r.gate.request(command(ai,'5',true)), true); assert.equal(r.gate.handleActual(actual(ai,'5',true)), true);
  assert.equal(r.gate.forwardCapture({kind:'captured',...ai}), true);
});

test('关闭和超时后迟到true承诺必须再精确null补偿，不冒released或恢复UDP', async () => {
  for (const timedOut of [false,true]) {
    const r=ownershipDevice(), scope={sessionId:'human',routeGeneration:'11'};
    assert.equal(r.gate.bindRoute(scope,'humanSender',{peer:r.peer,sender:r.senders[0]}),true);
    const original={kind:'inputOwnershipGate',...scope,ticket:'1',enabled:true};
    assert.equal(r.gate.request(original),true);
    assert.equal(r.gate.handleActual({...original,kind:'inputOwnershipGated',enabled:false}),true);
    if(timedOut){r.setTime(2000); [...r.timers.values()][0]();} else r.gate.close();
    assert.equal(r.gate.senderCleanupState,'pending');
    assert.equal(r.senders[0].calls[1],null);
    r.senders[0].waits[1].resolve(); await settle();
    assert.equal(r.gate.senderCleanupState,'pending','旧true未终态，较早null不能当released');
    r.senders[0].waits[0].resolve(); await settle();
    assert.equal(r.senders[0].calls[2],null,'迟到true后必须再次清原Sender');
    r.senders[0].waits[2].resolve(); await settle();
    assert.equal(r.senders[0].track,null); assert.equal(r.gate.senderCleanupState,'confirmed');
    assert.equal(r.events.length,0); assert.equal(r.track.readyState,'live');
    assert.equal(r.gate.request({...original,ticket:'2'}),false);
  }
});

test('原Peer归属变动与replace失败保持未知，不自动绑定另一Sender', async () => {
  const r=ownershipDevice(), scope={sessionId:'human',routeGeneration:'11'};
  assert.equal(r.gate.bindRoute(scope,'humanSender',{peer:r.peer,sender:{track:null}}),false);
  assert.equal(r.gate.bindRoute(scope,'humanSender',{peer:r.peer,sender:r.senders[0]}),true);
  const request={kind:'inputOwnershipGate',...scope,ticket:'1',enabled:true};
  r.peer.connectionState='disconnected';assert.equal(r.gate.request(request),false);
  r.peer.connectionState='connected'; assert.equal(r.gate.request(request),true);
  assert.equal(r.gate.handleActual({...request,kind:'inputOwnershipGated',enabled:false}),true);
  r.peer.senders=[]; r.senders[0].waits[0].resolve(); await settle();
  for(const pending of r.senders[0].waits.slice(1)) pending.resolve(); await settle();
  assert.equal(r.events.length,0); assert.equal(r.gate.closed,true); assert.equal(r.senders[0].track,null);
  const failed=ownershipDevice();assert.equal(failed.gate.bindRoute(scope,'humanSender',{peer:failed.peer,sender:failed.senders[0]}),true);
  assert.equal(failed.gate.request(request),true); assert.equal(failed.gate.handleActual({...request,kind:'inputOwnershipGated',enabled:false}),true);
  failed.senders[0].waits[0].reject(new Error('fixed_test_failure')); await settle();
  for(const pending of failed.senders[0].waits.slice(1)) pending.reject(new Error('fixed_test_failure')); await settle();
  assert.equal(failed.gate.senderCleanupState,'unknown'); assert.equal(failed.events.length,0);
});

test('Human媒体绑定只允许原peer和sender，不允许额外kind覆盖路由策略', () => {
  const r=ownershipDevice(), scope={sessionId:'human',routeGeneration:'11'};
  assert.equal(r.gate.bindRoute(scope,'humanSender',{peer:r.peer,sender:r.senders[0],kind:'udp'}),false);
  assert.equal(r.gate.bindRoute(scope,'humanSender',{peer:r.peer,sender:r.senders[0],sessionId:'other'}),false);
  assert.equal(r.gate.bindRoute(scope,'udp',{peer:r.peer,sender:r.senders[0]}),false);
});

test('合法候选可在Peer尚未连通时绑定，但取得必须回读原Peer实际connected', () => {
  const r=ownershipDevice(),scope={sessionId:'human',routeGeneration:'11'};
  r.peer.connectionState='new';
  assert.equal(r.gate.bindRoute(scope,'humanSender',{peer:r.peer,sender:r.senders[0]}),true);
  const request={kind:'inputOwnershipGate',...scope,ticket:'1',enabled:true};
  assert.equal(r.gate.request(request),false);assert.equal(r.posted.length,0);
  r.peer.connectionState='connected';assert.equal(r.gate.request(request),true);
});

test('原输入适配需要原节点和Sender事实，命令接纳不能提前授采集', () => {
  const r = ownershipDevice(), scope = {sessionId: 'ai', routeGeneration: '10'};
  assert.equal(r.gate.bindRoute(scope, 'udp'), true);
  const command = {kind: 'inputOwnershipGate', ...scope, ticket: '1', enabled: true};
  assert.equal(r.gate.request(command), true);
  assert.equal(r.gate.forwardControl({kind: 'captureStart', id: 'i', ...scope}), false);
  assert.equal(r.gate.handleActual({...command, kind: 'inputOwnershipGated', ticket: '2'}), false);
  assert.equal(r.gate.handleActual({...command, kind: 'inputOwnershipGated'}), true);
  assert.equal(r.gate.forwardControl({kind: 'captureStart', id: 'i'}), false, '不能给旧消息补scope');
  assert.equal(r.gate.forwardControl({kind: 'captureStart', id: 'i', ...scope}), true);
  assert.equal(r.gate.forwardCapture({kind: 'captured', ...scope}), true);
  assert.equal(r.gate.request({...command, ticket: '2', enabled: false}), true);
  assert.equal(r.gate.forwardCapture({kind: 'captured', ...scope}), false, '关闭入队立即阻断主线程旧帧');
  assert.equal(r.gate.handleActual({...command, kind: 'inputOwnershipGated', ticket: '2', enabled: false}), true);
  assert.equal(r.gate.handleActual({...command, kind: 'inputOwnershipGated'}), true);
  assert.equal(r.gate.forwardCapture({kind: 'captured', ...scope}), false, '旧actual不可重开旧路由');
  assert.equal(r.events.length, 2);
});

test('原输入适配未知Sender和断连不确认，Human缺原Peer保持拒绝', () => {
  const r = ownershipDevice(), scope = {sessionId: 'human', routeGeneration: '11'};
  assert.equal(r.gate.bindRoute(scope, 'humanSender'), false);
  assert.equal(r.gate.request({kind: 'inputOwnershipGate', ...scope, ticket: '1', enabled: true}), false);
  assert.equal(r.posted.length, 0);
  r.senders[0].track = r.track;
  assert.equal(r.gate.handleActual({kind: 'inputOwnershipIdle', bootGeneration: '7'}), false);
  r.senders[0].track = null;
  assert.equal(r.gate.handleActual({kind: 'inputOwnershipIdle', bootGeneration: '7'}), true);
  r.track.readyState = 'ended';
  assert.equal(r.gate.request({kind: 'inputOwnershipGate', ...scope, ticket: '1', enabled: false}), false);
  r.gate.close(); r.track.readyState = 'live'; assert.equal(r.gate.live(), false);
});

test('原输入适配严格类型与有界票据、pending不能被新请求取代', () => {
  const r = ownershipDevice(), scope = {sessionId: 'ai', routeGeneration: '10'};
  assert.equal(r.gate.bindRoute(scope, 'udp'), true);
  for (const bad of [{ticket: '0'}, {ticket: 1}, {routeGeneration: '9007199254740992'}, {enabled: null}, {private: true}])
    assert.equal(r.gate.request({kind: 'inputOwnershipGate', ...scope, ticket: '1', enabled: false, ...bad}), false);
  const original = {kind: 'inputOwnershipGate', ...scope, ticket: '1', enabled: false};
  assert.equal(r.gate.request(original), true);
  assert.equal(r.gate.request({...original, ticket: '2'}), false);
  assert.equal(r.gate.request(original), true); assert.equal(r.posted.length, 1);
  assert.equal(r.gate.handleActual({...original, kind: 'inputOwnershipGated'}), true);
  for (let n = 2; n <= 128; ++n) {
    const command = {...original, ticket: String(n)};
    assert.equal(r.gate.request(command), true); assert.equal(r.gate.handleActual({...command, kind: 'inputOwnershipGated'}), true);
  }
  assert.equal(r.gate.request({...original, ticket: '129'}), false);
});

test('输入Worklet未知scope与畸形票据不造实际完成，默认旧路径不需要scope', () => {
  const old = device();
  old.processor.command({kind: 'captureStart', id: 'legacy'});
  old.processor.process([[new Float32Array(320).fill(0.5)]], [[new Float32Array(320)]]);
  assert.equal(old.events[0].kind, 'captured');
  assert.equal(Object.hasOwn(old.events[0], 'sessionId'), false);
  for (const bad of [{ticket: '0'}, {ticket: '18446744073709551616'}, {sessionId: '\ud800'}, {routeGeneration: 1}, {private: true}]) {
    const r = device(16000, 20, false, {inputOwnership: true, ownershipBootGeneration: '7'});
    r.processor.command({kind: 'inputOwnershipGate', sessionId: 'ai', routeGeneration: '10', ticket: '1', enabled: false, ...bad});
    assert.equal(r.processor.failed, true);
    assert.deepEqual(r.events, [{kind: 'audioFailed'}]);
  }
  const r = device(16000, 20, false, {inputOwnership: true, ownershipBootGeneration: '7'});
  r.processor.command({kind: 'captureStart', id: 'legacy'});
  r.processor.process([[new Float32Array(320)]], [[new Float32Array(320)]]);
  assert.equal(r.processor.inputId, '');
  assert.equal(r.events.filter(v => v.kind === 'captured').length, 0);
});

test('借用输入适配不接纳仿造节点或跨腿实态，重复原回执不再次发送', () => {
  const r = ownershipDevice(), scope = {sessionId: 'ai', routeGeneration: '10'};
  r.source.mediaStream = {};
  assert.equal(r.gate.bindRoute(scope, 'udp'), false);
  const good = ownershipDevice();
  assert.equal(good.gate.bindRoute(scope, 'udp'), true);
  assert.equal(good.gate.request({kind: 'inputOwnershipGate', ...scope, ticket: '1', enabled: false}), true);
  const receipt = {kind: 'inputOwnershipGated', ...scope, ticket: '1', enabled: false};
  assert.equal(good.gate.handleActual({...receipt, sessionId: 'human'}), false);
  assert.equal(good.gate.handleActual({...receipt, routeGeneration: '11'}), false);
  assert.equal(good.gate.handleActual({...receipt, enabled: true}), false);
  assert.equal(good.gate.handleActual(receipt), true);
  assert.equal(good.gate.handleActual(receipt), true);
  assert.equal(good.events.length, 1);
});

test('输出门控只在真实渲染块确认，静音仍持续采集和消费旧回复', () => {
  const {processor: p, events} = device();
  p.command({kind: 'captureStart', id: 'i'});
  p.command({kind: 'playbackStart', epoch: '1'});
  p.command({kind: 'playbackFrame', epoch: '1', ticket: 1, samples: Array(320).fill(16384)});
  p.command({kind: 'playbackGate', ticket: '9007199254740993', enabled: false});
  assert.equal(p.failed, false);
  assert.equal(events.length, 0, '命令入队不能提前证明静音');
  p.process([], []);
  assert.equal(events.length, 0, '没有实际输出块不能确认门控');
  const output = new Float32Array(320).fill(1);
  p.process([[new Float32Array(320).fill(0.5)]], [[output]]);
  assert.ok(output.every(v => v === 0));
  assert.deepEqual(events.map(v => v.kind), ['captured', 'played', 'playbackGated']);
  assert.equal(events[0].samples[0], 16384, '扬声器门控不关闭或静音采集');
  assert.equal(events[1].nonSilent, false, '静音消费不得充当实际非静音输出');
  assert.deepEqual(events[2], {kind: 'playbackGated', ticket: '9007199254740993', enabled: false});
});

test('输出门控跨回复保持，旧票据不能恢复声音，恢复需新原票据', () => {
  const {processor: p, events} = device();
  p.command({kind: 'playbackGate', ticket: '1', enabled: false});
  p.process([], [[new Float32Array(128)]]);
  p.command({kind: 'playbackStart', epoch: '1'});
  p.command({kind: 'playbackFrame', epoch: '1', ticket: 1, samples: Array(320).fill(16384)});
  p.command({kind: 'playbackGate', ticket: '1', enabled: false});
  const muted = new Float32Array(320);
  p.process([], [[muted]]);
  assert.ok(muted.every(v => v === 0));
  p.command({kind: 'playbackStop', epoch: '1'});
  p.command({kind: 'playbackStart', epoch: '2'});
  p.command({kind: 'playbackFrame', epoch: '2', ticket: 1, samples: Array(320).fill(16384)});
  p.command({kind: 'playbackGate', ticket: '2', enabled: true});
  p.process([], [[new Float32Array(320)]]);
  assert.equal(events.filter(v => v.kind === 'played').at(-1).nonSilent, true);
  p.command({kind: 'playbackGate', ticket: '1', enabled: false});
  assert.equal(p.outputEnabled, true);
  assert.equal(events.filter(v => v.kind === 'playbackGated').length, 2);
});

test('门控畸形、错正文和重叠在途都明确失败，不发送已完成证明', () => {
  for (const value of [
    {kind: 'playbackGate', ticket: '0', enabled: false},
    {kind: 'playbackGate', ticket: '01', enabled: false},
    {kind: 'playbackGate', ticket: '18446744073709551616', enabled: false},
    {kind: 'playbackGate', ticket: 1, enabled: false},
    {kind: 'playbackGate', ticket: '1', enabled: null},
    {kind: 'playbackGate', ticket: '1', enabled: false, epoch: '1'},
  ]) {
    const {processor: p, events} = device();
    p.command(value);
    assert.equal(p.failed, true);
    assert.deepEqual(events, [{kind: 'audioFailed'}]);
  }
  for (const overlap of [false, true]) {
    const {processor: p, events} = device();
    p.command({kind: 'playbackGate', ticket: '1', enabled: false});
    p.command({kind: 'playbackGate', ticket: overlap ? '2' : '1', enabled: true});
    assert.equal(p.process([], [[new Float32Array(128)]]), false);
    assert.deepEqual(events, [{kind: 'audioFailed'}]);
  }
});

// device创建每例独立音频线程上下文；事件JSON复制避免跨VM原型影响断言。
function device(rate = 16000, ms = 20, observeCaptureState = false, ownership = {}) {
  const events = [];
  let Processor;
  class Port {
    constructor() { this.port = {postMessage: v => events.push(JSON.parse(JSON.stringify(v)))}; }
  }
  vm.runInNewContext(source, {AudioWorkletProcessor: Port, sampleRate: rate,
    registerProcessor: (_name, constructor) => { Processor = constructor; }});
  const processor = new Processor({processorOptions: {sampleRate: rate, frameMs: ms, observeCaptureState, ...ownership}});
  return {processor, events};
}

test('显式采集观察只在音频线程实际应用原输入启停后报告，旧输入不能伪造停止', () => {
  const {processor: p, events} = device(16000, 20, true);
  assert.deepEqual(events.splice(0), [{kind: 'captureState', id: '', enabled: false}]);
  p.command({kind: 'captureStart', id: 'original'});
  assert.deepEqual(events, [{kind: 'captureState', id: 'original', enabled: true}]);
  p.process([[new Float32Array(128).fill(0.5)]], [[new Float32Array(128)]]);
  p.command({kind: 'captureStop', id: 'other'});
  assert.equal(events.length, 1);
  assert.equal(p.inputId, 'original');
  p.command({kind: 'captureStop', id: 'original'});
  assert.deepEqual(events[1], {kind: 'captureState', id: 'original', enabled: false});
  assert.equal(p.inputId, '');
  assert.equal(p.capture.length, 0);
  p.process([[new Float32Array(320).fill(0.5)]], [[new Float32Array(320)]]);
  assert.equal(events.length, 2, '停止后不产生采集帧或新完成事实');
  p.command({kind: 'captureStop', id: 'original'});
  assert.equal(events.length, 2, '重复停止不补造实际确认');
});

// 以下新增ownership用例仍是受控VM；实际浏览器组件另保独立证据，不借此造物理许可。
test('输入所有权初始化只在原音频处理块确认boot空闲，不造session', () => {
  const {processor: p, events} = device(16000, 20, false,
    {inputOwnership: true, ownershipBootGeneration: '7'});
  assert.equal(events.length, 0);
  p.process([], []);
  assert.equal(events.length, 0);
  p.process([[new Float32Array(128)]], [[new Float32Array(128)]]);
  assert.deepEqual(events, [{kind: 'inputOwnershipIdle', bootGeneration: '7'}]);
});

test('输入所有权门控清旧帧且等待实际处理块，旧scope不能重开采集', () => {
  const {processor: p, events} = device(16000, 20, false,
    {inputOwnership: true, ownershipBootGeneration: '7'});
  p.process([[new Float32Array(128)]], [[new Float32Array(128)]]);
  events.length = 0;
  const scope = {sessionId: 'ai', routeGeneration: '10'};
  p.command({kind: 'inputOwnershipGate', ...scope, ticket: '1', enabled: true});
  assert.equal(p.failed, false);
  assert.equal(events.length, 0);
  p.process([[new Float32Array(128)]], [[new Float32Array(128)]]);
  assert.deepEqual(events.splice(0), [{kind: 'inputOwnershipGated', ...scope, ticket: '1', enabled: true}]);
  p.command({kind: 'captureStart', id: 'original', ...scope});
  p.process([[new Float32Array(128).fill(0.5)]], [[new Float32Array(128)]]);
  assert.equal(p.capture.length, 128);
  p.command({kind: 'inputOwnershipGate', ...scope, ticket: '2', enabled: false});
  assert.equal(p.capture.length, 0);
  assert.equal(p.capturePending, 0);
  assert.equal(events.length, 0);
  p.command({kind: 'captureStart', id: 'late', ...scope});
  p.process([[new Float32Array(320).fill(0.5)]], [[new Float32Array(320)]]);
  assert.deepEqual(events, [{kind: 'inputOwnershipGated', ...scope, ticket: '2', enabled: false}]);
  assert.equal(p.inputId, '');
  assert.equal(p.failed, false);
});

test('音频线程有效初始化即报告尚未开始输入，停止不存在的输入无需等待虚构回执', () => {
  const {processor: p, events} = device(16000, 20, true);
  assert.deepEqual(events, [{kind: 'captureState', id: '', enabled: false}]);
  p.command({kind: 'captureStop', id: 'not-started'});
  assert.equal(events.length, 1);
  p.process([[new Float32Array(320).fill(0.5)]], [[new Float32Array(320)]]);
  assert.equal(events.length, 1);
  const invalid = device(8000, 20, true);
  assert.deepEqual(invalid.events, [{kind: 'audioFailed'}]);
});

test('采集帧长与正负满幅正确，不跨输入拼接', () => {
  const {processor: p, events} = device();
  p.command({kind: 'captureStart', id: 'i'});
  p.process([[new Float32Array(128).fill(-1)]], [[new Float32Array(128)]]);
  assert.equal(events.length, 0);
  p.command({kind: 'captureStop', id: 'i'});
  p.command({kind: 'captureStart', id: 'j'});
  p.process([[new Float32Array(320).fill(1)]], [[new Float32Array(320)]]);
  assert.equal(events.length, 1);
  assert.equal(events[0].id, 'j');
  assert.deepEqual(events[0].samples, Array(320).fill(32767));
  p.process([[new Float32Array(320).fill(-1)]], []);
  assert.deepEqual(events[1].samples, Array(320).fill(-32768));
});

test('双向并发且取到帧不提前确认播放', () => {
  const {processor: p, events} = device();
  p.command({kind: 'captureStart', id: 'i'});
  p.command({kind: 'playbackStart', epoch: '9007199254740993'});
  p.command({kind: 'playbackFrame', epoch: p.epoch, ticket: 1, samples: Array(320).fill(16384)});
  const first = new Float32Array(128);
  p.process([[new Float32Array(128)]], [[first]]);
  assert.equal(events.length, 0);
  assert.equal(first[0], 0.5);
  p.process([[new Float32Array(192)]], [[new Float32Array(192)]]);
  assert.deepEqual(events.map(v => v.kind), ['captured', 'played']);
  assert.equal(events[1].epoch, '9007199254740993');
});

test('取消丢弃旧音频但不关闭采集', () => {
  const {processor: p, events} = device(24000, 60);
  p.command({kind: 'captureStart', id: 'i'});
  p.command({kind: 'playbackStart', epoch: '1'});
  p.command({kind: 'playbackFrame', epoch: '1', ticket: 1, samples: Array(1440).fill(100)});
  p.command({kind: 'playbackStop', epoch: '1'});
  p.command({kind: 'playbackStart', epoch: '2'});
  p.command({kind: 'playbackFrame', epoch: '1', ticket: 2, samples: Array(1440).fill(100)});
  const output = new Float32Array(1440).fill(1);
  p.process([[new Float32Array(1440)]], [[output]]);
  assert.ok(output.every(v => v === 0));
  assert.deepEqual(events.map(v => v.kind), ['captured']);
});

test('格式、帧序、容量、非法采样失败显式且不重复', () => {
  for (const mode of ['format', 'ticket', 'capacity', 'sample']) {
    const {processor: p, events} = device(mode === 'format' ? 44100 : 16000);
    if (mode === 'sample') {
      p.command({kind: 'captureStart', id: 'i'});
      p.process([[new Float32Array([NaN])]], []);
    } else if (mode !== 'format') {
      p.command({kind: 'playbackStart', epoch: '1'});
      for (let ticket = mode === 'ticket' ? 2 : 1; ticket <= 33; ++ticket)
        p.command({kind: 'playbackFrame', epoch: '1', ticket, samples: Array(320).fill(0)});
    }
    assert.equal(p.process([], [[new Float32Array(128)]]), false);
    assert.deepEqual(events, [{kind: 'audioFailed'}]);
  }
});

test('主线程未接纳的采集帧最多十六帧，确认释放容量且旧输入确认无效', () => {
  const {processor: p, events} = device();
  p.command({kind: 'captureStart', id: 'i'});
  for (let i = 0; i < 16; ++i) assert.equal(p.process([[new Float32Array(320)]], []), true);
  p.command({kind: 'captureAck', id: 'old'});
  assert.equal(p.capturePending, 16);
  p.command({kind: 'captureAck', id: 'i'});
  assert.equal(p.process([[new Float32Array(320)]], []), true);
  assert.equal(p.process([[new Float32Array(320)]], []), false);
  assert.equal(events.filter(v => v.kind === 'captured').length, 17);
  assert.equal(events.at(-1).kind, 'audioFailed');
});

// failureCase只施加原驱动边界；故障旁证不是实际FILE现场首因或浏览器媒体验收。
function failureCase(cause, apply, options = {}) {
  const r = device(options.rate ?? 16000, 20, false, {observeFailure: true, ...options});
  apply(r.processor);
  r.processor.command({kind: 'unknown'}); // 确定首因后不得被后续故障覆盖或重复发送。
  assert.equal(r.processor.process([], [[new Float32Array(128)]]), false);
  assert.deepEqual(r.events.filter(v => v.kind === 'audioFailureDiagnostic'),
    [{kind: 'audioFailureDiagnostic', cause}]);
  assert.deepEqual(r.events.filter(v => v.kind === 'audioFailed'), [{kind: 'audioFailed'}]);
  assert.equal(r.events.at(-1).kind, 'audioFailed');
  return r;
}

test('音频首因旁证默认关闭且只接受精确boolean，原失败消息仍严格单字段', () => {
  for (const observeFailure of [undefined, false, '1', 1, null]) {
    const r = device(16000, 20, false, {observeFailure});
    r.processor.command({kind: 'unknown'});
    assert.deepEqual(r.events, [{kind: 'audioFailed'}]);
  }
});

test('音频首因旁证配置、命令、采集边界均固定且只发一次', () => {
  failureCase('configurationInvalid', () => {}, {rate: 44100});
  failureCase('commandInvalid', p => p.command(null));
  failureCase('unknownCommand', p => p.command({kind: 'private-unrecognized', secret: 'private-payload'}));
  failureCase('captureStartInvalid', p => p.command({kind: 'captureStart', id: ''}));
  failureCase('captureStartInvalid', p => {
    p.command({kind: 'captureStart', id: 'private-input'});
    p.command({kind: 'captureStart', id: 'private-input'});
  });
  failureCase('zeroPendingAck', p => {
    p.command({kind: 'captureStart', id: 'private-input'});
    p.command({kind: 'captureAck', id: 'private-input'});
  });
  failureCase('captureSampleInvalid', p => {
    p.command({kind: 'captureStart', id: 'private-input'});
    p.process([[new Float32Array([NaN])]], [[new Float32Array(128)]]);
  });
  failureCase('captureBacklogFull', p => {
    p.command({kind: 'captureStart', id: 'private-input'});
    for (let i = 0; i < 17; ++i) p.process([[new Float32Array(320)]], [[new Float32Array(128)]]);
  });
});

test('音频首因旁证播放帧与门控保留原短路顺序及32帧上限', () => {
  failureCase('playbackStartInvalid', p => p.command({kind: 'playbackStart', epoch: '0'}));
  failureCase('playbackFrameInvalid', p => {
    p.command({kind: 'playbackStart', epoch: '1'});
    p.command({kind: 'playbackFrame', epoch: '1', ticket: 2, samples: []});
  });
  for (const badTicket of [false, true]) failureCase(badTicket ? 'playbackFrameInvalid' : 'playbackQueueFull', p => {
    p.command({kind: 'playbackStart', epoch: '1'});
    for (let ticket = 1; ticket <= 32; ++ticket)
      p.command({kind: 'playbackFrame', epoch: '1', ticket, samples: Array(320).fill(0)});
    p.command({kind: 'playbackFrame', epoch: '1', ticket: badTicket ? 34 : 33, samples: []});
  });
  for (const badTicket of [false, true]) failureCase(badTicket ? 'playbackFrameInvalid' : 'playbackQueueFull', p => {
    p.command({kind: 'playbackStart', epoch: '1'});
    for (let ticket = 1; ticket <= 32; ++ticket)
      p.command({kind: 'playbackFrame', epoch: '1', ticket, samples: Array(320).fill(0)});
    p.command({kind: 'playbackFrame', epoch: '1', ticket: badTicket ? 34 : 33,
      get samples() { assert.fail('原票据/容量前置失败时不得求值后置PCM'); }});
  });
  failureCase('playbackGateInvalid', p => p.command({kind: 'playbackGate', ticket: '1', enabled: false, secret: 'private-gate'}));
  failureCase('playbackGateConflict', p => {
    p.command({kind: 'playbackGate', ticket: '1', enabled: false});
    p.command({kind: 'playbackGate', ticket: '1', enabled: true});
  });
  failureCase('playbackGatePending', p => {
    p.command({kind: 'playbackGate', ticket: '1', enabled: false});
    p.command({kind: 'playbackGate', ticket: '2', enabled: true});
  });
});

test('音频首因旁证所有权校验不越过原前置短路也不采集身份正文', () => {
  const options = {inputOwnership: true, ownershipBootGeneration: '7'};
  // ownershipGate只提供原严格字段；scope与ticket不会出现在旁证对象中。
  const ownershipGate = (ticket, sessionId, enabled) =>
    ({kind: 'inputOwnershipGate', sessionId, routeGeneration: '1', ticket, enabled});
  failureCase('ownershipGateInvalid', p => p.command(ownershipGate('0', 'private-session', false)), options);
  failureCase('ownershipTicketConflict', p => {
    p.command(ownershipGate('1', 'private-session', false));
    p.command(ownershipGate('1', 'other-private', false));
  }, options);
  failureCase('ownershipGatePending', p => {
    p.command(ownershipGate('1', 'private-session', true));
    p.command(ownershipGate('2', 'other-private', true));
  }, options);
  failureCase('ownershipOwnerConflict', p => {
    p.command(ownershipGate('1', 'private-session', true));
    p.process([[new Float32Array(128)]], [[new Float32Array(128)]]);
    p.command(ownershipGate('2', 'other-private', true));
  }, options);
  const r = failureCase('ownershipRouteCapacity', p => {
    for (let n = 1; n <= 2; ++n) {
      p.command(ownershipGate(String(n), `private-session-${n}`, false));
      p.process([[new Float32Array(128)]], [[new Float32Array(128)]]);
    }
    p.command(ownershipGate('3', 'private-session-3', false));
  }, options);
  assert.equal(JSON.stringify(r.events.filter(v => ['audioFailureDiagnostic', 'audioFailed'].includes(v.kind))).includes('private'), false);
});

test('音频首因旁证发送异常不阻断原audioFailed，未知调用参数不输出自由值', () => {
  const r = device(16000, 20, false, {observeFailure: true});
  let attempts = 0;
  r.processor.port.postMessage = value => {
    if (value.kind === 'audioFailureDiagnostic') { ++attempts; throw Error('private diagnostic rejection'); }
    r.events.push(JSON.parse(JSON.stringify(value)));
  };
  assert.doesNotThrow(() => r.processor.command({kind: 'unknown'}));
  assert.equal(attempts, 1);
  assert.deepEqual(r.events, [{kind: 'audioFailed'}]);
  const other = device(16000, 20, false, {observeFailure: true});
  other.processor.fail('private exception text');
  assert.deepEqual(other.events, [{kind: 'audioFailureDiagnostic', cause: 'unknown'}, {kind: 'audioFailed'}]);
});

// 控制断连回归通过原Gate、原Worklet和原应用接线观察；节点为受控替身，不冒真实浏览器媒体验收。
const captureProjectionSources = {worklet: source, gate: ownershipSource,
  app: fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/app.js'), 'utf8')};
// fixture连接真实Gate/Worklet/原应用观察函数；手动process代表受控实际线程块而非真实声卡。
function captureProjectionFixture(observe = true) {
  const sources = captureProjectionSources;
  const posted = [], wire = [], events = [], timers = new Map();
  const context = {state: 'running'}, track = {readyState: 'live', enabled: true};
  class Stream { getAudioTracks() { return [track]; } }
  class Source { constructor(stream) { this.mediaStream = stream; this.context = context; } }
  class Worklet { constructor() { this.context = context; this.port = {postMessage: v => posted.push({...v})}; } }
  class Sender {}
  class Peer {}
  const sandbox = {TextEncoder, MediaStream: Stream, MediaStreamAudioSourceNode: Source,
    AudioWorkletNode: Worklet, RTCRtpSender: Sender, RTCPeerConnection: Peer,
    performance: {now: () => 0}, setTimeout: f => { timers.set(f, f); return f; }, clearTimeout: f => timers.delete(f),
    captureCommand: null, metrics: {captureOpen: null}, terminated: false,
    send: v => { wire.push(JSON.parse(JSON.stringify(v))); return true; }, inputOwnership: null,
    observeAudioFailure: false, release: () => { throw new Error('unexpectedRelease'); }};
  vm.createContext(sandbox); vm.runInContext(sources.gate, sandbox);
  const observerStart = sources.app.indexOf('// forwardInputOwnershipActual');
  const fallbackStart = sources.app.indexOf('// recordCaptureState 只观察');
  vm.runInContext(sources.app.slice(observerStart < 0 ? fallbackStart : observerStart,
    sources.app.indexOf('// recordAudioFailureDiagnostic')), sandbox);
  let Processor;
  class Port { constructor() { this.port = {postMessage: value => {
    const v = JSON.parse(JSON.stringify(value)); events.push(v);
    if (v.kind !== 'captured') sandbox.receiveAudioThread(v);
  }}; } }
  vm.runInNewContext(sources.worklet, {AudioWorkletProcessor: Port, sampleRate: 16000,
    registerProcessor: (_, p) => { Processor = p; }});
  const node = new Worklet(), stream = new Stream();
  sandbox.inputOwnership = new sandbox.UrInputOwnershipGate({stream, source: new Source(stream), worklet: node,
    bootGeneration: '7', senders: () => [], emit: sandbox.forwardInputOwnershipActual || sandbox.send});
  const p = new Processor({processorOptions: {sampleRate: 16000, frameMs: 20,
    observeCaptureState: observe, inputOwnership: true, ownershipBootGeneration: '7'}});
  const gate = sandbox.inputOwnership, scope = {sessionId: 'first', routeGeneration: '10'};
  assert.equal(gate.bindRoute(scope, 'udp'), true);
  // flush保留MessagePort FIFO，不能在门控命令入队时先伪造实际完成。
  const flush = () => { for (const v of posted.splice(0)) p.command(v); };
  const block = () => p.process([[new Float32Array(128).fill(0.5)]], [[new Float32Array(128)]]);
  const request = (ticket, enabled, s = scope) => {
    assert.equal(gate.request({kind: 'inputOwnershipGate', ...s, ticket, enabled}), true); flush();
  };
  const start = (id, s = scope) => {
    assert.equal(gate.forwardControl({kind: 'captureStart', id, ...s}), true);
    sandbox.captureCommand = {id, ...s, enabled: true}; flush();
  };
  block(); request('1', true); block(); start('original');
  return {sandbox, p, gate, scope, request, start, flush, block, events, wire, timers};
}

const captureProjectionCases = [['current_false_actual_projects_stop', () => {
    const r = captureProjectionFixture(); assert.equal(r.sandbox.metrics.captureOpen, true);
    r.request('2', false);
    assert.equal(r.p.inputId, ''); assert.equal(r.sandbox.metrics.captureOpen, true);
    assert.equal(r.gate.forwardControl({kind: 'captureStop', id: 'original', ...r.scope}), false);
    r.block(); assert.equal(r.sandbox.metrics.captureOpen, false);
    assert.equal(r.events.filter(v => v.kind === 'captureState' && v.id === 'original' && !v.enabled).length, 1);
    const actualIndex = r.events.findIndex(v => v.kind === 'inputOwnershipGated' && v.ticket === '2');
    assert.equal(r.events[actualIndex + 1].kind, 'captureState', '原单端口先接纳票据再投影停止实态');
    const frames = r.events.filter(v => v.kind === 'captured').length;
    for (let n = 0; n < 8; ++n) r.block();
    assert.equal(r.events.filter(v => v.kind === 'captured').length, frames);
  }], ['duplicate_old_false_cannot_stop_new_input', () => {
    const r = captureProjectionFixture(); r.request('2', false); r.block(); r.request('3', true); r.block(); r.start('new');
    const count = r.events.length;
    assert.equal(r.gate.handleActual({kind: 'inputOwnershipGated', ...r.scope, ticket: '2', enabled: false}), true);
    r.p.command({kind: 'inputOwnershipGate', ...r.scope, ticket: '2', enabled: false}); r.block();
    assert.equal(r.p.inputId, 'new'); assert.equal(r.sandbox.metrics.captureOpen, true); assert.equal(r.events.length, count);
    r.sandbox.recordCaptureState({kind: 'captureState', id: 'original', ...r.scope, enabled: false});
    assert.equal(r.sandbox.metrics.captureOpen, true);
    r.request('4', false); r.block(); r.request('5', true); r.block(); r.start('original');
    r.gate.handleActual({kind: 'inputOwnershipGated', ...r.scope, ticket: '4', enabled: false});
    r.sandbox.recordCaptureState({kind: 'captureState', id: 'original', ...r.scope, enabled: false});
    assert.equal(r.sandbox.metrics.captureOpen, true, '同id复用仍不能由旧false覆新true意图');
  }], ['other_scope_false_cannot_stop_owner', () => {
    const r = captureProjectionFixture(), other = {sessionId: 'other', routeGeneration: '11'};
    assert.equal(r.gate.bindRoute(other, 'udp'), true); r.request('2', false, other); r.block();
    assert.equal(r.p.inputId, 'original'); assert.equal(r.sandbox.metrics.captureOpen, true);
  }], ['late_false_cannot_overwrite_new_owner', () => {
    const r = captureProjectionFixture(), other = {sessionId: 'other', routeGeneration: '11'};
    assert.equal(r.gate.bindRoute(other, 'udp'), true); r.request('2', false); r.block();
    r.request('3', true, other); r.block(); r.start('new', other);
    r.gate.handleActual({kind: 'inputOwnershipGated', ...r.scope, ticket: '2', enabled: false});
    r.sandbox.recordCaptureState({kind: 'captureState', id: 'new', ...r.scope, enabled: false});
    assert.equal(r.sandbox.metrics.captureOpen, true); assert.equal(r.p.inputId, 'new');
  }], ['no_render_block_cannot_confirm_stop', () => {
    const r = captureProjectionFixture(); r.request('2', false);
    for (const [input, output] of [[[], []], [[], [[new Float32Array(128)]]],
      [[[new Float32Array(128)]], []]]) {
      r.p.process(input, output);
      assert.equal(r.sandbox.metrics.captureOpen, true); assert.ok(r.gate.pending);
    }
  }], ['late_true_cannot_reopen_completed_stop', () => {
    const r = captureProjectionFixture(); r.request('2', false); r.block();
    r.sandbox.recordCaptureState({kind: 'captureState', id: 'original', enabled: true});
    assert.equal(r.sandbox.metrics.captureOpen, false);
  }], ['observation_disabled_adds_no_capture_fact', () => {
    const r = captureProjectionFixture(false); r.request('2', false); r.block();
    assert.equal(r.events.filter(v => v.kind === 'captureState').length, 0); assert.equal(r.p.inputId, '');
  }]];

for (const [name, check] of captureProjectionCases) test(name, check);

// captureAckThread以原constructor/command/process执行正式脚本，并用同线程模拟currentTime。
// workletSourceForAck复用本文件已读正式线程源码，不引入TEMP依赖。
const workletSourceForAck = source;
// captureAckThread仅用受控单调钟核原线程帧与ACK，不作为真实声卡证明。
function captureAckThread(enabled, ownership = false, source = workletSourceForAck) {
  const events = [];
  const clock = {currentTime: 0};
  let Constructor;
  class Port { constructor() { this.port = {postMessage: v => events.push(JSON.parse(JSON.stringify(v)))}; } }
  vm.runInNewContext(source, {AudioWorkletProcessor: Port, sampleRate: 16000,
    get currentTime() { return clock.currentTime; }, registerProcessor: (_, value) => { Constructor = value; }});
  const p = new Constructor({processorOptions: {sampleRate: 16000, frameMs: 20,
    observeFailure: enabled, inputOwnership: ownership, ownershipBootGeneration: '7'}});
  const scope = {sessionId: 'scope', routeGeneration: '10'};
  if (ownership) {
    p.command({kind: 'inputOwnershipGate', ...scope, ticket: '1', enabled: true});
    p.process([[new Float32Array(128)]], [[new Float32Array(128)]]);
  }
  p.command({kind: 'captureStart', id: 'input', ...(ownership ? scope : {})});
  return {p, events, clock, scope,
    frame() { clock.currentTime += 0.02; return p.process([[new Float32Array(320)]], [[new Float32Array(320)]]); }};
}

test('旧id和错owner ACK只记拒绝，不消耗当前原pending', () => {
  const r = captureAckThread(true, true);
  r.frame();
  r.p.command({kind: 'captureAck', id: 'old', ...r.scope});
  r.p.command({kind: 'captureAck', id: 'input', ...r.scope, sessionId: 'other'});
  assert.equal(r.p.capturePending, 1);
  assert.equal(r.p.captureAckDiagnostic.oldInputRejected, 1);
  assert.equal(r.p.captureAckDiagnostic.ownerRejected, 1);
  r.p.command({kind: 'captureAck', id: 'input', ...r.scope});
  assert.equal(r.p.capturePending, 0);
  // 停止后旧未确认帧保留原pending语义，新Start才清FIFO；累计差值不能当当前pending。
  const restarted = captureAckThread(true);
  restarted.frame();
  restarted.p.command({kind: 'captureStop', id: 'input'});
  assert.equal(restarted.p.capturePending, 1);
  restarted.p.command({kind: 'captureAck', id: 'input'});
  assert.equal(restarted.p.capturePending, 1);
  restarted.p.command({kind: 'captureStart', id: 'new'});
  assert.equal(restarted.p.capturePending, 0);
  assert.equal(restarted.p.captureAckDiagnostic.times.length, 0);
  restarted.frame(); restarted.p.command({kind: 'captureAck', id: 'new'});
  const snapshot = restarted.p.captureAckSnapshot();
  assert.equal(snapshot.generated, 2); assert.equal(snapshot.applied, 1); assert.equal(snapshot.pending, 0);
  // 原所有权false门清原pending时同步清采证FIFO；别的owner拒绝不会清新输入。
  r.frame();
  r.p.command({kind: 'inputOwnershipGate', ...r.scope, ticket: '2', enabled: false});
  assert.equal(r.p.capturePending, 0); assert.equal(r.p.captureAckDiagnostic.times.length, 0);
  r.p.process([[new Float32Array(128)]], [[new Float32Array(128)]]);
  r.p.command({kind: 'inputOwnershipGate', ...r.scope, ticket: '3', enabled: true});
  r.p.process([[new Float32Array(128)]], [[new Float32Array(128)]]);
  r.p.command({kind: 'captureStart', id: 'new', ...r.scope}); r.frame();
  r.p.command({kind: 'captureAck', id: 'input', ...r.scope});
  assert.equal(r.p.capturePending, 1); assert.equal(r.p.captureAckDiagnostic.times.length, 1);
  r.p.command({kind: 'captureAck', id: 'new', ...r.scope});
  assert.equal(r.p.capturePending, 0); assert.equal(r.p.captureAckDiagnostic.times.length, 0);
});

test('同线程回拨或缺钟标坏旁证，计数耗尽不改变16保护', () => {
  const noPending = captureAckThread(true);
  assert.equal(noPending.p.captureAckSnapshot().oldestPendingAgeMilliseconds, null);
  noPending.frame();
  assert.equal(noPending.p.captureAckSnapshot().oldestPendingAgeMilliseconds, 0);
  noPending.clock.currentTime += 0.05;
  assert.ok(Math.abs(noPending.p.captureAckSnapshot().oldestPendingAgeMilliseconds - 50) < 0.000001);
  for (const bad of [-1, NaN]) {
    const r = captureAckThread(true); r.frame(); r.clock.currentTime = bad;
    r.p.command({kind: 'captureAck', id: 'input'});
    const snapshot = r.p.captureAckSnapshot();
    assert.equal(snapshot.clock, bad === -1 ? 'invalid' : 'unavailable');
    assert.equal(snapshot.maximumAgeMilliseconds, null);
    assert.equal(snapshot.oldestPendingAgeMilliseconds, null);
    assert.equal(r.p.capturePending, 0); assert.equal(r.p.failed, false);
  }
  const r = captureAckThread(true); r.p.captureAckDiagnostic.generated = 1000000;
  for (let n = 0; n < 17; n++) r.frame();
  assert.equal(r.p.failed, true); assert.equal(r.p.captureAckSnapshot().overflow, true);
  assert.equal(r.p.captureAckSnapshot().generated, 1000000);
});

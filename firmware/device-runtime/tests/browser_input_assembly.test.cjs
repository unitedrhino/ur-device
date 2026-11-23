// 文件说明：正式设备页输入门控装配回归；模拟HAL只验证分派和原scope，不代替真实Worklet/Sender验收。
'use strict';
const test = require('node:test'), assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/app.js'), 'utf8');

// 用户显式分享既有引用，页面不得自动产生授权或遗漏opt-in。
test('服务分享显式勾选原授权ID才随原HAL意图发送', () => {
  const p = page({UrHumanVoiceEnabled: true});
  p.incoming(handoffFact({canEscalate: true}));
  p.elements.get('handoffTargetKind').value = 'service';
  p.elements.get('handoffTarget').value = 'human-support';
  p.context.document.getElementById('handoffShareContext').checked = true;
  p.context.document.getElementById('handoffConsentId').value = 'owner-consent';
  p.elements.get('handoffEscalate').listeners.click();
  assert.deepEqual(JSON.parse(JSON.stringify(p.sent)), [{kind: 'handoffControl', operation: 'escalate',
    targetKind: 'service', targetId: 'human-support', shareContext: true, consentId: 'owner-consent'}]);
});
test('native临时通话只提交普通设备dial，不借永久escalate或服务分享', () => {
  const p = page({UrHumanVoiceEnabled: true, UrTemporaryCalls: true});
  p.incoming(handoffFact({canEscalate: true}));
  p.elements.get('handoffTargetKind').value = 'device';
  p.elements.get('handoffTarget').value = 'p:other';
  p.elements.get('handoffEscalate').listeners.click();
  assert.deepEqual(JSON.parse(JSON.stringify(p.sent)), [{kind: 'handoffControl', operation: 'dial',
    targetKind: 'device', targetId: 'p:other'}]);
  p.sent.length = 0;
  p.elements.get('handoffTargetKind').value = 'service';
  p.elements.get('handoffEscalate').listeners.click();
  assert.equal(p.sent.length, 0);
});
test('显式纯Human主动入口不创建AI，仅发送设备dial且仍等待C++可用投影', () => {
  const p = page({UrHumanVoiceEnabled: true, UrHumanReceiveOnly: true,
    UrHumanEndpointSource: true, UrHumanDialEnabled: true});
  assert.equal(p.elements.get('handoffOutbound').hidden, false);
  assert.equal(p.elements.get('handoffCancel').hidden, false);
  p.elements.get('handoffTargetKind').value = 'device';
  p.elements.get('handoffTarget').value = 'p:other';
  p.elements.get('handoffEscalate').listeners.click();
  assert.equal(p.sent.length, 0);
  p.incoming(handoffFact({canEscalate: true}));
  p.elements.get('handoffEscalate').listeners.click();
  assert.deepEqual(JSON.parse(JSON.stringify(p.sent)), [{kind: 'handoffControl', operation: 'dial',
    targetKind: 'device', targetId: 'p:other'}]);
  assert.equal(p.requests.length, 0);
  assert.equal(p.posted.length, 0);
  p.sent.length = 0;
  p.elements.get('handoffCancel').listeners.click();
  assert.equal(p.sent.length, 0); // 原创建未知时没有C++授权，按钮可见不等于可以取消。
  p.incoming(handoffFact({canCancel: true}));
  p.elements.get('handoffCancel').listeners.click();
  assert.deepEqual(JSON.parse(JSON.stringify(p.sent)), [{kind: 'handoffControl', operation: 'cancel'}]);
});
test('纯Human主动界面只接受完整原生组合，服务分享与多余开关不形成拨号权限', () => {
  for (const flags of [{UrHumanDialEnabled: true},
    {UrHumanVoiceEnabled: true, UrHumanReceiveOnly: true, UrHumanDialEnabled: true},
    {UrHumanReceiveOnly: true, UrHumanEndpointSource: true, UrHumanDialEnabled: true}]) {
    const p = page(flags);
    assert.equal(p.elements.get('handoffOutbound').hidden, flags.UrHumanReceiveOnly === true);
    assert.equal(p.sent.length, 0);
  }
  const p = page({UrHumanVoiceEnabled: true, UrHumanReceiveOnly: true,
    UrHumanEndpointSource: true, UrHumanDialEnabled: true});
  p.incoming(handoffFact({canEscalate: true}));
  p.elements.get('handoffTargetKind').value = 'service';
  p.elements.get('handoffTarget').value = 'p:other';
  p.elements.get('handoffEscalate').listeners.click();
  assert.equal(p.sent.length, 0);
  p.elements.get('handoffTargetKind').value = 'device';
  p.context.document.getElementById('handoffShareContext').checked = true;
  p.elements.get('handoffEscalate').listeners.click();
  assert.equal(p.sent.length, 0);
});
// coordinateClickFixture 模拟坐标落点而非直接调业务handler；只验证正式页面事件装配，不冒称真实CDP命中。
function coordinateClickFixture(p, rect, viewport, covered = false) {
  const button = p.elements.get('handoffEscalate');
  button.getBoundingClientRect = () => ({...rect});
  const overlay = {}; // 测试内覆盖层，无正文、身份或第三方页面。
  p.context.document.elementFromPoint = (x, y) =>
    x >= 0 && x < viewport.width && y >= 0 && y < viewport.height ? (covered ? overlay : button) : null;
  const box = button.getBoundingClientRect(), x = box.x + box.width / 2, y = box.y + box.height / 2;
  const centerInViewport = x >= 0 && x < viewport.width && y >= 0 && y < viewport.height;
  const hitSelf = p.context.document.elementFromPoint(x, y) === button;
  const dispatched = centerInViewport && hitSelf && button.disabled === false;
  if (dispatched) button.listeners.click(); // 仅模拟浏览器真正派到启用控件时调用原app注册的监听器。
  return {centerInViewport, hitSelf, dispatched};
}
test('坐标点击离屏或被覆盖时CLI成功不能替代正式app实际dial投递', () => {
  const p = page({UrHumanVoiceEnabled: true, UrTemporaryCalls: true});
  p.incoming(handoffFact({canEscalate: true}));
  p.elements.get('handoffTargetKind').value = 'device';
  p.elements.get('handoffTarget').value = 'p:other';
  assert.equal(p.elements.get('handoffEscalate').disabled, false);
  const viewport = {width: 800, height: 600};
  assert.deepEqual(coordinateClickFixture(p, {x: 10, y: 700, width: 100, height: 40}, viewport),
    {centerInViewport: false, hitSelf: false, dispatched: false});
  assert.equal(p.sent.length, 0);
  assert.deepEqual(coordinateClickFixture(p, {x: 10, y: 100, width: 100, height: 40}, viewport, true),
    {centerInViewport: true, hitSelf: false, dispatched: false});
  assert.equal(p.sent.length, 0);
  assert.deepEqual(coordinateClickFixture(p, {x: 10, y: 100, width: 100, height: 40}, viewport),
    {centerInViewport: true, hitSelf: true, dispatched: true});
  assert.deepEqual(JSON.parse(JSON.stringify(p.sent)), [{kind: 'handoffControl', operation: 'dial',
    targetKind: 'device', targetId: 'p:other'}]);
});
test('前置控件启用不能跨release授权坐标点击，终态清状态而非改发escalate', () => {
  const p = page({UrHumanVoiceEnabled: true, UrTemporaryCalls: true});
  p.incoming(handoffFact({canEscalate: true}));
  p.elements.get('handoffTargetKind').value = 'device';
  p.elements.get('handoffTarget').value = 'p:other';
  assert.equal(p.elements.get('handoffEscalate').disabled, false);
  vm.runInContext('release("fixture_closed")', p.context);
  assert.equal(vm.runInContext('terminated && handoffState === undefined', p.context), true);
  assert.deepEqual(coordinateClickFixture(p, {x: 10, y: 100, width: 100, height: 40}, {width: 800, height: 600}),
    {centerInViewport: true, hitSelf: true, dispatched: false});
  assert.equal(p.sent.length, 0);
});
test('临时通话显式恢复只发新start，本机资格不授予采集或伪造云ID', () => {
  const p = page({UrHumanVoiceEnabled: true, UrTemporaryCalls: true});
  const button = p.context.document.getElementById('temporaryResumeAI');
  p.incoming({kind: 'temporaryCallState', canResumeAI: false, restoring: false});
  assert.equal(button.disabled, true);
  p.incoming({kind: 'temporaryCallState', canResumeAI: true, restoring: false});
  assert.equal(button.disabled, false);
  button.listeners.click(); button.listeners.click();
  assert.deepEqual(JSON.parse(JSON.stringify(p.sent)), [{kind: 'start'}]);
  assert.equal(p.posted.length, 0);
  assert.equal(p.requests.length, 0);
  assert.equal(button.disabled, true);
  p.incoming({kind: 'temporaryCallState', canResumeAI: false, restoring: true});
  assert.equal(button.disabled, true);
});
test('临时通话资格拒未知字段与未装配宿主，不改变原按钮', () => {
  const p = page({UrHumanVoiceEnabled: true, UrTemporaryCalls: true});
  for (const change of [{canResumeAI: 1}, {sessionId: 'borrowed'}, {restoring: 'true'},
    {canResumeAI: true, restoring: true}])
    assert.throws(() => p.incoming({kind: 'temporaryCallState', canResumeAI: false, restoring: false, ...change}));
  for (const values of [{}, {UrTemporaryCalls: true}, {UrHumanVoiceEnabled: true}])
    assert.throws(() => page(values).incoming({kind: 'temporaryCallState', canResumeAI: true, restoring: false}));
});
// page借固定HAL假端口观察正式页面，不安装第二onmessage或申请真实采集源。
function page(values = {}) {
  const sent = [], posted = [], bound = [], requests = [], controlled = [];
  let owner = false;
  const elements = new Map();
  const scope = {sessionId: 'original-ai', routeGeneration: '7'};
  const gate = {
    bindRoute(value, kind, media) { bound.push({value: {...value}, kind, ...(media !== undefined ? {media} : {})}); return true; },
    request(value) { requests.push({...value}); return true; },
    forwardCapture(value) { return owner && value.sessionId === scope.sessionId && value.routeGeneration === scope.routeGeneration; },
    forwardControl(value) { controlled.push({...value}); return owner && value.sessionId === scope.sessionId && value.routeGeneration === scope.routeGeneration; },
    handleActual(value) { if (value.kind !== 'inputOwnershipGated') return false; owner = value.enabled; sent.push({...value}); return true; },
    close() { owner = false; }, // 模拟HAL的关闭仅清原fixture所有权，不提供实际媒体释放证明。
  };
  const context = vm.createContext({...values, TextEncoder, URLSearchParams, gate, sent, posted,
    document: {getElementById(id) { if (!elements.has(id)) elements.set(id, {textContent: '', value: '', listeners: {},
      addEventListener(kind, callback) { this.listeners[kind] = callback; }}); return elements.get(id); }},
    window: {addEventListener() {}},
  });
  vm.runInContext(source, context);
  // 输入节点真实存在于index.html；VM按需创建其DOM节点，不要求业务脚本初始化时提前读取用户目标。
  context.document.getElementById('handoffTargetKind');
  context.document.getElementById('handoffTarget');
  vm.runInContext('inputOwnership = gate; worklet = {port:{postMessage: value => posted.push({...value}),close(){}},disconnect(){}}; send = value => {sent.push({...value});return true;}', context);
  return {scope, gate, sent, posted, bound, requests, controlled,
    context, elements,
    incoming: value => context.incoming(value),
    actual: value => context.receiveAudioThread(value),
    own: value => { owner = value; },
    metrics: () => JSON.parse(vm.runInContext('JSON.stringify(metrics)', context)),
  };
}
// originalInputPage 加载真实门控类和原socket.onmessage；浏览器节点是有类型的合成端口，不冒充实际媒体。
function originalInputPage(values = {}) {
  const p = page(values), sdk = fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/input-ownership.js'), 'utf8');
  Object.assign(p.context, {setTimeout, clearTimeout, performance: {now: () => 100}});
  vm.runInContext(`
    globalThis.MediaStream = class { getAudioTracks(){return this.tracks;} getTracks(){return this.tracks;} };
    globalThis.MediaStreamAudioSourceNode = class { disconnect(){} };
    globalThis.AudioWorkletNode = class { disconnect(){} };
    globalThis.RTCRtpSender = class { constructor(){this.track=null;} replaceTrack(track){this.track=track;return Promise.resolve();} };
    globalThis.RTCPeerConnection = class { constructor(){this.connectionState='connected';this.sender=new RTCRtpSender();}getSenders(){return [this.sender];}close(){this.connectionState='closed';} };
    context={state:'running',close:async()=>{}};stream=new MediaStream();stream.tracks=[{enabled:true,readyState:'live',stop(){this.readyState='ended';}}];
    source=new MediaStreamAudioSourceNode();source.mediaStream=stream;source.context=context;
    worklet=new AudioWorkletNode();worklet.context=context;worklet.port={postMessage:value=>posted.push({...value}),close(){}};
    globalThis.originalPeer=new RTCPeerConnection();globalThis.socketCloses=0;
    socket={close(){socketCloses++;},send(){}};
  `, p.context);
  vm.runInContext(sdk, p.context);
  vm.runInContext(`inputOwnership=new UrInputOwnershipGate({stream,source,worklet,bootGeneration:'7',senders:()=>originalPeer.getSenders(),emit:value=>send(value)});
    duplexHumanMedia={inputBinding:()=>({peer:originalPeer,sender:originalPeer.sender}),close(){}};`, p.context);
  const start = source.indexOf('    socket.onmessage = event => {'), end = source.indexOf('\n    };', start);
  assert.ok(start >= 0 && end > start);
  vm.runInContext(source.slice(start, end + '\n    };'.length), p.context);
  return {...p, deliver: value => vm.runInContext('socket.onmessage', p.context)({data: JSON.stringify(value)}),
    terminated: () => vm.runInContext('terminated', p.context),
    closeCount: () => p.context.socketCloses,
    gateActual: value => vm.runInContext('inputOwnership', p.context).handleActual(value),
    gateClose: () => vm.runInContext('inputOwnership', p.context).close()};
}
test('原页面真实门控缺Bind时拒绝CPP首票据并沿原catch关闭socket', async () => {
  const p = originalInputPage();
  p.deliver({kind: 'inputOwnershipGate', sessionId: 'original-ai', routeGeneration: '1', ticket: '1', enabled: false});
  assert.equal(p.terminated(), true);assert.equal(p.closeCount(), 1);assert.equal(p.posted.length, 0);assert.equal(p.sent.length, 0);
  await p.gateClose();
});
test('原页面先Bind后UDP两阶段票据只在实际Worklet事实之后确认', async () => {
  const p = originalInputPage(), scope = {sessionId: 'ai"\\scope', routeGeneration: '1'};
  p.deliver({kind: 'inputOwnershipBind', ...scope, routeKind: 'udp'});
  assert.equal(p.sent.length, 0);assert.equal(p.posted.length, 0);assert.equal(p.terminated(), false);
  for (const [ticket, enabled] of [['1', false], ['2', true]]) {
    p.deliver({kind: 'inputOwnershipGate', ...scope, ticket, enabled});
    assert.equal(p.terminated(), false);assert.equal(p.posted.at(-1).sessionId, scope.sessionId);
    const before = p.sent.length;
    assert.equal(p.gateActual({kind: 'inputOwnershipGated', ...scope, ticket, enabled}), true);
    assert.equal(p.sent.length, before + 1);
  }
  assert.equal(p.closeCount(), 0);await p.gateClose();
});
test('原人工Bind借同Peer和Sender，Worklet屏障后才回原Sender事实', async () => {
  const p = originalInputPage(), scope = {sessionId: 'original-human', routeGeneration: '2'};
  p.deliver({kind: 'inputOwnershipBind', ...scope, routeKind: 'humanSender'});
  assert.equal(p.sent.length, 0);
  p.deliver({kind: 'inputOwnershipGate', ...scope, ticket: '1', enabled: true});
  assert.equal(p.terminated(), false);assert.equal(p.posted.at(-1).enabled, false);assert.equal(p.sent.length, 0);
  assert.equal(p.gateActual({kind: 'inputOwnershipGated', ...scope, ticket: '1', enabled: false}), true);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(p.sent.at(-1).kind, 'inputOwnershipSenderGated');assert.equal(p.sent.at(-1).enabled, true);
  assert.equal(p.closeCount(), 0);await p.gateClose();
});
// 目标AI先取得整组但不开始输入；同原Sender的传输必须同时具备焦点和新的原采集命令。
test('WebRTC AI候选Focus绑定仍静音，原新输入命令才开同Sender', async () => {
  const p = originalInputPage({UrAIEndpointListen: true, location: {search: '?media=webrtc'}});
  vm.runInContext('rtcPeer=originalPeer;rtcEndpointSender=originalPeer.sender;rtcPeerRevision=1n;rtcWasConnected=true;', p.context);
  const scope = {sessionId: 'original-ai', routeGeneration: '1'};
  p.deliver({kind: 'inputOwnershipBind', ...scope, routeKind: 'aiSender'});
  assert.equal(p.terminated(), false);
  p.context.__scope = scope;
  assert.equal(vm.runInContext('inputOwnership.forwardSenderControl({kind:"captureStart",id:"old-input",...__scope})', p.context), false);
  assert.equal(vm.runInContext('originalPeer.sender.track', p.context), null);
  p.deliver({kind: 'inputOwnershipGate', ...scope, ticket: '1', enabled: true});
  assert.equal(p.posted.at(-1).enabled, false);
  assert.equal(p.gateActual({kind: 'inputOwnershipGated', ...scope, ticket: '1', enabled: false}), true);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(p.sent.at(-1).kind, 'inputOwnershipSenderGated');
  assert.equal(vm.runInContext('originalPeer.sender.track===stream.getAudioTracks()[0]', p.context), true);
  assert.equal(vm.runInContext('originalPeer.sender.track.enabled', p.context), false);
  assert.equal(vm.runInContext('inputOwnership.forwardSenderControl({kind:"captureStart",id:"wrong-scope",sessionId:"other",routeGeneration:"1"})', p.context), false);
  p.deliver({kind: 'captureStart', id: 'fresh-input', peerRevision: '1', ...scope});
  assert.equal(p.terminated(), false);
  assert.equal(vm.runInContext('originalPeer.sender.track.enabled', p.context), true);
  p.deliver({kind: 'captureStop', id: 'fresh-input', peerRevision: '1', ...scope});
  assert.equal(vm.runInContext('originalPeer.sender.track.enabled', p.context), false);
  p.deliver({kind: 'inputOwnershipGate', ...scope, ticket: '2', enabled: false});
  p.deliver({kind: 'captureStop', id: 'fresh-input', peerRevision: '1', ...scope});
  assert.equal(p.terminated(), false); // 原focus false在途的旧停采不能使宿主先于实际回执退出。
  assert.equal(p.gateActual({kind: 'inputOwnershipGated', ...scope, ticket: '2', enabled: false}), true);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(vm.runInContext('originalPeer.sender.track', p.context), null);
  assert.equal(p.sent.at(-1).enabled, false);
  assert.equal(p.terminated(), false);
  await p.gateClose();
});
// 已断网络不能取得新许可，但原Sender在仍未close的Peer上可实际null，不能因此卡死资源释放。
test('AI Sender断网只允许原false清理，关闭先静音再等待实际null', async () => {
  const p = originalInputPage({UrAIEndpointListen: true, location: {search: '?media=webrtc'}});
  vm.runInContext('rtcPeer=originalPeer;rtcEndpointSender=originalPeer.sender;rtcPeerRevision=1n;rtcWasConnected=true;', p.context);
  const scope = {sessionId: 'original-ai', routeGeneration: '1'};
  p.deliver({kind: 'inputOwnershipBind', ...scope, routeKind: 'aiSender'});
  p.deliver({kind: 'inputOwnershipGate', ...scope, ticket: '1', enabled: true});
  assert.equal(p.gateActual({kind: 'inputOwnershipGated', ...scope, ticket: '1', enabled: false}), true);
  await new Promise(resolve => setImmediate(resolve));
  p.deliver({kind: 'captureStart', id: 'fresh-input', peerRevision: '1', ...scope});
  vm.runInContext("originalPeer.connectionState='disconnected'", p.context);
  p.context.__scope = scope;
  assert.equal(vm.runInContext('inputOwnership.request({kind:"inputOwnershipGate",...__scope,ticket:"2",enabled:true})', p.context), false);
  p.deliver({kind: 'inputOwnershipGate', ...scope, ticket: '2', enabled: false});
  assert.equal(p.gateActual({kind: 'inputOwnershipGated', ...scope, ticket: '2', enabled: false}), true);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(vm.runInContext('originalPeer.sender.track', p.context), null);
  assert.equal(vm.runInContext('stream.getAudioTracks()[0].enabled', p.context), false);
  assert.equal(p.sent.at(-1).enabled, false);
  assert.equal(p.terminated(), false);
  await p.gateClose();
});
// source-only或普通WebRTC页面不能借AI Sender命名绕过自己的能力与原本机装配。
test('普通WebRTC页拒绝AI候选Sender绑定而不扩大原Human契约', async () => {
  const p = originalInputPage({location: {search: '?media=webrtc'}});
  vm.runInContext('rtcPeer=originalPeer;rtcPeerRevision=1n;rtcWasConnected=true;', p.context);
  p.deliver({kind: 'inputOwnershipBind', sessionId: 'original-ai', routeGeneration: '1', routeKind: 'aiSender'});
  assert.equal(p.terminated(), true);
  assert.equal(p.sent.length, 0);
  await p.gateClose();
});
test('重复Bind与非法原scope仍拒绝，不因新装配放宽普通页', async () => {
  for (const id of ['ordinary', '\ud800']) {
    const p = originalInputPage(), value = {kind: 'inputOwnershipBind', sessionId: id, routeGeneration: '1', routeKind: 'udp'};
    p.deliver(value);if (id === 'ordinary') p.deliver(value);
    assert.equal(p.terminated(), true);assert.equal(p.closeCount(), 1);assert.equal(p.sent.length, 0);await p.gateClose();
  }
  const ordinary = page();vm.runInContext('inputOwnership=undefined;', ordinary.context);
  assert.throws(() => ordinary.incoming({kind: 'inputOwnershipBind', sessionId: 'ai', routeGeneration: '1', routeKind: 'udp'}), /invalid_input_binding/);
});
// frame只使用原PCM320帧与原scope，迟到用例明确换原session而非贴当前owner。
// controlFact仅作本地投影夹具，不制造后台ACK或实际媒体事实。
function controlFact(change = {}) {
  return {kind: 'humanControlState', canHold: false, canResume: false, held: false,
    controlState: 'idle', ...change};
}
test('人工控制独立闭集与pending未知禁操作，旧七字段挂断不被替换', () => {
  const p = page({UrHumanVoiceEnabled: true});
  p.incoming(controlFact({canHold: true}));
  p.elements.get('handoffHold').listeners.click();
  assert.deepEqual(JSON.parse(JSON.stringify(p.sent)), [{kind: 'handoffControl', operation: 'hold'}]);
  p.incoming(controlFact({controlState: 'pending'}));
  p.elements.get('handoffHold').listeners.click();
  p.elements.get('handoffResume').listeners.click();
  assert.equal(p.sent.length, 1);
  p.incoming(controlFact({canResume: true, held: true, controlState: 'completed'}));
  p.elements.get('handoffResume').listeners.click();
  assert.equal(p.sent.at(-1).operation, 'resume');
  p.incoming(controlFact({held: true, controlState: 'unknown'}));
  p.incoming(handoffFact({state: 8, canHangup: true}));
  assert.equal(p.elements.get('handoffHangup').disabled, false);
  for (const change of [{canHold: true, held: true}, {canResume: true},
    {controlState: 'pending', canHold: true}, {sessionId: 'untrusted'}, {canHold: 'true'}]) {
    assert.throws(() => p.incoming(controlFact(change)), /invalid_human_control_state/);
  }
});
test('人工完成回执有界留内存，ACK200单独不算实际完成，重复水位拒绝', () => {
  const p = page({UrHumanVoiceEnabled: true});
  const receipt = index => ({kind: 'humanControlReceipt', operation: 'hold', controlToken: 'original-control',
    state: 'completed', reason: '', revision: 1, code: 200, actualApplied: true, actualHeld: true, receiptIndex: index});
  for (let index = 1; index <= 35; index++) p.incoming(receipt(index));
  assert.equal(p.metrics().humanControlReceipts.length, 32);
  assert.equal(p.metrics().humanControlReceiptIndex, 35);
  for (const change of [{actualApplied: false}, {code: 0}, {actualHeld: false},
    {receiptIndex: 35}, {receiptIndex: 513}, {text: 'private'}, {reason: 'free text'}]) {
    assert.throws(() => p.incoming({...receipt(36), ...change}), /invalid_human_control_receipt/);
  }
  assert.equal(p.metrics().humanControlReceiptIndex, 35);
});
test('分享勾选不授本人权限，空引用device目标拒绝，缺省忽略残留引用', () => {
  const p = page({UrHumanVoiceEnabled: true});
  p.incoming(handoffFact({canEscalate: true}));
  const checkbox = p.context.document.getElementById('handoffShareContext');
  const reference = p.context.document.getElementById('handoffConsentId');
  p.elements.get('handoffTarget').value = 'human-support';
  checkbox.checked = true;
  p.elements.get('handoffTargetKind').value = 'device';
  reference.value = 'owner-consent';
  p.elements.get('handoffEscalate').listeners.click();
  p.elements.get('handoffTargetKind').value = 'service';
  for (const id of ['', 'x'.repeat(129), 'bad\nref', '\ud800']) {
    reference.value = id;
    p.elements.get('handoffEscalate').listeners.click();
  }
  assert.equal(p.sent.length, 0);
  reference.value = 'owner-consent';
  checkbox.checked = false;
  p.elements.get('handoffEscalate').listeners.click();
  assert.deepEqual(JSON.parse(JSON.stringify(p.sent)), [{kind: 'handoffControl', operation: 'escalate',
    targetKind: 'service', targetId: 'human-support'}]);
});
function frame(scope) { return {kind: 'captured', ...scope, id: 'original-input', samples: Array(320).fill(128)}; }

test('正式页面只向原端口绑定可信UDP关联，绑定不产生取得事实', () => {
  const p = page();
  p.incoming({kind: 'inputOwnershipBind', routeKind: 'udp', ...p.scope});
  assert.deepEqual(p.bound, [{value: p.scope, kind: 'udp'}]);
  assert.equal(p.sent.length, 0);
  assert.equal(p.posted.length, 0);
  assert.throws(() => p.incoming({kind: 'inputOwnershipBind', routeKind: 'udp', ...p.scope, deviceSecret: 'invalid'}));
});

// handoffFact模拟原C++的固定七字段投影；它不能替代资源实际取得或平台提交。
function handoffFact(overrides = {}) {
  return {kind: 'handoffState', state: 0, canEscalate: false, canAccept: false,
    canReject: false, canCancel: false, canHangup: false, ...overrides};
}
test('共享宿主控制界面仅显式启用，初始不能凭资产存在授予任何按钮', () => {
  const old = page();
  assert.equal(old.elements.get('handoffControls').hidden, true);
  assert.throws(() => old.incoming(handoffFact()), /invalid_handoff_state/);
  const shared = page({UrHumanVoiceEnabled: true});
  assert.equal(shared.elements.get('handoffControls').hidden, false);
  for (const id of ['handoffEscalate', 'handoffAccept', 'handoffReject', 'handoffCancel', 'handoffHangup'])
    assert.equal(shared.elements.get(id).disabled, true);
});
test('纯人工原生文案标识不启用共享媒体或授予接听操作', () => {
  const p = page({UrHumanReceiveOnly: true});
  assert.equal(p.elements.get('handoffControls').hidden, true);
  assert.throws(() => p.incoming(handoffFact()), /invalid_handoff_state/);
  assert.equal(p.sent.length, 0);
  assert.equal(p.posted.length, 0);
});
test('纯人工原生入口展示来电状态而非影子AI会话', () => {
  const p = page({UrHumanVoiceEnabled: true, UrHumanReceiveOnly: true});
  assert.equal(p.elements.get('handoffOutbound').hidden, true);
  assert.equal(p.elements.get('handoffCancel').hidden, true);
  p.incoming(handoffFact());
  assert.equal(p.elements.get('handoffStatus').textContent, '等待语音来电');
  p.incoming(handoffFact({state: 2, canAccept: true, canReject: true}));
  assert.equal(p.elements.get('handoffStatus').textContent, '语音来电处理中');
  assert.equal(p.elements.get('handoffAccept').disabled, false);
  assert.equal(p.sent.length, 0);
  assert.equal(p.requests.length, 0);
});
test('纯人工取得资源文案不能冒充平台接通或本地许可', () => {
  const p = page({UrHumanVoiceEnabled: true, UrHumanReceiveOnly: true});
  p.incoming(handoffFact({state: 6}));
  assert.equal(p.elements.get('handoffStatus').textContent, '语音资源已取得，等待平台建立');
  assert.equal(p.elements.get('handoffHangup').disabled, true);
  p.incoming(handoffFact({state: 8, canHangup: true}));
  assert.equal(p.elements.get('handoffStatus').textContent, '人工语音通话');
  p.elements.get('handoffHangup').listeners.click();
  assert.deepEqual(JSON.parse(JSON.stringify(p.sent)), [{kind: 'handoffControl', operation: 'hangup'}]);
  assert.equal(p.requests.length, 0);
  assert.equal(p.posted.length, 0);
});
test('严格七字段投影只更新界面，不产生输入租约、媒体命令或ACK', () => {
  const p = page({UrHumanVoiceEnabled: true});
  p.incoming(handoffFact({state: 2, canAccept: true, canReject: true}));
  assert.equal(p.elements.get('handoffAccept').disabled, false);
  assert.equal(p.elements.get('handoffReject').disabled, false);
  assert.equal(p.elements.get('handoffEscalate').disabled, true);
  assert.equal(p.sent.length, 0);
  assert.equal(p.requests.length, 0);
  assert.equal(p.posted.length, 0);
});
test('共享投影拒绝身份泄漏、缺失字段和非法枚举，不改变原按钮', () => {
  const p = page({UrHumanVoiceEnabled: true});
  p.incoming(handoffFact({canEscalate: true}));
  for (const change of [{sessionId: 'private'}, {focusId: 'private'}, {targetId: 'private'},
    {state: -1}, {state: 11}, {state: 0.5}, {state: 'HumanActive'}, {canEscalate: 1},
    {canAccept: undefined}, {kind: 'handoffReceipt'}])
    assert.throws(() => p.context.recordHandoffState(handoffFact(change)), /invalid_handoff_state/);
  assert.equal(p.elements.get('handoffEscalate').disabled, false);
  assert.equal(p.sent.length, 0);
});
test('本地明确转人工只发送四字段目标，其余控制只发送两字段意图', () => {
  const p = page({UrHumanVoiceEnabled: true});
  p.elements.get('handoffTargetKind').value = 'device';
  p.elements.get('handoffTarget').value = 'explicit-device';
  p.elements.get('handoffEscalate').listeners.click();
  assert.equal(p.sent.length, 0);
  p.incoming(handoffFact({canEscalate: true}));
  p.elements.get('handoffEscalate').listeners.click();
  assert.deepEqual(JSON.parse(JSON.stringify(p.sent)), [{kind: 'handoffControl', operation: 'escalate',
    targetKind: 'device', targetId: 'explicit-device'}]);
  p.incoming(handoffFact({state: 2, canAccept: true, canReject: true, canCancel: true}));
  for (const operation of ['accept', 'reject', 'cancel']) {
    const id = 'handoff' + operation[0].toUpperCase() + operation.slice(1);
    p.elements.get(id).listeners.click();
    assert.deepEqual(JSON.parse(JSON.stringify(p.sent.at(-1))), {kind: 'handoffControl', operation});
  }
  p.incoming(handoffFact({state: 8, canHangup: true}));
  p.elements.get('handoffHangup').listeners.click();
  assert.deepEqual(JSON.parse(JSON.stringify(p.sent.at(-1))), {kind: 'handoffControl', operation: 'hangup'});
});
test('本地目标按原UTF8标识边界检查，服务不因界面选择变成已授权', () => {
  const p = page({UrHumanVoiceEnabled: true});
  p.incoming(handoffFact({canEscalate: true}));
  const kind = p.elements.get('handoffTargetKind'), target = p.elements.get('handoffTarget');
  kind.value = 'service'; target.value = 'human-support';
  p.elements.get('handoffEscalate').listeners.click();
  assert.equal(p.sent.at(-1).targetKind, 'service');
  for (const value of ['', 'x'.repeat(129), '中'.repeat(43), '\u0000', '\u007f', '\ud800']) {
    target.value = value;
    const before = p.sent.length;
    p.elements.get('handoffEscalate').listeners.click();
    assert.equal(p.sent.length, before);
  }
  kind.value = 'user'; target.value = 'explicit-device';
  p.elements.get('handoffEscalate').listeners.click();
  assert.equal(p.sent.length, 1);
});
test('终态关闭清除按钮权限，迟到投影不能复活本地操作', () => {
  const p = page({UrHumanVoiceEnabled: true});
  p.incoming(handoffFact({canEscalate: true}));
  p.context.release('已关闭');
  assert.equal(p.elements.get('handoffEscalate').disabled, true);
  assert.throws(() => p.incoming(handoffFact({canEscalate: true})), /invalid_handoff_state/);
  p.elements.get('handoffEscalate').listeners.click();
  assert.equal(p.sent.length, 0);
});

test('正式门控命令只交原门控端口，不能当成线程已应用或直接开采集', () => {
  const p = page(), command = {kind: 'inputOwnershipGate', ...p.scope, ticket: '1', enabled: true};
  p.incoming(command);
  assert.deepEqual(p.requests, [command]);
  assert.equal(p.sent.length, 0);
  assert.equal(p.posted.length, 0);
});

test('实际原线程ACK沿门控端口处理，原有单一onmessage不被覆盖', () => {
  const p = page(), actual = {kind: 'inputOwnershipGated', ...p.scope, ticket: '1', enabled: true};
  p.actual(actual);
  assert.deepEqual(p.sent, [actual]);
  assert.equal(p.posted.length, 0);
});

test('原采集帧保留scope并生成同scope ACK，未取得和迟到帧不得发送', () => {
  const p = page();
  p.actual(frame(p.scope));
  assert.equal(p.sent.length, 0);
  p.own(true);
  p.actual(frame({...p.scope, sessionId: 'retired-ai'}));
  assert.equal(p.sent.length, 0);
  p.actual(frame(p.scope));
  // VM对象的原型属于隔离realm；按原序列化字节比较，不把realm差异误报为丢scope。
  assert.deepEqual(JSON.parse(JSON.stringify(p.sent)), [frame(p.scope)]);
  assert.deepEqual(JSON.parse(JSON.stringify(p.controlled)), [{kind: 'captureAck', ...p.scope, id: 'original-input'}]);
  assert.equal(p.metrics().captured, 1);
  assert.equal(p.posted.length, 0);
});

test('原采集命令被围栏时隔离，不能从未确认命令更新页面采集事实', () => {
  const p = page(), command = {kind: 'captureStart', ...p.scope, id: 'original-input'};
  p.incoming(command);
  assert.equal(p.posted.length, 0);
  assert.equal(p.metrics().captureOpen, null);
  p.own(true);
  p.incoming(command);
  assert.deepEqual(p.controlled, [command, command]);
  assert.equal(p.posted.length, 0);
});

// ratePage只验证正式配置派生，不把VM或固定boot假值当浏览器实际采样率事实。
function ratePage(values) {
  const context = vm.createContext({...values,
    document: {getElementById() { return {addEventListener() {}}; }},
    window: {addEventListener() {}},
  });
  vm.runInContext(source, context);
  return () => context.captureSampleRate();
}
test('专用共享宿主显式48k，两用途不得在单一能力快照之后偷换采样率', () => {
  assert.equal(ratePage({UrInputOwnershipBoot: '7', UrInputOwnershipSampleRate: 48000})(), 48000);
  assert.equal(ratePage({UrInputOwnershipBoot: '7', UrInputOwnershipSampleRate: 16000})(), 16000);
  assert.equal(ratePage({UrInputOwnershipBoot: '7'})(), 16000);
  for (const value of [24000, 44100, '48000', 48000.1, false, {}, []])
    assert.throws(ratePage({UrInputOwnershipBoot: '7', UrInputOwnershipSampleRate: value}), /invalid_input_format/);
});
test('旧独立UDP入口不因存在未装配格式变量而改成共享人工格式', () => {
  assert.equal(ratePage({UrInputOwnershipSampleRate: 48000})(), 16000);
  assert.equal(ratePage({})(), 16000);
});

test('共享48k实际播放帧按原格式960样本，不能继续固定为16k的320样本', () => {
  const p = page({UrInputOwnershipBoot: '7', UrInputOwnershipSampleRate: 48000});
  p.incoming({kind: 'playbackStart', id: 'reply', epoch: '1'});
  const packet = {kind: 'playbackFrame', epoch: '1', ticket: 1, samples: Array(960).fill(256)};
  p.incoming(packet);
  assert.equal(p.metrics().decodedNonSilent, 1);
  assert.equal(p.posted.at(-1).samples.length, 960);
});
test('共享48k拒绝错误帧长，旧独立16k仍只接320样本', () => {
  for (const length of [320, 959, 961]) {
    const p = page({UrInputOwnershipBoot: '7', UrInputOwnershipSampleRate: 48000});
    p.incoming({kind: 'playbackStart', id: 'reply', epoch: '1'});
    assert.throws(() => p.incoming({kind: 'playbackFrame', epoch: '1', ticket: 1, samples: Array(length).fill(256)}), /invalid_playback_frame/);
  }
  const p = page();
  p.incoming({kind: 'playbackStart', id: 'reply', epoch: '1'});
  p.incoming({kind: 'playbackFrame', epoch: '1', ticket: 1, samples: Array(320).fill(256)});
  assert.equal(p.posted.at(-1).samples.length, 320);
});

test('旧页面不因固定人工资产存在而装配另一媒体端点', async () => {
  let created = 0;
  const p = page({UrHumanVoiceMedia: function () { ++created; }});
  await p.context.initializeHumanVoiceMedia();
  assert.equal(created, 0);
  assert.throws(() => p.context.dispatchLocalMessage({kind: 'humanVoiceStart'}), /invalid_human_voice_command/);
});
test('专用共享页面人工适配只借同一个源和context，初始化不等于输出ACK', async () => {
  const sharedStream = {}, sharedSource = {}, sharedContext = {}, initializations = [], creations = [];
  const p = page({UrInputOwnershipBoot: '7', UrInputOwnershipSampleRate: 48000, UrHumanVoiceEnabled: true,
    sharedStream, sharedSource, sharedContext,
    UrHumanVoiceMedia: function (options) {
      creations.push(options);
      this.initialize = async boot => { initializations.push(boot); return true; };
    }});
  vm.runInContext('stream=sharedStream;source=sharedSource;context=sharedContext;', p.context);
  await p.context.initializeHumanVoiceMedia();
  assert.equal(creations.length, 1);
  assert.equal(creations[0].stream, sharedStream);
  assert.equal(creations[0].source, sharedSource);
  assert.equal(creations[0].context, sharedContext);
  assert.deepEqual(initializations, ['7']);
  assert.equal(p.sent.length, 0);
  await assert.rejects(p.context.initializeHumanVoiceMedia(), /invalid_human_voice_assembly/);
});
test('人工命令异步交原适配，输入绑定使用其原Peer/Sender而不复制物理音轨', async () => {
  const commands = [], media = {peer: {}, sender: {}};
  const p = page({UrInputOwnershipBoot: '7', UrInputOwnershipSampleRate: 48000, UrHumanVoiceEnabled: true,
    UrHumanVoiceMedia: function () {
      this.initialize = async () => true;
      this.handle = async command => { commands.push({...command}); return true; };
      this.inputBinding = (session, generation) => session === 'human' && generation === '8' ? media : null;
    }});
  await p.context.initializeHumanVoiceMedia();
  const command = {kind: 'humanVoiceStart', sessionId: 'human', routeGeneration: '8', peerEpoch: '9',
    sampleRate: 48000, channels: 1, frameMs: 20};
  p.context.dispatchLocalMessage(command);
  await Promise.resolve();
  assert.deepEqual(commands, [command]);
  p.incoming({kind: 'inputOwnershipBind', routeKind: 'humanSender', sessionId: 'human', routeGeneration: '8'});
  assert.equal(p.bound.at(-1).media, media);
  assert.equal(p.sent.length, 0);
  assert.equal(p.posted.length, 0);
});
test('人工装配格式不符或初始化失败不能静默切换旧视频驱动', async () => {
  let created = 0;
  const invalid = page({UrInputOwnershipBoot: '7', UrInputOwnershipSampleRate: 16000, UrHumanVoiceEnabled: true,
    UrHumanVoiceMedia: function () { ++created; }});
  await assert.rejects(invalid.context.initializeHumanVoiceMedia(), /invalid_human_voice_assembly/);
  assert.equal(created, 0);
  const rejected = page({UrInputOwnershipBoot: '7', UrInputOwnershipSampleRate: 48000, UrHumanVoiceEnabled: true,
    UrHumanVoiceMedia: function () { this.initialize = async () => false; }});
  await assert.rejects(rejected.context.initializeHumanVoiceMedia(), /invalid_human_voice_assembly/);
  assert.equal(rejected.sent.length, 0);
});

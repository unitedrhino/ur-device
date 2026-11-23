// 文件说明：原同一设备页面的节目宿主合同；模拟原末端事实，不声称实际浏览器能力或节目验收。
const {test} = require('node:test');
const assert = require('node:assert/strict');
const {fixture, flush} = require('./live_test_support.cjs');

// until 本例有界推进模拟呈现/消息微任务，超时保留失败而非无限等或改生产预算。
async function until(predicate) {
  for (let count = 0; count < 250; ++count) {
    if (predicate()) return;
    await new Promise(resolve => setTimeout(resolve, 1)); // 呈现模拟用timer，不能用极速微任务循环饿死rAF。
  }
  assert.equal(predicate(), true, 'simulated original fact not delivered');
}
// activated 仅调用真实实现的模拟驱动激活；初始每组仍须独立原false票据。
async function activated(value) {
  const host = new value.context.UrLiveHost(event => value.sent.push({...event}), value.root, value.context);
  assert.equal(await host.activate(), true); return host;
}
// closedLane 为原组交实际false模拟回执，未来真Core只能在该票据收录后调用begin。
async function closedLane(host, name, ticket = '1') {
  assert.equal(host.gate({kind: 'liveLaneGate', lane: name, gateTicket: ticket, enabled: false}), true);
  await until(() => host.lanes.get(name).ready);
}
// beginBinding 保持固定本地无设备身份的完整group；并非云端Invite或授权凭据。
function beginBinding(name, instance = '1') {
  return {kind: 'liveBegin', instance, mediaRevision: 1, lane: name,
    audio: name !== 'video', video: name !== 'audio'};
}

test('原begin派生platform必须保留原Window样式receiver，canvas与祖先读回不误失败', async () => {
  const value = fixture();let styleReads = 0;
  // 约束对应独立真实Chromium原Host.begin/Player.present已复现的Window原生品牌检查，不更改呈现成功条件。
  value.context.getComputedStyle = function(element) {
    if (this !== value.context) throw new TypeError('window_receiver_required');
    ++styleReads;return {display: element.style.display || 'block', visibility: 'visible', opacity: '1'};
  };
  const host = await activated(value);await closedLane(host, 'video');
  assert.equal(host.begin(beginBinding('video')), true);
  await until(() => value.sent.some(event => event.kind === 'liveOffer'));
  const lane = host.lanes.get('video');value.root.style = {display: 'block'};lane.canvas.parentElement = value.root;
  try {
    assert.notEqual(host.player.platform, value.context);
    assert.equal(value.context.liveContext, undefined, '节目上下文不写入原Window平台');
    await assert.doesNotReject(host.player.present(new value.context.HTMLVideoElement(), host.life.signal));
    assert.equal(styleReads, 2);assert.equal(host.player.metrics.visibleFrames, 1);
  } finally { await host.close(); }
});

test('原begin派生platform读取document必须保留Window getter receiver，不阻断视频初始化', async () => {
  const value = fixture();const document = value.context.document;
  // document与样式方法同属真实Window品牌边界；派生平台不能以自身作原生getter receiver。
  Object.defineProperty(value.context, 'document', {get() {
    if (this !== value.context) throw new TypeError('window_document_receiver_required');
    return document;
  }});
  const host = await activated(value);await closedLane(host, 'video');
  assert.equal(host.begin(beginBinding('video')), true);
  try {
    assert.equal(host.player.platform.document, document);
    await until(() => value.sent.some(event => event.kind === 'liveOffer'));
    assert.equal(value.sent.some(event => event.kind === 'liveFailed'), false);
  } finally { await host.close(); }
});

test('codec and actual native transform readiness intersection; no capture or active', async () => {
  const value = fixture(); const host = await activated(value);
  assert.deepEqual(value.sent, [{kind: 'liveReady', audio: true, video: true}]);
  assert.equal(value.workers.length, 2); assert.equal(value.workers.every(worker => worker.closed), true);
  assert.equal(value.peers.every(peer => peer.connectionState === 'closed'), true);
  assert.equal([...host.lanes.values()].every(lane => !lane.enabled && !lane.ready), true);
  assert.equal(await host.close(), true);
});

test('initial actual false gate required independently for all three output groups', async () => {
  const value = fixture(); const host = await activated(value);
  assert.equal(host.begin(beginBinding('audio')), false);
  assert.equal(host.gate({kind: 'liveLaneGate', lane: 'audio', gateTicket: '1', enabled: true}), false);
  await closedLane(host, 'audio');
  assert.equal(host.begin(beginBinding('video')), false);
  for (const name of ['video', 'audioVideo']) await closedLane(host, name);
  const gates = value.sent.filter(event => event.kind === 'liveLaneGated');
  assert.equal(gates.length, 3); assert.equal(gates.every(gate => gate.enabled === false), true);
  assert.equal(await host.close(), true);
});

test('pending original gate cannot be replaced and old/future conflict stays unconfirmed', async () => {
  const value = fixture(); const host = await activated(value);
  const original = {kind: 'liveLaneGate', lane: 'audio', gateTicket: '1', enabled: false};
  assert.equal(host.gate(original), true);
  assert.equal(host.gate({...original, gateTicket: '2'}), false);
  await until(() => host.lanes.get('audio').ready);
  assert.equal(host.gate(original), false);
  assert.equal(host.gate({...original, gateTicket: '01'}), false);
  assert.equal(host.gate({...original, unknown: 1}), false);
  assert.equal(host.gate({...original, gateTicket: '18446744073709551616'}), false);
  assert.equal(value.sent.filter(event => event.kind === 'liveLaneGated').length, 1);
  assert.equal(await host.close(), true);
});

test('same host pending resume full close and next programme waits new actual false node', async () => {
  const value = fixture(); const host = await activated(value);
  await closedLane(host, 'audio');
  assert.equal(host.begin(beginBinding('audio')), true);
  const first = host.player; await until(() => value.sent.some(event => event.kind === 'liveOffer'));
  assert.equal(await first.answer({kind: 'liveAnswer', instance: '1', mediaRevision: 1,
    sdp: 'v=0\r\ns=one\r\n'}), true);
  assert.equal(host.gate({kind: 'liveLaneGate', lane: 'audio', gateTicket: '2', enabled: true}), true);
  await until(() => !host.lanes.get('audio').pending);
  assert.equal(first.track({kind: 'liveTrackControl', instance: '1', mediaRevision: 1,
    trackId: 'broadcast-out', generation: 1, operation: 'resume', gateTicket: '2'}), true);
  await flush();
  const oldNode = first.lane.node;
  assert.equal(await host.closePlayer(first), true);
  assert.equal(oldNode.disconnected, true);
  assert.equal(first.peer.connectionState, 'closed');
  assert.equal(first.transforms.get('audio').receiver.track.readyState, 'ended');
  assert.equal(value.sent.filter(event => event.kind === 'liveStopped').length, 1);
  assert.equal(host.player, null); assert.equal(host.lanes.get('audio').ready, false);
  assert.equal(host.lanes.get('audio').enabled, false);
  assert.notEqual(host.lanes.get('audio').node, oldNode);
  assert.equal(host.begin(beginBinding('audio', '2')), false);
  assert.equal(host.lanes.get('audio').node.port.commands.length, 0); // 不重放旧true或旧resume。
  await closedLane(host, 'audio', '3');
  assert.equal(host.begin(beginBinding('audio', '1')), false); // 原实例不能复活。
  assert.equal(host.begin(beginBinding('audio', '2')), true);
  await until(() => value.sent.filter(event => event.kind === 'liveOffer').length === 2);
  assert.equal(host.player.generations.size, 0); assert.equal(host.player.wanted.size, 0);
  assert.equal(await host.close(), true);
});

test('original volume controls every lane before worklet and closed host refuses change', async () => {
  const value = fixture(); const host = await activated(value);
  assert.equal(host.setGain(0.4), true);
  assert.equal([...host.lanes.values()].every(lane => lane.gain.gain.value === 0.4), true);
  for (const invalid of [-1, 2, NaN, Infinity, '0.4']) assert.equal(host.setGain(invalid), false);
  assert.equal(host.setGain(0), true);
  assert.equal(await host.close(), true); assert.equal(host.setGain(0.4), false);
});

test('local original volume ticket waits actual every-lane readback without cloud action identity', async () => {
  const value = fixture(); const host = await activated(value);
  assert.equal(host.handle({kind: 'liveVolume', volumeTicket: '1', percent: 40}), true);
  await until(() => value.sent.some(event => event.kind === 'liveVolumeApplied'));
  const actual = value.sent.find(event => event.kind === 'liveVolumeApplied');
  assert.deepEqual(actual, {kind: 'liveVolumeApplied', volumeTicket: '1', percent: 40});
  assert.equal([...host.lanes.values()].every(lane => lane.gain.gain.value === 0.4), true);
  assert.equal(host.handle({kind: 'liveVolume', volumeTicket: '1', percent: 40}), false);
  assert.equal(host.handle({kind: 'liveVolume', volumeTicket: '2', percent: 40}), true);
  await until(() => value.sent.filter(event => event.kind === 'liveVolumeApplied').length === 2);
  assert.equal(await host.close(), true);
  assert.equal(host.handle({kind: 'liveVolume', volumeTicket: '3', percent: 40}), false);
});

test('local volume malformed or failed actual gain readback cannot acknowledge success', async () => {
  const value = fixture(); const host = await activated(value);
  for (const command of [
    {kind: 'liveVolume', volumeTicket: '01', percent: 40},
    {kind: 'liveVolume', volumeTicket: '1', percent: 101},
    {kind: 'liveVolume', volumeTicket: '1', percent: '40'},
    {kind: 'liveVolume', volumeTicket: '1', percent: 40, token: 'private-action'}])
    assert.equal(host.handle(command), false);
  const gain = host.lanes.get('audioVideo').gain.gain;
  Object.defineProperty(gain, 'value', {get() { return 1; }, set() {}});
  assert.equal(host.handle({kind: 'liveVolume', volumeTicket: '1', percent: 40}), true);
  await flush(); await flush();
  assert.equal(value.sent.some(event => event.kind === 'liveVolumeApplied'), false);
  assert.equal(await host.close(), true);
});

test('unknown/file/AI and fabricated media state cannot become live output', async () => {
  const value = fixture(); const host = await activated(value);
  for (const kind of ['audioStart', 'filePlay', 'trackState', 'liveConnected', 'liveTrackFact'])
    assert.equal(host.handle({kind, instance: '1', mediaRevision: 1, state: 'active'}), false);
  await closedLane(host, 'video');
  assert.equal(host.handle({...beginBinding('video'), cameraId: 'front'}), false);
  assert.equal(host.handle({...beginBinding('video'), url: 'https://invalid.example/a.mp4'}), false);
  assert.equal(host.player, null); assert.equal(await host.close(), true);
});

test('missing H264/Opus does not advertise unsupported decoder or fallback', async () => {
  const value = fixture({missingCodec: true});
  const host = new value.context.UrLiveHost(event => value.sent.push({...event}), value.root, value.context);
  assert.equal(await host.activate(), false);
  assert.deepEqual(value.sent, [{kind: 'liveReady', audio: false, video: false}]);
  await closedLane(host, 'audio'); assert.equal(host.begin(beginBinding('audio')), false);
  assert.equal(await host.close(), true);
});

test('wrong actual context sample rate advertises neither track and never acquires media', async () => {
  const value = fixture({sampleRate: 44100});
  const host = new value.context.UrLiveHost(event => value.sent.push({...event}), value.root, value.context);
  assert.equal(await host.activate(), false);
  assert.equal(value.workers.length, 0); assert.equal(value.peers.length, 0);
  assert.deepEqual(value.sent, [{kind: 'liveReady', audio: false, video: false}]);
  assert.equal(await host.close(), true);
});

test('actual transform close unknown does not emit successful programme release', async () => {
  const value = fixture({failedTransformClose: true, fastTimeout: true});
  const player = value.player();
  assert.equal(await player.initialize(), true);
  assert.equal(await player.close(), false);
  assert.equal(value.sent.some(event => event.kind === 'liveStopped'), false);
  assert.equal(player.closed, false);
});

test('full host close releases programme before base resources and never rebuilds nodes', async () => {
  const value = fixture(); const host = await activated(value);
  await closedLane(host, 'audioVideo'); assert.equal(host.begin(beginBinding('audioVideo')), true);
  await until(() => value.sent.some(event => event.kind === 'liveOffer'));
  const count = value.nodes.length;
  assert.equal(await host.close(), true); assert.equal(host.context.state, 'closed');
  assert.equal(value.nodes.length, count);
  assert.equal(value.workers.every(worker => worker.closed), true);
  assert.equal(value.sent.filter(event => event.kind === 'liveStopped').length, 1);
  assert.equal(host.begin(beginBinding('audioVideo', '2')), false);
});

test('close while original activation awaits resume cannot allocate later or advertise readiness', async () => {
  const value = fixture(); let continueResume;
  const blocked = new Promise(resolve => { continueResume = resolve; });
  value.context.AudioContext.prototype.resume = async function () { await blocked; this.state = 'running'; };
  const host = new value.context.UrLiveHost(event => value.sent.push({...event}), value.root, value.context);
  const active = host.activate(); await flush();
  const count = value.nodes.length; const closing = host.close();
  continueResume();
  assert.equal(await active, false); assert.equal(await closing, true);
  assert.equal(value.nodes.length, count); assert.equal(value.peers.length, 0);
  assert.equal(value.sent.length, 0); assert.equal(host.context.state, 'closed');
});

// 文件说明：独立节目接收驱动合同测试；显式模拟媒体事实，真实Chrome验证另行执行。
const {test} = require('node:test');
const assert = require('node:assert/strict');
const {fixture, flush} = require('./live_test_support.cjs');

// established 本例完成真实接口调用顺序的模拟网络事实，不能称平台授权或真实媒体成功。
async function established(value) {
  const player = value.player(); assert.equal(await player.initialize(), true);
  assert.equal(await player.answer({kind: 'liveAnswer', instance: '1', mediaRevision: 1,
    sdp: 'v=0\r\ns=answer-contract\r\n'}), true);
  assert.equal(player.connected, true); return player;
}

test('recvonly offer/connected never imply capture or active', async () => {
  const value = fixture(); const player = await established(value);
  assert.equal(value.peers[0].transceivers.every(track => track.direction === 'recvonly'), true);
  assert.deepEqual(value.sent.map(event => event.kind), ['liveOffer', 'liveConnected']);
  assert.equal(value.platform.liveContext.sources.length, 0);
  assert.equal(value.lane.canvas.draws, 0);
  assert.equal(await player.close(), true);
});

test('audio active needs original fresh frame AND actual PCM original gate ticket', async () => {
  const value = fixture({lane: 'audio'}); const player = await established(value);
  assert.equal(player.track(value.control('audio', 'resume')), true); await flush();
  value.emitAudio(); await flush(); // 没有新编码帧，末端尚未受理resume，旧PCM不能成为事实。
  assert.equal(value.sent.some(event => event.kind === 'liveTrackFact'), false);
  value.emitFresh('audio'); await flush(); value.emitAudio(); await flush();
  const fact = value.sent.find(event => event.kind === 'liveTrackFact');
  assert.equal(fact.state, 'active'); assert.equal(fact.gateTicket, '2'); assert.equal(fact.generation, 1);
  assert.equal(value.platform.liveContext.sources.length, 1);
  assert.equal(await player.close(), true);
});

test('video needs current fresh IDR RTP stamp and actual visible presentation', async () => {
  const value = fixture({lane: 'video'}); const player = await established(value);
  assert.equal(player.track(value.control('video', 'resume')), true); await flush();
  value.frame(99); await flush(); assert.equal(value.lane.canvas.draws, 0);
  value.emitFresh('video', 1, 100); await flush();
  value.frame(99); await flush(); assert.equal(value.lane.canvas.draws, 0);
  value.frame(100); await flush();
  const fact = value.sent.find(event => event.kind === 'liveTrackFact');
  assert.equal(fact.state, 'active'); assert.equal(fact.gateTicket, '2');
  assert.equal(value.lane.canvas.style.display, 'block');
  assert.equal(await player.close(), true);
  assert.equal(value.frames.size, 0);
});

test('audio resume remains independent of paused video and single audio stop preserves video', async () => {
  const value = fixture(); const player = await established(value);
  assert.equal(player.track(value.control('video', 'pause')), true);
  assert.equal(player.track(value.control('audio', 'resume')), true); await flush();
  value.emitFresh('audio'); await flush(); value.emitAudio(); await flush();
  assert.equal(value.sent.find(event => event.trackId === 'broadcast-out').state, 'active');
  assert.equal(player.track(value.control('audio', 'stop', 2)), true); await flush(); await flush();
  assert.equal(value.peers[0].transceivers[0].receiver.track.readyState, 'ended');
  assert.equal(value.peers[0].transceivers[1].receiver.track.readyState, 'live');
  assert.equal(player.track(value.control('audio', 'resume', 3)), false);
  assert.equal(await player.close(), true);
});

test('pending resume focus loss fails only after actual close; restored mask cannot resume', async () => {
  const value = fixture({lane: 'audio'}); const player = await established(value);
  player.track(value.control('audio', 'resume')); await flush();
  let settle; const settled = new Promise(resolve => { settle = resolve; });
  value.lane.ticket = '3'; value.lane.enabled = false;
  const changed = player.maskChanged(false, '3', settled); await flush();
  assert.equal(value.sent.some(event => event.kind === 'liveTrackFact'), false);
  settle(); await changed;
  const original = value.sent.find(event => event.kind === 'liveTrackFact');
  assert.equal(original.code, 409); assert.equal(original.failure, 'outputBusy');
  value.lane.ticket = '4'; value.lane.enabled = true;
  await player.maskChanged(true, '4', Promise.resolve());
  value.emitFresh('audio'); value.emitAudio(); await flush();
  assert.equal(value.sent.filter(event => event.kind === 'liveTrackFact').length, 1);
  assert.equal(player.wanted.get('audio'), false); assert.equal(await player.close(), true);
});

test('audio cancellation fact cannot acknowledge before full AV mask is actually hidden', async () => {
  const value = fixture(); const player = await established(value);
  player.track(value.control('audio', 'resume')); await flush(); value.emitFresh('audio'); await flush();
  let hidden; const wholeGroupSettled = new Promise(resolve => { hidden = resolve; });
  value.lane.ticket = '3'; value.lane.enabled = false;
  const closing = player.maskChanged(false, '3', wholeGroupSettled);
  value.lane.node.port.emit({kind: 'liveAudioTrackFailed', generation: 1, failure: 'outputBusy'});
  await flush();
  assert.equal(value.sent.some(event => event.kind === 'liveTrackFact'), false);
  hidden(); await closing; await flush();
  const facts = value.sent.filter(event => event.kind === 'liveTrackFact');
  assert.equal(facts.length, 1); assert.equal(facts[0].code, 409);
  assert.equal(await player.close(), true);
});

test('valid already formed audio original fact survives temporary mask in bridge queue', async () => {
  const value = fixture({lane: 'audio'}); const player = await established(value);
  player.track(value.control('audio', 'resume')); await flush(); value.emitFresh('audio'); await flush();
  value.emitAudio(); // 原实际回调完成，但promise消费尚在微任务队列。
  value.lane.ticket = '3'; value.lane.enabled = false;
  await player.maskChanged(false, '3', Promise.resolve());
  const facts = value.sent.filter(event => event.kind === 'liveTrackFact');
  assert.equal(facts.length, 1); assert.equal(facts[0].state, 'active'); assert.equal(facts[0].gateTicket, '2');
  assert.equal(player.wanted.get('audio'), false);
  assert.equal(await player.close(), true);
});

test('full close cancels pending silent resume without waiting for programme audio', async () => {
  const value = fixture({lane: 'audio'}); const player = await established(value);
  player.track(value.control('audio', 'resume')); await flush(); value.emitFresh('audio'); await flush();
  assert.equal(await player.close(), true);
  assert.equal(value.sent.some(event => event.kind === 'liveTrackFact'), false);
  const released = value.sent.find(event => event.kind === 'liveStopped');
  assert.equal(['peerClosed', 'tracksStopped', 'outputsDetached', 'callbacksStopped', 'transformsStopped']
    .every(name => released[name] === true), true);
  assert.equal(value.lane.node.disconnected, true);
  assert.equal(value.workers.every(worker => worker.closed), true);
  assert.equal(value.peers[0].transceivers[0].receiver.track.readyState, 'ended');
});

test('fixed local gain validates bounds and refuses closed player', async () => {
  const value = fixture({lane: 'audio'}); const player = await established(value);
  for (const invalid of [NaN, Infinity, -0.1, 1.1, '0.4']) assert.equal(player.setGain(invalid), false);
  assert.equal(player.setGain(0.4), true); assert.equal(value.lane.gain.gain.value, 0.4);
  assert.equal(player.setGain(0), true); assert.equal(await player.close(), true);
  assert.equal(player.setGain(0.7), false);
});

test('mismatch instance/m/generation/free fields cannot replace original operation', async () => {
  const value = fixture({lane: 'audio'}); const player = await established(value);
  for (const extra of [{instance: '2'}, {mediaRevision: 2}, {generation: 0},
    {gateTicket: '3'}, {url: 'https://invalid.example/media'}])
    assert.equal(player.track({...value.control('audio', 'resume'), ...extra}), false);
  assert.equal(player.track(value.control('audio', 'pause')), true); await flush();
  assert.equal(player.track(value.control('audio', 'pause')), false);
  assert.equal(await player.close(), true);
});

test('H264/Opus codec absence rejects instead of VP8/AAC fallback', async () => {
  const value = fixture({missingCodec: true, lane: 'video'}); const player = value.player();
  assert.equal(await player.initialize(), false); await flush();
  assert.equal(value.sent.some(event => event.kind === 'liveOffer'), false);
  assert.equal(value.sent.some(event => event.kind === 'liveFailed'), true);
  assert.equal(await player.close(), true);
});

test('cancelled original wait never posts a late command or leaks subscription', async () => {
  const value = fixture(); const abort = new AbortController(); abort.abort();
  const count = value.lane.node.port.commands.length;
  await assert.rejects(value.lane.audio.expect({kind: 'forbidden'}, () => true, abort.signal));
  assert.equal(value.lane.node.port.commands.length, count);
  assert.equal(value.lane.audio.listeners.size, 0);
});

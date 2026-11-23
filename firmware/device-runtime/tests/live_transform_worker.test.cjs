// 文件说明：接收transform控制与真实NAL结构回归；模拟pipeline不等同真实DTLS/H264解码。
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

// create 捕获真正stream控制接口；frame测试只核原NAL/原意图，不造实际播放active。
function create(video = true) {
  const messages = [];
  let keyRequests = 0;
  const context = vm.createContext({self: {}, ArrayBuffer, Uint8Array, AbortController,
    ReadableStream, WritableStream, TransformStream, Promise});
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/live-transform-worker.js'), 'utf8'), context);
  const transformer = {options: {kind: video ? 'video' : 'audio', port: {postMessage: value => messages.push(value)}},
    readable: new ReadableStream(), writable: new WritableStream(),
    sendKeyFrameRequest: async () => { keyRequests += 1; }};
  context.transformer = transformer;
  const driver = vm.runInContext('new UrLiveTransform(transformer)', context);
  const frames = [];
  const frame = (nal = [0, 0, 0, 1, 0x65, 1], type = 'key', stamp = 9000) => driver.frame({
    data: new Uint8Array(nal).buffer, type, getMetadata: () => ({rtpTimestamp: stamp})}, {enqueue: value => frames.push(value)});
  const close = async () => { driver.command({kind: 'liveTransformClose'}); await driver.completion; };
  return {driver, messages, frames, frame, close, requests: () => keyRequests};
}
// mask 只请求原末端控制，不构造新的节目播放意图。
function mask(fixture, ticket, enabled) { fixture.driver.command({kind: 'liveTransformMask', ticket: String(ticket), enabled}); }
// track 只请求本轨原新generation，视频等待自己的新IDR而不依赖audio。
function track(fixture, generation, operation = 'resume') { fixture.driver.command({kind: 'liveTransformTrack', generation, operation}); }
test('initial queues closed and encoded ready cannot claim active', async () => {
  const fixture = create(); fixture.frame(); assert.equal(fixture.frames.length, 0);
  assert.equal(fixture.messages[0].kind, 'liveTransformReady');
  assert.equal(fixture.messages.some(value => value.state === 'active'), false); await fixture.close();
});
test('video resume requests new key but only actual NAL5 passes', async () => {
  const fixture = create(); mask(fixture, 1, true); track(fixture, 1);
  assert.equal(fixture.requests(), 1);
  fixture.frame([0, 0, 0, 1, 0x41, 1], 'delta'); fixture.frame([0, 0, 0, 1, 0x67, 1]);
  assert.equal(fixture.frames.length, 0);
  fixture.frame(); assert.equal(fixture.frames.length, 1);
  assert.equal(fixture.messages.at(-1).kind, 'liveEncodedFresh');
  assert.equal(fixture.messages.at(-1).rtpTimestamp, 9000);
  assert.equal(fixture.messages.at(-1).gateTicket, '1'); await fixture.close();
});
test('avcc IDR accepted but corrupt or unrelated key packet rejected', async () => {
  const fixture = create(); mask(fixture, 1, true); track(fixture, 1);
  fixture.frame([0, 0, 0, 99, 0x65, 1]); fixture.frame([0, 0, 0, 2, 0x67, 1]);
  assert.equal(fixture.frames.length, 0); fixture.frame([0, 0, 0, 2, 0x65, 1]);
  assert.equal(fixture.frames.length, 1); await fixture.close();
});
test('temporary mask closes old intent and restore needs explicit new resume', async () => {
  const fixture = create(); mask(fixture, 1, true); track(fixture, 1); fixture.frame();
  mask(fixture, 2, false); fixture.frame(); mask(fixture, 3, true); fixture.frame();
  assert.equal(fixture.frames.length, 1); track(fixture, 2);
  fixture.frame([0, 0, 0, 1, 0x41, 1], 'delta'); assert.equal(fixture.frames.length, 1);
  fixture.frame(); assert.equal(fixture.frames.length, 2); assert.equal(fixture.requests(), 2);
  await fixture.close();
});
test('audio independent frame never waits for video or key request', async () => {
  const fixture = create(false); mask(fixture, 1, true); track(fixture, 1); fixture.frame([], 'delta');
  assert.equal(fixture.frames.length, 1); assert.equal(fixture.requests(), 0);
  track(fixture, 2, 'pause'); fixture.frame(); assert.equal(fixture.frames.length, 1);
  await fixture.close();
});
test('stopped single track cannot resume and close awaits real pipeline cancellation', async () => {
  const fixture = create(); mask(fixture, 1, true); track(fixture, 1, 'stop'); track(fixture, 2);
  fixture.frame(); assert.equal(fixture.frames.length, 0); assert.equal(fixture.driver.failed, true);
  await fixture.close();
});
test('wrong RTP timestamp and malformed original control fail closed', async () => {
  const fixture = create(); mask(fixture, 1, true); track(fixture, 1); fixture.frame(undefined, 'key', -1);
  assert.equal(fixture.driver.failed, true); assert.equal(fixture.frames.length, 0); await fixture.close();
  const next = create(); next.driver.command({kind: 'liveTransformMask', ticket: '01', enabled: true});
  assert.equal(next.driver.failed, true); await next.close();
});

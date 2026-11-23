// 文件说明：节目音频线程确定性测试；真实PCM/WebRTC验收另由正式浏览器链路证明。
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

// create 只模拟线程调度/真实数值音频块，不把VM视为真实浏览器或扬声器证明。
function create(rate = 48000) {
  const messages = [];
  let Processor;
  const sandbox = {sampleRate: rate, registerProcessor: (_, type) => { Processor = type; },
    AudioWorkletProcessor: class { constructor() { this.port = {postMessage: value => messages.push(value)}; } }};
  vm.runInNewContext(fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/live-worklet.js'), 'utf8'), sandbox);
  const processor = new Processor();
  const command = value => processor.port.onmessage({data: value});
  const block = (value = .125) => {
    const input = new Float32Array(128).fill(value);
    const output = new Float32Array(128).fill(.5);
    const running = processor.process([[input]], [[output]]);
    return {running, peak: Math.max(...output.map(Math.abs))};
  };
  return {processor, command, block, messages};
}
// mask 只请求原末端票据，实际处理块前不算已关闭/开启。
function mask(fixture, ticket, enabled) {
  fixture.command({kind: 'liveAudioMask', ticket: String(ticket), enabled});
}
// track 只请求原generation的显式单轨操作，不从mask派生新播放意图。
function track(fixture, generation, operation = 'resume') {
  fixture.command({kind: 'liveAudioTrack', generation, operation});
}
test('initial output is closed and no capture or invented active exists', () => {
  const fixture = create();
  assert.equal(fixture.block().peak, 0);
  assert.equal(fixture.messages.length, 0);
});
test('mask applies only after actual block and cannot become track active', () => {
  const fixture = create(); mask(fixture, 1, false);
  assert.equal(fixture.messages.length, 0); fixture.block();
  assert.equal(fixture.messages[0].kind, 'liveAudioMasked');
  mask(fixture, 2, true); assert.equal(fixture.block().peak, 0);
  assert.equal(fixture.messages.filter(value => value.kind === 'liveAudioTrackApplied').length, 0);
});
test('resume needs non-silent actual output while pause and stop confirm actual silence', () => {
  const fixture = create(); mask(fixture, 1, true); fixture.block(); track(fixture, 1);
  assert.equal(fixture.block(0).peak, 0);
  assert.equal(fixture.messages.filter(value => value.state === 'active').length, 0);
  assert.equal(fixture.block().peak, .125);
  assert.equal(fixture.messages.at(-1).state, 'active');
  assert.equal(fixture.messages.at(-1).gateTicket, '1');
  track(fixture, 2, 'pause'); assert.equal(fixture.block().peak, 0);
  assert.equal(fixture.messages.at(-1).state, 'paused');
  track(fixture, 3, 'stop'); assert.equal(fixture.block().peak, 0);
  assert.equal(fixture.messages.at(-1).state, 'stopped');
});
test('pending resume focus cancellation waits for actual silence and cannot auto restore', () => {
  const fixture = create(); mask(fixture, 1, true); fixture.block(); track(fixture, 1);
  fixture.block(0); mask(fixture, 2, false);
  assert.equal(fixture.messages.filter(value => value.kind === 'liveAudioTrackFailed').length, 0);
  assert.equal(fixture.block().peak, 0);
  assert.equal(fixture.messages.at(-1).failure, 'outputBusy');
  mask(fixture, 3, true); assert.equal(fixture.block().peak, 0);
  track(fixture, 2); assert.equal(fixture.block().peak, .125);
});
test('active focus suspend clears playback intent without inventing track paused', () => {
  const fixture = create(); mask(fixture, 1, true); fixture.block(); track(fixture, 1); fixture.block();
  const old = fixture.messages.filter(value => value.kind === 'liveAudioTrackApplied').length;
  mask(fixture, 2, false); assert.equal(fixture.block().peak, 0);
  mask(fixture, 3, true); assert.equal(fixture.block().peak, 0);
  assert.equal(fixture.messages.filter(value => value.kind === 'liveAudioTrackApplied').length, old);
});
test('wrong values or malformed commands fail closed without echoing free errors', () => {
  for (const value of [
    {kind: 'liveAudioMask', ticket: '01', enabled: true},
    {kind: 'liveAudioMask', ticket: '18446744073709551616', enabled: false},
    {kind: 'liveAudioMask', ticket: '1', enabled: null},
    {kind: 'liveAudioTrack', generation: 1.5, operation: 'resume'},
    {kind: 'liveAudioTrack', generation: 1, operation: 'seek'},
    {kind: 'liveAudioTrack', generation: 1, operation: 'resume', source: 'private'},
  ]) {
    const fixture = create(); fixture.command(value); assert.equal(fixture.block().peak, 0);
    assert.equal(fixture.messages.at(-1).kind, 'liveAudioFailed');
    assert.equal(Object.keys(fixture.messages.at(-1)).length, 1);
  }
});
test('original gate or generation cannot be replaced while pending', () => {
  const fixture = create(); mask(fixture, 1, false); mask(fixture, 2, true);
  assert.equal(fixture.block().peak, 0); assert.equal(fixture.processor.failed, true);
  const next = create(); track(next, 1); track(next, 2); assert.equal(next.block().peak, 0);
  assert.equal(next.processor.failed, true);
});
test('invalid PCM and wrong actual sample rate cannot become active', () => {
  const fixture = create(); mask(fixture, 1, true); fixture.block(); track(fixture, 1);
  assert.equal(fixture.block(NaN).peak, 0);
  assert.equal(fixture.messages.filter(value => value.state === 'active').length, 0);
  assert.equal(create(16000).block().peak, 0);
});
test('close confirms actual silent block and late commands cannot reopen output', () => {
  const fixture = create(); mask(fixture, 1, true); fixture.block(); track(fixture, 1); fixture.block();
  fixture.command({kind: 'liveAudioClose'});
  const result = fixture.block(); assert.equal(result.peak, 0); assert.equal(result.running, false);
  assert.equal(fixture.messages.at(-1).kind, 'liveAudioClosed');
  track(fixture, 2); assert.equal(fixture.block().peak, 0);
});

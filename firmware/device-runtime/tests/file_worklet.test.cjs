// 独立节目音频门控回归；执行正式Worklet源码，不把VM模拟当实际浏览器/硬件验收。
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/file-worklet.js'), 'utf8');

// processor 创建单实例音频线程，输出只用于验证本节目而非AI节点。
function processor() {
  const events = [];
  let Constructor;
  class Base {
    constructor() { this.port = {postMessage: value => events.push(JSON.parse(JSON.stringify(value)))}; }
  }
  vm.runInNewContext(source, {AudioWorkletProcessor: Base,
    registerProcessor: (name, value) => { assert.equal(name, 'ur-file-audio'); Constructor = value; }});
  return {p: new Constructor(), events};
}
// block 执行一个真实长度音频块；undefined代表没有解码输入而不是补造静音音源。
function block(p, value, count = 128) {
  const output = new Float32Array(count).fill(1);
  const running = p.process(value === undefined ? [] : [[new Float32Array(count).fill(value)]], [[output]]);
  return {output, running};
}
// gate 门控序号和代次独立，等待后续块确认；不把命令受理当已静音。
function gate(p, enabled, generation = '1', ticket = '1') {
  p.command({kind: 'fileAudioGate', generation, enabled, ticket});
}
test('未获节目许可始终静音，门控必须实际输出块后确认', () => {
  const {p, events} = processor();
  assert.ok(block(p, 0.5).output.every(v => v === 0));
  gate(p, true);
  assert.equal(events.length, 0);
  assert.ok(block(p, 0.5).output.every(v => v === 0.5));
  assert.deepEqual(events[0], {kind: 'fileAudioGated', generation: '1', ticket: '1', enabled: true});
});
test('首事实来自连续实际采样，能量不依赖play承诺或墙钟', () => {
  const {p, events} = processor();
  gate(p, true, '18446744073709551615');
  block(p, 0.5);
  assert.equal(events.length, 1);
  block(p, -0.5);
  assert.deepEqual(events[1], {kind: 'fileAudioMeasured', generation: '18446744073709551615',
    ticket: 1, samples: 256, energy: 64});
  p.command({kind: 'fileAudioAck', ticket: 1});
  assert.equal(p.pending.length, 0);
});
test('暂停确认在零输出块之后，其他节目或麦克风不在本组件职责内', () => {
  const {p, events} = processor();
  gate(p, true);
  block(p, 0.8);
  gate(p, false, '2', '2');
  assert.equal(events.length, 1);
  assert.ok(block(p, 0.8).output.every(v => v === 0));
  assert.deepEqual(events[1], {kind: 'fileAudioGated', generation: '2', ticket: '2', enabled: false});
  assert.equal(p.samples, 0);
});
test('没有解码输入不造采样，有效静音音源仍记实际处理', () => {
  const {p, events} = processor();
  gate(p, true);
  for (let count = 0; count < 4; ++count) block(p, undefined);
  assert.equal(events.filter(v => v.kind === 'fileAudioMeasured').length, 0);
  block(p, 0); block(p, 0);
  assert.equal(events[1].samples, 256);
  assert.equal(events[1].energy, 0);
});
test('新操作重置测量，迟到旧门控不能重开输出', () => {
  const {p, events} = processor();
  gate(p, true); block(p, 0.5); block(p, 0.5);
  p.command({kind: 'fileAudioAck', ticket: 1});
  gate(p, false, '2', '2'); block(p, 1);
  gate(p, true, '1', '3');
  assert.ok(block(p, 1).output.every(v => v === 0));
  assert.equal(p.samples, 0);
  assert.equal(events.filter(v => v.kind === 'fileAudioMeasured').length, 1);
});
test('采样畸形切断输出且失败只通知一次', () => {
  for (const bad of [NaN, Infinity, -Infinity]) {
    const {p, events} = processor();
    gate(p, true);
    const result = block(p, bad);
    assert.equal(result.running, false);
    assert.ok(result.output.every(v => v === 0));
    assert.equal(block(p, 1).running, false);
    assert.equal(events.filter(v => v.kind === 'fileAudioFailed').length, 1);
  }
});
test('非法门控及乱序确认不能默认开声', () => {
  for (const variant of ['epoch', 'zero', 'overflow', 'type', 'unknown', 'gateBusy', 'ack']) {
    const {p, events} = processor();
    if (variant === 'epoch') gate(p, true, '01');
    if (variant === 'zero') gate(p, true, '0');
    if (variant === 'overflow') gate(p, true, '18446744073709551616');
    if (variant === 'type') gate(p, 'true');
    if (variant === 'unknown') p.command({kind: 'captureStart'});
    if (variant === 'gateBusy') { gate(p, true); gate(p, false, '2'); }
    if (variant === 'ack') p.command({kind: 'fileAudioAck', ticket: 1});
    assert.equal(block(p, 0.5).running, false, variant);
    assert.equal(events[0].kind, 'fileAudioFailed');
  }
});
test('不消费测量的主线程触发有界背压，不无限积压MessagePort', () => {
  const {p, events} = processor();
  gate(p, true);
  let running = true;
  for (let count = 0; count < 200 && running; ++count) running = block(p, 0.5).running;
  assert.equal(running, false);
  assert.equal(events.filter(v => v.kind === 'fileAudioMeasured').length, 8);
  assert.equal(events.at(-1).kind, 'fileAudioFailed');
});
test('释放节点须实际处理静音块后退出，不让共享上下文留下活动节目处理器', () => {
  const {p, events} = processor();
  gate(p, true); block(p, 0.5);
  p.command({kind: 'fileAudioClose', generation: '1', enabled: false, ticket: '2'});
  assert.equal(events.length, 1);
  const output = block(p, 0.5);
  assert.equal(output.running, false);
  assert.ok(output.output.every(v => v === 0));
  assert.deepEqual(events.at(-1), {kind: 'fileAudioClosed', generation: '1', enabled: false, ticket: '2'});
  const count = events.length;
  gate(p, true, '2', '3');
  assert.equal(block(p, 1).running, false);
  assert.equal(events.length, count);
});

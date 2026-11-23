// 文件说明：人际呼叫音频块门控合同；离线PCM断言不冒真实浏览器/扬声器或模型验收。
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
// fixture 只提供音频线程调度，生产DSP直接处理固定PCM，观测原实际回执。
function fixture(rate = 48000) {
  let Type;
  const messages = [];
  vm.runInNewContext(fs.readFileSync(path.join(__dirname, '../adapters/desktop/web/call-worklet.js'), 'utf8'), {
    sampleRate: rate, registerProcessor: (_, value) => { Type = value; },
    AudioWorkletProcessor: class { constructor() { this.port = {postMessage: value => messages.push(value)}; } }
  });
  const processor = new Type();
  const command = value => processor.port.onmessage({data: value});
  const block = (value = .125) => {
    const input = new Float32Array(128).fill(value);
    const output = new Float32Array(128).fill(.5);
    const running = processor.process([[input]], [[output]]);
    return {peak: Math.max(...output.map(Math.abs)), running};
  };
  return {command, block, messages, processor};
}
// mask 提交原票据，本函数不模拟音频块或提前回执。
function mask(fixtureValue, ticket, enabled, peerEpoch = '1') {
  fixtureValue.command({kind: 'callAudioMask', ticket: String(ticket), peerEpoch, enabled});
}
test('default silent, actual block required and stereo-compatible source passes only after gate', () => {
  const value = fixture();
  assert.equal(value.block().peak, 0);
  mask(value, 1, false, '0'); assert.equal(value.messages.length, 0);
  assert.equal(value.block().peak, 0); assert.equal(value.messages.at(-1).enabled, false);
  mask(value, 2, true); assert.equal(value.messages.length, 1);
  assert.equal(value.block().peak, .125); assert.equal(value.messages.at(-1).ticket, '2');
  mask(value, 3, false); assert.equal(value.block().peak, 0);
  assert.equal(value.messages.at(-1).enabled, false);
});
test('close and abort silence actual block and never confirm canceled opening', () => {
  for (const close of [false, true]) {
    const value = fixture(); mask(value, 1, true);
    value.command(close ? {kind: 'callAudioClose'} : {kind: 'callAudioAbort', ticket: '1'});
    const block = value.block(); assert.equal(block.peak, 0); assert.equal(block.running, !close);
    assert.equal(value.messages.filter(message => message.kind === 'callAudioGated').length, 0);
    mask(value, 2, true); assert.equal(value.block().peak, 0);
  }
});
test('empty outputs cannot confirm or count any actual output block', () => {
  const value = fixture(); mask(value, 1, false, '0');
  for (const outputs of [[], [[]], [[new Float32Array(0)]]]) {
    assert.equal(value.processor.process([], outputs), true);
    assert.equal(value.messages.length, 0);
    assert.equal(value.processor.zeroBlocks, 0);
    assert.equal(value.processor.nonSilentBlocks, 0);
  }
  assert.equal(value.block().peak, 0);
  assert.equal(value.messages.at(-1).kind, 'callAudioGated');
});
test('old epoch, wrong ticket, pending overwrite, malformed data and non-finite PCM fail closed', () => {
  for (const invalid of [
    {kind: 'callAudioMask', ticket: '01', peerEpoch: '1', enabled: true},
    {kind: 'callAudioMask', ticket: '18446744073709551616', peerEpoch: '1', enabled: true},
    {kind: 'callAudioMask', ticket: '1', peerEpoch: '1', enabled: null},
    {kind: 'callAudioMask', ticket: '1', peerEpoch: '1', enabled: true, extra: 'private'},
  ]) { const value = fixture(); value.command(invalid); assert.equal(value.block().peak, 0); }
  const old = fixture(); mask(old, 1, false, '2'); old.block(); mask(old, 2, true, '1');
  assert.equal(old.block().peak, 0);
  const pending = fixture(); mask(pending, 1, false); mask(pending, 2, true);
  assert.equal(pending.block().peak, 0);
  const pcm = fixture(); mask(pcm, 1, true); assert.equal(pcm.block(NaN).peak, 0);
  assert.equal(pcm.messages.filter(value => value.kind === 'callAudioGated').length, 0);
  const wrongRate = fixture(16000); mask(wrongRate, 1, true); assert.equal(wrongRate.block().peak, 0);
});

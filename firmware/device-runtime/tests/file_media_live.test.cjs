// 真实媒体验收入口的语料回归；只验证输入，不将语料有效性当作实际播放验收。
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {waveform, Probe} = require('./file_media_live.cjs');

// focusHarness只验证验收器原请求/原事实校验，不冒称真实C++或后端已执行。
function focusHarness(resources, alter = value => value) {
  const probe = Object.create(Probe.prototype);
  Object.assign(probe, {sessionId: 'fixture-session', sequence: 2, focusRevision: 0, focusResources: resources});
  probe.child = {stdin: {write(line) { probe.request = JSON.parse(line); }}};
  probe.wait = async predicate => {
    const request = probe.request;
    const reply = alter({msgToken: request.msgToken, method: 'focusState', sessionId: request.sessionId,
      code: 200, data: {focusId: 'probe-focus', resources,
        revision: probe.focusRevision + 1,
        state: {acquire: 'acquired', suspend: 'suspended', restore: 'restored', release: 'released'}[request.params.operation]}});
    assert.equal(predicate(reply), true);
    return reply;
  };
  return probe;
}

test('焦点验收器四操作使用完整组和独立修订号，原截止不续期', async () => {
  for (const resources of [['speaker'], ['speaker', 'display']]) {
    const probe = focusHarness(resources);
    for (const operation of ['acquire', 'suspend', 'restore', 'release']) {
      await probe.focusCommand(operation);
      assert.equal(probe.request.params.expectedRevision, probe.focusRevision - 1);
      assert.deepEqual(probe.request.params.resources, resources);
      assert.equal(probe.request.params.expiresAt, ['acquire', 'restore'].includes(operation) ? 120000 : undefined);
      assert.equal(probe.request.eventSeq, probe.sequence);
    }
    assert.equal(probe.focusRevision, 4);
  }
});

test('焦点验收器拒绝失败、错组、错会话和错状态，不推进已确认修订号', async () => {
  for (const change of [value => { value.code = 409; },
    value => { value.data.resources = ['speaker']; },
    value => { value.sessionId = 'other'; },
    value => { value.data.state = 'suspended'; },
    value => { value.data.revision = 2; }]) {
    const probe = focusHarness(['speaker', 'display'], value => { change(value); return value; });
    await assert.rejects(probe.focusCommand('acquire'));
    assert.equal(probe.focusRevision, 0);
  }
});

// WAV语料固定为20秒、48kHz、单声道PCM16，能独立核验长度、音量和周期。
test('真实音频输入具有有效WAV头和非静音440Hz采样', () => {
  const temporary = path.resolve(__dirname, '../../../.temp/device-runtime');
  fs.mkdirSync(temporary, {recursive: true});
  const directory = fs.mkdtempSync(path.join(temporary, 'fixture-wav-'));
  try {
    const filename = path.join(directory, 'tone.wav');
    waveform(filename);
    const bytes = fs.readFileSync(filename);
    assert.equal(bytes.toString('ascii', 0, 4), 'RIFF');
    assert.equal(bytes.toString('ascii', 8, 16), 'WAVEfmt ');
    assert.equal(bytes.readUInt32LE(4), bytes.length - 8);
    assert.equal(bytes.readUInt16LE(20), 1);
    assert.equal(bytes.readUInt16LE(22), 1);
    assert.equal(bytes.readUInt32LE(24), 48000);
    assert.equal(bytes.readUInt32LE(28), 96000);
    assert.equal(bytes.readUInt16LE(32), 2);
    assert.equal(bytes.readUInt16LE(34), 16);
    assert.equal(bytes.toString('ascii', 36, 40), 'data');
    assert.equal(bytes.readUInt32LE(40), 48000 * 20 * 2);
    assert.equal(bytes.length, 44 + 48000 * 20 * 2);
    let energy = 0;
    for (let index = 0; index < 48000; ++index) {
      const actual = bytes.readInt16LE(44 + index * 2);
      // PCM整数没有负零；舍入结果归零后再比较，不能误判有效零交叉点。
      const expected = Math.round(Math.sin(index * 2 * Math.PI * 440 / 48000) * 8192) || 0;
      assert.equal(actual, expected);
      energy += actual * actual;
    }
    assert.ok(Math.sqrt(energy / 48000) > 5700);
  } finally {
    fs.unlinkSync(path.join(directory, 'tone.wav'));
    fs.rmdirSync(directory);
  }
});

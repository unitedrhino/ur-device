// 文件说明：纯文字真实验收器的离线控制面回归；不把合成子进程/回复标为真实API验证。
'use strict';
const {test} = require('node:test');
const assert = require('node:assert/strict');
const {EventEmitter} = require('node:events');
const {PassThrough, Writable} = require('node:stream');
const {cases, run} = require('./text_only_live.cjs');
const env = {UR_TEXT_MODEL_LIVE: '1', UR_DEVICE_TEXT_BIN: '/fixture/device_text',
  UR_DEVICE_PRODUCT: 'fixture', UR_DEVICE_NAME: 'fixture', UR_DEVICE_SECRET: 'fixture',
  UR_MQTT_HOST: '127.0.0.1', UR_MQTT_PORT: '1883'};
// fixture只生成明确合成CLI事件；每次stdin写入严格检查原固定语料和先建立规则。
function fixture(mode = 'normal') {
  const child = new EventEmitter();
  child.stdout = new PassThrough(); child.stderr = new PassThrough();
  let established = false, round = 0, released = false, killed = false;
  const emit = value => child.stdout.write(JSON.stringify(value) + '\n');
  const close = code => {
    if (released) return;
    released = true; child.stdout.end(); child.stderr.end(); child.emit('close', code);
  };
  child.kill = () => { killed = true; queueMicrotask(() => close(143)); return true; };
  child.stdin = new Writable({write(bytes, _, complete) {
    assert.equal(established, true);
    const line = bytes.toString('utf8').trim();
    if (line === '/quit') {
      assert.equal(round, 2); emit({sessionEnded: true, completedRounds: 2});
      complete(); queueMicrotask(() => close(mode === 'badExit' ? 1 : 0)); return;
    }
    assert.equal(line, cases[round].input);
    ++round;
    if (mode === 'rejected') { emit({error: 'inputRejected'}); complete(); return; }
    emit({inputAccepted: true, round});
    // 第二轮开始后的上一轮换行不算首文本；受控延迟只模拟两个管道的独立到达。
    if (mode === 'tailNewline' && round === 2) {
      child.stderr.write('\n');
      setTimeout(() => {
        child.stderr.write(cases[1].keyword);
        emit({replyCompleted: true, round: 2});
      }, 30);
      complete(); return;
    }
    const text = Buffer.from(mode === 'wrongEffect' ? '公开不相关回复' : cases[round - 1].keyword);
    child.stderr.write(text.subarray(0, 2)); child.stderr.write(text.subarray(2));
    emit({replyCompleted: true, round});
    if (mode === 'duplicateTerminal') emit({replyCompleted: true, round});
    complete();
  }});
  queueMicrotask(() => {
    if (mode === 'earlyExit') return close(0);
    if (mode === 'privateDiagnostic') { emit({secret: 'synthetic'}); return; }
    established = true; emit({sessionEstablished: true, transport: 'none'});
  });
  return {child, facts: () => ({round, released, killed})};
}
test('显式开启前不启动进程', async () => {
  let started = false;
  await assert.rejects(run({...env, UR_TEXT_MODEL_LIVE: '0'}, {spawn: () => { started = true; }}));
  assert.equal(started, false);
});
test('同一合成进程两轮保持顺序并正确合并跨块中文，报告不含正文', async () => {
  const f = fixture(); let count = 0;
  const result = await run(env, {spawn: (binary, args) => {
    ++count; assert.equal(binary, env.UR_DEVICE_TEXT_BIN); assert.deepEqual(args, ['--tui']); return f.child;
  }, timeoutMs: 1000});
  assert.equal(count, 1); assert.equal(result.completedRounds, 2);
  assert.equal(result.evidence, 'offline-control-fixture');
  assert.equal(result.ownedProcessReleased, true);
  assert.ok(result.rounds.every(value => value.keywordMatched && value.firstTextMs >= 0));
  assert.ok(cases.every(item => !JSON.stringify(result).includes(item.keyword)));
  assert.deepEqual(f.facts(), {round: 2, released: true, killed: false});
});
for (const mode of ['rejected', 'earlyExit', 'privateDiagnostic', 'badExit', 'duplicateTerminal']) {
  test('拒绝' + mode + '且只释放原子进程', async () => {
    const f = fixture(mode);
    await assert.rejects(run(env, {spawn: () => f.child, timeoutMs: 1000}));
    assert.equal(f.facts().released, true);
  });
}
test('握手与完成回执不能代替实际文本效果，失败仍精确释放并脱敏', async () => {
  const f = fixture('wrongEffect');
  await assert.rejects(run(env, {spawn: () => f.child, timeoutMs: 100}), error => {
    assert.equal(error.facts.phase, 'replyEffect');
    assert.equal(error.facts.completedRounds, 0);
    assert.equal(error.facts.ownedProcessReleased, true);
    assert.equal(error.facts.evidence, 'offline-control-fixture');
    assert.ok(!JSON.stringify(error.facts).includes('公开不相关回复'));
    return true;
  });
});
test('第二轮迟到换行不能被统计成首文本', async () => {
  const f = fixture('tailNewline');
  const result = await run(env, {spawn: () => f.child, timeoutMs: 1000});
  assert.ok(result.rounds[1].firstTextMs >= 15, 'late_newline_was_counted_as_reply_text');
  assert.equal(result.ownedProcessReleased, true);
});
test('进程只读观察只接原spawn对象，不替换或重新启动设备', async () => {
  const f = fixture(); let observed = 0;
  await run(env, {spawn: () => f.child, timeoutMs: 1000, onSpawn: child => {
    assert.equal(child, f.child); ++observed;
  }});
  assert.equal(observed, 1);
  assert.equal(f.facts().released, true);
});
test('进程观察失败仍释放原进程且不回显自由错误', async () => {
  const f = fixture();
  await assert.rejects(run(env, {spawn: () => f.child, timeoutMs: 1000, onSpawn: () => {
    throw Error('synthetic-private-observer');
  }}), error => {
    assert.equal(error.facts.phase, 'processObservation');
    assert.equal(error.facts.ownedProcessReleased, true);
    assert.ok(!JSON.stringify(error.facts).includes('synthetic-private-observer'));
    return true;
  });
});

// 文件说明：none纯文字客户端工具验收驱动器的离线控制面回归；合成子进程不构成设备、模型或真实E2E验证。
'use strict';
const {test} = require('node:test');
const assert = require('node:assert/strict');
const {EventEmitter} = require('node:events');
const {PassThrough, Writable} = require('node:stream');
const live = require('./none_client_tool_live.cjs');
const {rounds, cancelRound, run} = live;
const env = {UR_NONE_CLIENT_TOOL_LIVE: '1', UR_DEVICE_TEXT_BIN: '/fixture/device_text',
  UR_DEVICE_PRODUCT: 'fixture', UR_DEVICE_NAME: 'fixture', UR_DEVICE_SECRET: 'fixture',
  UR_MQTT_HOST: '127.0.0.1', UR_MQTT_PORT: '1883'};
// fixture只生成明确合成CLI事实；每次stdin写入严格检查原固定语料、取消命令和先建立规则。
function fixture(mode = 'normal') {
  const child = new EventEmitter();
  child.stdout = new PassThrough(); child.stderr = new PassThrough();
  let established = false, round = 0, released = false, killed = false;
  const emit = value => child.stdout.write(JSON.stringify(value) + '\n');
  const keyword = index => Buffer.from(live.rounds[index].keyword);
  const close = code => {
    if (released) return;
    released = true; child.stdout.end(); child.stderr.end(); child.emit('close', code);
  };
  child.kill = () => { killed = true; queueMicrotask(() => close(143)); return true; };
  child.stdin = new Writable({write(bytes, _, complete) {
    assert.equal(established, true);
    const line = bytes.toString('utf8').trim();
    if (line === '/quit') {
      assert.equal(round, 3); emit({sessionEnded: true, completedRounds: mode === 'wrongCount' ? 3 : 2});
      complete(); queueMicrotask(() => close(mode === 'badExit' ? 1 : 0)); return;
    }
    if (line === '/cancel') {
      assert.equal(round, 3);
      if (mode === 'cancelMissed') { complete(); return; } // 取消落空时不发任何事实，驱动只能超时。
      emit({cancelRequested: true});
      if (mode === 'cancelAfterAccepted')
        emit({clientToolSubmitted: true, code: 200, continuation: 'accepted', receiptIndex: 3});
      else if (mode !== 'submitSuppressed')
        emit({clientToolSubmitted: true, code: 409, continuation: 'unknown', receiptIndex: 3});
      if (mode === 'duplicate409')
        emit({clientToolSubmitted: true, code: 409, continuation: 'unknown', receiptIndex: 3});
      emit({replyCancelled: true});
      if (mode === 'revived') { // 取消生效后的执行与完成都是复活，必须失败。
        emit({clientToolStarted: true, round: 3, theme: 'dark'});
        emit({replyCompleted: true, round: 3});
      }
      complete(); return;
    }
    if (round < 2) {
      assert.equal(line, live.rounds[round].input);
      ++round;
      if (mode === 'rejected') { emit({error: 'inputRejected'}); complete(); return; }
      emit({inputAccepted: true, round});
      emit({clientToolStarted: true, round,
        theme: round === 2 && mode === 'wrongTheme' ? 'dark' : live.rounds[round - 1].theme});
      if (mode !== 'noSubmit')
        emit({clientToolSubmitted: true, code: 200,
          continuation: round === 1 && mode === 'unknownContinuation' ? 'unknown' : 'accepted',
          receiptIndex: round});
      const reply = Buffer.from(mode === 'wrongEffect' ? '公开不相关回复' : live.rounds[round - 1].keyword);
      child.stderr.write(reply.subarray(0, 2)); child.stderr.write(reply.subarray(2));
      emit({replyCompleted: true, round});
      complete(); return;
    }
    assert.equal(line, live.cancelRound.input);
    ++round;
    emit({inputAccepted: true, round});
    emit({clientToolStarted: true, round, theme: 'dark'});
    if (mode === 'cancelMissed') emit({replyCompleted: true, round}); // 提交前回复已完成，随后取消必然落空。
    complete();
  }});
  queueMicrotask(() => {
    if (mode === 'earlyExit') return close(0);
    if (mode === 'privateDiagnostic') { emit({secret: 'synthetic'}); return; }
    established = true; emit({sessionEstablished: true, transport: 'none'});
    if (mode !== 'noNegotiation') emit({clientToolsNegotiated: true});
  });
  return {child, facts: () => ({round, released, killed})};
}
test('显式开启前不启动进程', async () => {
  let started = false;
  await assert.rejects(run({...env, UR_NONE_CLIENT_TOOL_LIVE: '0'}, {spawn: () => { started = true; }}));
  assert.equal(started, false);
});
test('同一合成进程两轮主题切换与取消轮保持顺序，报告不含正文与设备身份', async () => {
  const f = fixture(); let count = 0;
  const result = await run(env, {spawn: (binary, args) => {
    ++count; assert.equal(binary, env.UR_DEVICE_TEXT_BIN); assert.deepEqual(args, ['--tui']); return f.child;
  }, timeoutMs: 1000});
  assert.equal(count, 1); assert.equal(result.completedRounds, 2); assert.equal(result.cancelledRounds, 1);
  assert.equal(result.cancelOutcome, 'expiredReject');
  assert.equal(result.acceptedContinuations, 2); assert.equal(result.expiredReceipts, 1);
  assert.deepEqual(result.rounds.map(value => [value.round, value.theme, value.submittedCode]),
    [[1, 'dark', 200], [2, 'light', 200]]);
  assert.equal(result.evidence, 'offline-control-fixture');
  assert.equal(result.ownedProcessReleased, true); assert.equal(result.hardwareVerified, false);
  for (const privateText of [live.rounds[0].keyword, live.rounds[1].keyword, live.cancelRound.input])
    assert.ok(!JSON.stringify(result).includes(privateText));
  assert.deepEqual(f.facts(), {round: 3, released: true, killed: false});
});
test('取消轮三种合规结局各得其所，accepted计数只反映实际明确续答', async () => {
  const expected = {normal: 'expiredReject', cancelAfterAccepted: 'cancelAfterAccepted',
    submitSuppressed: 'submitSuppressed'};
  for (const [mode, outcome] of Object.entries(expected)) {
    const f = fixture(mode);
    const result = await run(env, {spawn: () => f.child, timeoutMs: 1000});
    assert.equal(result.cancelOutcome, outcome);
    assert.equal(result.expiredReceipts, outcome === 'expiredReject' ? 1 : 0);
    assert.equal(result.acceptedContinuations, outcome === 'cancelAfterAccepted' ? 3 : 2);
    assert.equal(result.completedRounds, 2);
    assert.deepEqual(f.facts(), {round: 3, released: true, killed: false});
  }
});
test('未协商工具能力不得进入任何输入轮次', async () => {
  const f = fixture('noNegotiation');
  await assert.rejects(run(env, {spawn: () => f.child, timeoutMs: 400}), error => {
    assert.equal(error.facts.phase, 'sessionEstablishment');
    assert.equal(error.facts.completedRounds, 0); assert.equal(error.facts.ownedProcessReleased, true);
    return true;
  });
  // 失败样本由驱动SIGTERM清理；kill受理不冒充close，released只看真实end+close。
  assert.deepEqual(f.facts(), {round: 0, released: true, killed: true});
});
// 事实处理同步先行于驱动阶段推进：fixture一次emit的全部事实先过handler校验，失败观察点停在write后的当前阶段。
for (const [mode, phase] of [['wrongTheme', 'inputAcceptance'], ['noSubmit', 'toolSubmission'],
  ['unknownContinuation', 'toolSubmission'], ['cancelMissed', 'cancelRequest'],
  ['duplicate409', 'cancelRequest'], ['revived', 'cancelRequest'], ['wrongEffect', 'replyEffect'],
  ['wrongCount', 'sessionClose']]) {
  test('拒绝' + mode + '且只释放原子进程，不回显正文', async () => {
    const f = fixture(mode);
    await assert.rejects(run(env, {spawn: () => f.child, timeoutMs: 400}), error => {
      assert.equal(error.facts.phase, phase);
      assert.equal(error.facts.ownedProcessReleased, true);
      assert.equal(error.facts.evidence, 'offline-control-fixture');
      assert.ok(!JSON.stringify(error.facts).includes('公开不相关回复'));
      return true;
    });
    assert.equal(f.facts().released, true);
  });
}
for (const mode of ['rejected', 'earlyExit', 'privateDiagnostic', 'badExit']) {
  test('拒绝' + mode + '且只释放原子进程', async () => {
    const f = fixture(mode);
    await assert.rejects(run(env, {spawn: () => f.child, timeoutMs: 400}));
    assert.equal(f.facts().released, true);
  });
}
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
